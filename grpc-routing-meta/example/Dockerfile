# Ubuntu reference build — mirrors ci.yml's grpc-smoke job (proven green in CI):
# apt-installed protobuf + gRPC dev packages, then build.sh's own pipeline.
# For k8s: build this image in your pipeline / as an initContainer; the final
# stage only needs the build/ output, not the toolchain.
#
# NOTE: apt's libprotobuf-dev on 22.04 is 3.12.4 — proto3 `optional` needs
# --experimental_allow_proto3_optional at that version (TUTORIAL.zh.md §1.1).
# None of this kit's protos use `optional`; if yours do and you want >=3.15
# without the flag, build protobuf from source instead (see ci.yml's `matrix`
# job, which pins 3.20.3/3.21.12 that way).
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config \
    protobuf-compiler libprotobuf-dev libprotoc-dev libgrpc++-dev \
    && rm -rf /var/lib/apt/lists/*

# Build context is this directory (example/) — run as: cd example && docker build .
WORKDIR /src
COPY . .

# Fails the image build itself if codegen validation, tests, or receiver
# verification regress — same green gate as ./build.sh run locally.
RUN ./build.sh
