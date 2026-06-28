#!/usr/bin/env bash
# =============================================================================
# Real-wire gRPC demo + self-check (Story 2.1). Builds client+server with gRPC
# ENABLED, sends real traffic carrying the projected routing-meta, and ASSERTS the
# good case verifies (ACCEPT) and the tampered case is rejected (REJECT). Exits
# non-zero on regression OR if the gRPC toolchain is missing — fail loud, never a
# fake pass (BRIEF criterion C).
#
# Toolchain is discovered, not hardcoded (NFR1): set PROTOC / CXX / PKG_CONFIG_PATH
# as for build.sh. Conda dev host:
#   PROTOC=$CONDA/bin/protoc PKG_CONFIG_PATH=$CONDA/lib/pkgconfig \
#   DYLD_LIBRARY_PATH=$CONDA/lib CXX=clang++ ./run.sh
# =============================================================================
set -euo pipefail   # -e: a failed build.sh/protoc/compile aborts — never run stale binaries
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"   # grpc-routing-meta/example
cd "$ROOT"

PROTOC="${PROTOC:-protoc}"
CXX="${CXX:-c++}"
PORT="${PORT:-50551}"
ADDR="127.0.0.1:$PORT"
GEN="build/generated"
BIN="build"

# 1) toolchain probe — fail loud
pkg-config --exists grpc++ || {
  echo "grpc++ not found via pkg-config: install libgrpc++-dev, or (conda) export PKG_CONFIG_PATH=<prefix>/lib/pkgconfig" >&2; exit 1; }
pkg-config --exists protobuf || { echo "protobuf not found; set PKG_CONFIG_PATH" >&2; exit 1; }
PLUGIN="${GRPC_CPP_PLUGIN:-$(command -v grpc_cpp_plugin || true)}"
[ -n "$PLUGIN" ] || { echo "grpc_cpp_plugin not found (ships with libgrpc++-dev / grpc)" >&2; exit 1; }
PB_INC="$(pkg-config --variable=includedir protobuf)"
echo "== grpc++ $(pkg-config --modversion grpc++) | $("$PROTOC" --version) =="

# 2) ensure the kit's message + projection code exists (build.sh produces it)
[ -f "$GEN/sys1.proj.h" ] || { echo "== build.sh (generating kit code) =="; ./build.sh >/dev/null; }

# 3) generate gRPC service stubs for the demo's two services
echo "[gen ] grpc stubs: sys1, sys3"
"$PROTOC" -I proto --grpc_out="$GEN" --plugin=protoc-gen-grpc="$PLUGIN" proto/sys1.proto proto/sys3.proto

# 4) compile server + client (gRPC ON), reusing the kit's generated message + proj TUs
SRCS="$GEN/metadata_options.pb.cc $GEN/process_context.pb.cc \
      $GEN/sys1.pb.cc $GEN/sys1.proj.cc $GEN/sys1.grpc.pb.cc \
      $GEN/sys3.pb.cc $GEN/sys3.proj.cc $GEN/sys3.grpc.pb.cc"
GRPC_FLAGS="$(pkg-config --cflags --libs grpc++ protobuf)"
# Bake rpaths so the binaries find libgrpc++/libprotobuf at runtime without
# DYLD/LD_LIBRARY_PATH (macOS SIP strips DYLD_* for system-bash-spawned procs);
# mirrors build.sh's -Wl,-rpath for protobuf.
RPATHS="-Wl,-rpath,$(pkg-config --variable=libdir grpc++) -Wl,-rpath,$(pkg-config --variable=libdir protobuf)"
rm -f "$BIN/grpc_server" "$BIN/grpc_client"  # belt-and-braces: no stale binary survives a failed compile
for app in grpc_server grpc_client; do
  echo "[cc  ] $app"
  # shellcheck disable=SC2086
  # -Wno-deprecated-declarations silences gRPC 1.46's own headers (deprecated
  # std::iterator under C++17) — third-party noise, not demo code.
  $CXX -std=c++17 -O2 -Wall -Wno-deprecated-declarations -DROUTINGMETA_WITH_GRPC \
    -I "$GEN" -I src -I "$PB_INC" "demo/$app.cc" $SRCS $GRPC_FLAGS $RPATHS -o "$BIN/$app"
done

# 5) run: server backgrounded, wait for listen (bounded, no fixed sleep), client, teardown
SRV_LOG="$(mktemp)"; CLI_LOG="$(mktemp)"
"./$BIN/grpc_server" "$ADDR" >"$SRV_LOG" 2>&1 &
SRV_PID=$!
# `|| true` on kill/wait so the trap can't trip `set -e` (wait on a SIGTERM'd server
# returns 143) and clobber the script's real exit code.
trap 'kill "$SRV_PID" 2>/dev/null || true; wait "$SRV_PID" 2>/dev/null || true; rm -f "$SRV_LOG" "$CLI_LOG"' EXIT
# Readiness via the server's own LISTENING line (portable; no /dev/tcp) and bail if it
# died first — a port already in use makes BuildAndStart return null and the server exit,
# so we must NOT mistake a foreign listener for ours.
for _ in $(seq 1 50); do
  kill -0 "$SRV_PID" 2>/dev/null || { echo "server exited before listening (port $PORT in use / bind failed):" >&2; cat "$SRV_LOG" >&2; exit 1; }
  grep -q "LISTENING on" "$SRV_LOG" && break
  sleep 0.1
done
grep -q "LISTENING on" "$SRV_LOG" || { echo "server did not report LISTENING within timeout" >&2; cat "$SRV_LOG" >&2; exit 1; }

"./$BIN/grpc_client" "$ADDR" >"$CLI_LOG" 2>&1 || true
echo "------------------ server ------------------"; cat "$SRV_LOG"
echo "------------------ client ------------------"; cat "$CLI_LOG"

# 6) self-check oracle: good case ACCEPTed AND tamper case REJECTed
ok=1
grep -qE "digest check: OK.*-> ACCEPT" "$SRV_LOG" || { echo "FAIL: good case did not ACCEPT"; ok=0; }
grep -qE "digest MISMATCH.*-> REJECT"  "$SRV_LOG" || { echo "FAIL: tamper case did not REJECT"; ok=0; }
grep -q  "REJECTED:"                    "$CLI_LOG" || { echo "FAIL: client did not observe the rejection"; ok=0; }
if [ "$ok" = 1 ]; then
  echo "DEMO PASSED — real-wire projection verified; projected-header drift rejected"
else
  echo "DEMO FAILED"; exit 1
fi
