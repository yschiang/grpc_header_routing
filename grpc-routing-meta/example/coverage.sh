#!/usr/bin/env bash
# Line coverage for the hand-written kit (src/). Generated *.proj.cc / *.pb.cc are
# EXCLUDED from the gate — their correctness is guarded by the negative-codegen gate
# (build.sh) and the round-trip verify, not line counts. Mirrors build.sh conventions.
#
#   ./coverage.sh                 # gate src/ at >= $THRESHOLD (default 80)
#   THRESHOLD=90 ./coverage.sh
#   CXX=g++ ./coverage.sh         # CI: gcc -> gcov; clang -> llvm-cov gcov (auto)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"

THRESHOLD="${THRESHOLD:-80}"
PROTOC="${PROTOC:-protoc}"
CXX="${CXX:-c++}"
GEN=build/generated

# protobuf flags — same discovery as build.sh.
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists protobuf; then
  PB_CFLAGS="$(pkg-config --cflags protobuf)"; PB_LIBS="$(pkg-config --libs protobuf)"
else
  PREFIX="$(cd "$(dirname "$(command -v "$PROTOC")")/.." && pwd)"
  PB_CFLAGS="-I$PREFIX/include"; PB_LIBS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib -lprotobuf"
fi

# gcov reader must match the compiler: gcc -> gcov, clang -> llvm-cov gcov.
if [ -z "${GCOV:-}" ]; then
  if "$CXX" --version 2>/dev/null | grep -qi clang; then
    if command -v llvm-cov >/dev/null 2>&1; then GCOV="llvm-cov gcov"
    elif xcrun --find llvm-cov >/dev/null 2>&1; then GCOV="$(xcrun --find llvm-cov) gcov"
    else echo "error: clang build needs llvm-cov (set GCOV=...)"; exit 1; fi
  else
    GCOV="gcov"
  fi
fi

# Need the generated sources; build.sh produces them (and runs the negative gate).
[ -d "$GEN" ] || ./build.sh >/dev/null

CXXF="-std=c++17 -O0 -g --coverage -I $GEN -I src $PB_CFLAGS"
rm -rf build/cov && mkdir -p build/cov

# Shared instrumented generated objects, hit by all three drivers so gcda accumulates.
GENSRCS="$GEN/metadata_options.pb.cc $GEN/process_context.pb.cc \
$GEN/sys1.pb.cc $GEN/sys1.proj.cc $GEN/sys2.pb.cc $GEN/sys2.proj.cc $GEN/sys3.pb.cc $GEN/sys3.proj.cc"
GENOBJS=""
for s in $GENSRCS; do o="build/cov/$(basename "$s" .cc).o"; $CXX $CXXF -c "$s" -o "$o"; GENOBJS="$GENOBJS $o"; done

$CXX $CXXF -pthread -c tests/test_projection.cc -o build/cov/test_projection.o
$CXX $CXXF          -c sender/unified_sender.cc   -o build/cov/unified_sender.o
$CXX $CXXF          -c receiver/receiver_verify.cc -o build/cov/receiver_verify.o

$CXX --coverage -pthread build/cov/test_projection.o $GENOBJS $PB_LIBS -o build/cov/test_cov
$CXX --coverage          build/cov/unified_sender.o  $GENOBJS $PB_LIBS -o build/cov/sender_cov
$CXX --coverage          build/cov/receiver_verify.o $GENOBJS $PB_LIBS -o build/cov/receiver_cov

./build/cov/test_cov   >/dev/null
./build/cov/sender_cov >/dev/null
./build/cov/receiver_cov >/dev/null

echo "== coverage (info: src/ + generated projection) =="
gcovr -r . --gcov-executable "$GCOV" --gcov-ignore-parse-errors \
  --filter 'src/' --filter 'build/generated/.*\.proj\.cc' 2>/dev/null
gcovr -r . --gcov-executable "$GCOV" --gcov-ignore-parse-errors \
  --filter 'src/' --filter 'build/generated/.*\.proj\.cc' \
  --html-details -o build/cov/coverage.html 2>/dev/null || true
echo "HTML -> build/cov/coverage.html"

echo "== GATE: hand-written src/ must be >= ${THRESHOLD}% lines =="
gcovr -r . --gcov-executable "$GCOV" --gcov-ignore-parse-errors \
  --filter 'src/' --fail-under-line "$THRESHOLD" 2>/dev/null
echo "PASS: src/ line coverage >= ${THRESHOLD}%"