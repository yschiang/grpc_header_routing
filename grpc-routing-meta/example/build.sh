#!/usr/bin/env bash
# Manual build mirroring CMakeLists.txt (used because cmake is not installed here).
# List-driven: contract protos -> plugin -> per-system codegen -> apps + test.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

PROTOC="${PROTOC:-protoc}"

CXX="${CXX:-c++}"
CXXFLAGS="-std=c++17 -O2 -Wall"

# Protobuf toolchain discovered via pkg-config (stock Linux: protobuf.pc is on the
# default path; conda dev host: export PKG_CONFIG_PATH=<prefix>/lib/pkgconfig).
pkg-config --exists protobuf || {
  echo "protobuf not found via pkg-config: install libprotobuf-dev / protobuf-devel, or set PKG_CONFIG_PATH=<prefix>/lib/pkgconfig" >&2
  exit 1
}
PB_CFLAGS="$(pkg-config --cflags protobuf)"      # -I<include>
PB_LDIRS="$(pkg-config --libs-only-L protobuf)"  # -L<lib>
PB_LIBDIR="$(pkg-config --variable=libdir protobuf)"

GEN="$ROOT/build/generated"
BIN="$ROOT/build"

# -I dirs must be a textual prefix of the file args below, so keep them relative
# (we cd'd into $ROOT) to match "proto/x.proto". All protos live in proto/.
IPROTO=(-I proto $PB_CFLAGS)
PBFLAGS=(-I "$GEN" -I "$ROOT/src" $PB_CFLAGS $PB_LDIRS -lprotobuf -Wl,-rpath,"$PB_LIBDIR")

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
  $PB_LDIRS -lprotoc -lprotobuf -Wl,-rpath,"$PB_LIBDIR" \
  -o "$BIN/protoc-gen-meta"

# 2b. negative codegen: every fixture under tests/negative/ MUST be rejected by the
#     plugin's Validate pass (the "fail loud, never silent" contract). protoc returns
#     non-zero when the plugin does -> we assert that here.
echo "[neg ] codegen must reject malformed (routing.project)"
neg_ok=1
for f in tests/negative/*.proto; do
  if "$PROTOC" "${IPROTO[@]}" -I tests/negative \
       --plugin=protoc-gen-meta="$BIN/protoc-gen-meta" \
       --meta_out="$GEN" "$f" >/dev/null 2>&1; then
    echo "       FAIL: $f was accepted but must be rejected"; neg_ok=0
  else
    echo "       ok (rejected): $(basename "$f")"
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
$CXX $CXXFLAGS tests/test_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/test_projection"
echo "[bench] bench_projection"
$CXX $CXXFLAGS tests/bench_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/bench_projection"

echo "OK -> binaries in $BIN/"
