#!/usr/bin/env bash
# Real gRPC round-trip demo for sys1. Reuses the kit's generated *.pb.cc / *.proj.cc
# (run ../build.sh first), generates the gRPC stub, builds a server + client, and
# runs one clean call (verifies) and one tampered call (rejected) over HTTP/2.
#
# Needs a local grpc++ + grpc_cpp_plugin. Point GRPC_PREFIX at it (default: anaconda).
set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"; EXAMPLE="$(cd .. && pwd)"
GEN="$EXAMPLE/build/generated"
[ -f "$GEN/sys1.proj.cc" ] || { echo "run ../build.sh first (need $GEN)"; exit 1; }

PREFIX="${GRPC_PREFIX:-$HOME/anaconda3}"
PROTOC="$PREFIX/bin/protoc"
GRPC_PLUGIN="$PREFIX/bin/grpc_cpp_plugin"
CXX="${CXX:-clang++}"
PKG="PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config"
GFLAGS="$(eval "$PKG --cflags grpc++ protobuf")"
GLIBS="$(eval "$PKG --libs grpc++ protobuf")"

DGEN="$HERE/gen"; BIN="$HERE/bin"; mkdir -p "$DGEN" "$BIN"
echo "== grpc++ $(eval "$PKG --modversion grpc++") via $PREFIX =="

# gRPC service stubs for all three systems — messages come from the kit's *.pb.cc.
GRPC_SRCS=""
for n in sys1 sys2 sys3; do
  "$PROTOC" -I "$EXAMPLE/proto" -I "$GEN" \
    --grpc_out="$DGEN" --plugin=protoc-gen-grpc="$GRPC_PLUGIN" "$EXAMPLE/proto/$n.proto"
  GRPC_SRCS="$GRPC_SRCS $DGEN/$n.grpc.pb.cc"
done

PB="$GEN/sys1.pb.cc $GEN/sys2.pb.cc $GEN/sys3.pb.cc $GEN/process_context.pb.cc $GEN/metadata_options.pb.cc"
PROJ="$GEN/sys1.proj.cc $GEN/sys2.proj.cc $GEN/sys3.proj.cc"
INC="-I $EXAMPLE/src -I $GEN -I $DGEN $GFLAGS"

echo "[build] routing_server"
$CXX -std=c++17 -O1 $INC routing_server.cc $GRPC_SRCS $PB \
  $GLIBS -Wl,-rpath,"$PREFIX/lib" -o "$BIN/routing_server"
echo "[build] routing_client"
$CXX -std=c++17 -O1 -DROUTINGMETA_WITH_GRPC $INC routing_client.cc $GRPC_SRCS $PB $PROJ \
  $GLIBS -Wl,-rpath,"$PREFIX/lib" -o "$BIN/routing_client"

ADDR="127.0.0.1:50251"
"$BIN/routing_server" "$ADDR" & SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1                                   # let the server bind (client also wait_for_ready)
"$BIN/routing_client" "$ADDR"
echo; echo "== done =="
