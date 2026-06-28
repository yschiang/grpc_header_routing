#!/usr/bin/env bash
# Line-coverage gate for the RUNTIME kit (src/common). Run AFTER build.sh — it
# reuses build/generated/*.pb.cc + *.proj.cc. The codegen plugin (protoc-gen-meta)
# is gated separately by build.sh's negative-codegen fixtures, so it is out of
# scope here; this measures the library the wire path actually executes.
#
#   COV_THRESHOLD  minimum line %, default 80.  CXX  compiler, default c++.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"

CXX="${CXX:-c++}"
THRESHOLD="${COV_THRESHOLD:-80}"

pkg-config --exists protobuf || { echo "protobuf not found (set PKG_CONFIG_PATH)" >&2; exit 1; }
PB_INC="$(pkg-config --variable=includedir protobuf)"
PB_LIB="$(pkg-config --variable=libdir protobuf)"
[ -d build/generated ] || { echo "run ./build.sh first (build/generated missing)" >&2; exit 1; }

CF="--coverage -O0 -g -std=c++17 -I build/generated -I src -I $PB_INC"
rm -rf build/cov && mkdir -p build/cov

# Compile generated TUs + the three drivers that exercise the kit, then link and
# run each so .gcda accumulate; gcovr merges header coverage across the drivers.
for s in build/generated/*.pb.cc build/generated/*.proj.cc \
         tests/test_projection.cc sender/unified_sender.cc receiver/receiver_verify.cc; do
  $CXX $CF -c "$s" -o "build/cov/$(basename "$s").o"
done
GEN_OBJS=$(ls build/cov/*.pb.cc.o build/cov/*.proj.cc.o)
for m in test_projection unified_sender receiver_verify; do
  $CXX --coverage "build/cov/$m.cc.o" $GEN_OBJS -L "$PB_LIB" -lprotobuf -Wl,-rpath,"$PB_LIB" -o "build/cov/$m"
  "./build/cov/$m" >/dev/null
done

# Default gcov reads both gcc and (Apple-)clang .gcda; filter to the hand-written
# kit only — generated TUs and tests are excluded from the measured surface.
gcovr --root . --filter 'src/common/' --exclude-unreachable-branches \
  --fail-under-line "$THRESHOLD" --print-summary build/cov
