#!/usr/bin/env bash
# Manual build mirroring CMakeLists.txt (used because cmake is not installed here).
# List-driven: contract protos -> plugin -> per-system codegen -> apps + test.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Toolchain discovery — no hardcoded paths (criterion A). Override with env if needed:
#   PROTOC=/path/to/protoc CXX=clang++ ./build.sh
PROTOC="${PROTOC:-protoc}"
command -v "$PROTOC" >/dev/null 2>&1 || { echo "error: protoc not found (set PROTOC=...)"; exit 1; }

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists protobuf; then
  PB_INC="$(pkg-config --variable=includedir protobuf)"
  PB_CFLAGS="$(pkg-config --cflags protobuf)"
  PB_LIBS="$(pkg-config --libs protobuf)"
else
  # Derive <prefix> from <prefix>/bin/protoc (works for system, Homebrew, conda).
  PREFIX="$(cd "$(dirname "$(command -v "$PROTOC")")/.." && pwd)"
  PB_INC="$PREFIX/include"
  PB_CFLAGS="-I$PB_INC"
  PB_LIBS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib -lprotobuf"
fi

CXX="${CXX:-c++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -Wall}"
GEN="$ROOT/build/generated"
BIN="$ROOT/build"

# -I dirs must be a textual prefix of the file args below, so keep proto/ relative.
IPROTO=(-I proto -I "$PB_INC")
PBFLAGS=(-I "$GEN" -I "$ROOT/src" $PB_CFLAGS $PB_LIBS)

# Contract protos: --cpp_out only (no ProjectMeta). Add a new system by appending to
# SYSTEMS; add a shared message by appending to CONTRACT.
CONTRACT=(metadata_options process_context)
SYSTEMS=(sys1 sys2 sys3)

rm -rf "$BIN"
mkdir -p "$GEN"
echo "== protoc $("$PROTOC" --version) =="

# 1. contract protos -> C++
for n in "${CONTRACT[@]}"; do
  echo "[gen ] $n.proto (cpp)"
  "$PROTOC" "${IPROTO[@]}" --cpp_out="$GEN" "proto/$n.proto"
done

# 2. build the plugin (links libprotoc + libprotobuf + compiled options)
echo "[plug] protoc-gen-meta"
$CXX $CXXFLAGS \
  src/plugin/protoc-gen-meta.cc "$GEN/metadata_options.pb.cc" \
  -I "$GEN" $PB_CFLAGS \
  -lprotoc $PB_LIBS \
  -o "$BIN/protoc-gen-meta"

# 2b. negative codegen: every fixture under tests/negative/ MUST be rejected by the
#     plugin's Validate pass (the "fail loud, never silent" contract) AND for the
#     RIGHT reason. A fixture rejected by accident (a typo, a bad import) would still
#     be non-zero and pass a mere "did it fail?" check — so we capture stderr and
#     assert the expected diagnostic substring per fixture.
echo "[neg ] codegen must reject malformed (routing.project) — for the right reason"
neg_ok=1
for f in tests/negative/*.proto; do
  base="$(basename "$f")"
  case "$base" in
    bad_dup_key.proto)                want='duplicate (routing.project) key' ;;
    bad_message_project.proto)        want='message field cannot project' ;;
    bad_project_under_repeated.proto) want='is set under repeated field' ;;
    bad_repeated_scalar.proto)        want='repeated field cannot project' ;;
    *)                                want='' ;;
  esac
  err="$("$PROTOC" "${IPROTO[@]}" -I tests/negative \
         --plugin=protoc-gen-meta="$BIN/protoc-gen-meta" \
         --meta_out="$GEN" "$f" 2>&1 1>/dev/null)" && {
    echo "       FAIL: $base was accepted but must be rejected"; neg_ok=0; continue; }
  if [ -z "$want" ]; then
    echo "       WARN: $base rejected, but no expected reason mapped (reason unchecked)"
  elif printf '%s' "$err" | grep -qF -- "$want"; then
    echo "       ok (rejected for \"$want\"): $base"
  else
    echo "       FAIL: $base rejected, but NOT for the expected reason \"$want\""
    echo "             got: $err"; neg_ok=0
  fi
done
[ "$neg_ok" = 1 ] || { echo "negative codegen check FAILED"; exit 1; }

# 3. system protos -> messages (--cpp_out) + ProjectMeta (--meta_out)
GEN_SRCS=("$GEN/metadata_options.pb.cc" "$GEN/process_context.pb.cc")
for n in "${SYSTEMS[@]}"; do
  echo "[gen ] $n.proto (cpp + meta)"
  "$PROTOC" "${IPROTO[@]}" --cpp_out="$GEN" "proto/$n.proto"
  "$PROTOC" "${IPROTO[@]}" \
    --plugin=protoc-gen-meta="$BIN/protoc-gen-meta" \
    --meta_out="$GEN" "proto/$n.proto"
  GEN_SRCS+=("$GEN/$n.pb.cc" "$GEN/$n.proj.cc")
done

# 4. apps + test (link the full generated set, each .pb.cc exactly once)
echo "[app ] unified_sender"
$CXX $CXXFLAGS sender/unified_sender.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/unified_sender"
echo "[app ] receiver_verify"
$CXX $CXXFLAGS receiver/receiver_verify.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/receiver_verify"
echo "[test] test_projection"
$CXX $CXXFLAGS -pthread tests/test_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/test_projection"
echo "[bench] bench_projection"
$CXX $CXXFLAGS tests/bench_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/bench_projection"

# 5. run the green gate. The send-time digest toggle is exercised in-process by
#    test_projection (ProjectMeta(req, sink, false)); no second build is needed.
echo "[test] run test_projection"
"$BIN/test_projection"
echo "[recv] run receiver_verify"
"$BIN/receiver_verify" >/dev/null

echo "OK -> binaries in $BIN/ (digest on + off both covered)"
