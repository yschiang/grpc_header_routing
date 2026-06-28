# grpc-routing-meta

The project lives in [`grpc-routing-meta/`](grpc-routing-meta/) — a C++ kit that
projects gRPC routing metadata out of the protobuf body (a `protoc` plugin + one
unified sender), so APISIX can route on headers that can never drift from the body.

- Overview: [`grpc-routing-meta/README.md`](grpc-routing-meta/README.md)
- Runnable example: [`grpc-routing-meta/example/`](grpc-routing-meta/example/)
