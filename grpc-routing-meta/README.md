# grpc-routing-meta

A C++ kit that **projects routing metadata out of the gRPC request body** so APISIX
can route / preprocess without parsing the body — and so the headers can never
drift from it (the body is the single source of truth). A `protoc` plugin generates
the projection; one **unified sender** serves every system.

- **中文總覽:** [`OVERVIEW.zh.md`](OVERVIEW.zh.md) — 目的、用法、metadata 範例、error control。
- **Design contract:** [`CONTEXT.md`](CONTEXT.md) — one page of glossary + testable
  invariants (read this before writing tests or reviewing code).
- **Runnable kit:** [`example/`](example/).

## Three systems, one sender

| System | proto | methods | Projects |
|---|---|---:|---|
| **sys1** | `example/proto/sys1.proto` | 1 | process-context, batch (N contexts) |
| **sys2** | `example/proto/sys2.proto` | 5 | process-context, often sparse / `count=0` |
| **sys3** | `example/proto/sys3.proto` | 10 | domain scalar `x-mask-id` (nested paths) + process-context |

All three import the shared `example/proto/process_context.proto`, so the 7-field
schema can't diverge. The lib provides the two building blocks (`FillCommon` +
generated `ProjectMeta` → `ProjResult`); the **Sender** composes them — its own one
call, no per-system branching:

```cpp
// In the Sender (orchestration is the Sender's job, not the lib's):
template <class Req>
routingmeta::ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  FillCommon(rt, sink);            // 6 common headers, identical for every system  (lib)
  return ProjectMeta(req, sink);   // body projection; overload chosen by Req type   (lib)
}
```

`ProjectMeta` is generated per request type by `example/src/plugin/protoc-gen-meta.cc`.
Adding a 4th system (or a 16th method) = one proto + one line in the build list.

## Size guard (no silent failures)

gRPC bounds total metadata; exceeding it makes APISIX/HTTP2 reset or truncate the
stream — an opaque error. `EmitProcessContexts`
(`example/src/common/process_context_emit.h`) tracks the whole running metadata size
and, if projecting the contexts would exceed **7 KB** (or `count > 25`, or a context
`> 512 B`), emits an explicit `x-process-context-overflow: true` instead. The request
still routes on the small common headers; the backend reads full detail from the
body. Opaque transport failure → explicit, in-band signal.

## Error model (report, don't dictate)

`ProjectMeta` returns `ProjResult{ok, issues[], duration}` and never throws on a
data condition (a Sender's `Send` wrapper just forwards it). A missing **required** scalar
(sys3 `x-mask-id`) sets `ok=false`, records a `MissingRequired` issue, and emits
`x-routing-error: missing:x-mask-id` (the empty header is not sent). Overflow is a
non-blocking issue (`ok` stays true). The caller inspects `issues` and decides; the kit
logs nothing — the lib populates + reports, the Sender orchestrates.

## Build & run

```sh
cd example
cmake -S . -B build && cmake --build build -j     # canonical, portable
# or, when cmake is unavailable:
./build.sh                                         # direct protoc + clang, same steps

./build/unified_sender     # prints the metadata each system attaches
./build/receiver_verify    # parses + verifies the digest (round-trip)
./build/test_projection    # or: ctest --test-dir build
./build/bench_projection   # per-call duration for 1/2/25/60 contexts (sub-ms)
```

Requires a C++17 compiler and Protobuf **with libprotoc**. gRPC is optional
(generated code writes into `routingmeta::MetadataSink`); enable the
`grpc::ClientContext` adapter with `-DROUTINGMETA_WITH_GRPC=ON`. Verified end-to-end
on Protobuf 3.20.3 and 3.21.12.

## Layout

```
README.md          this overview
CONTEXT.md         design summary + testable invariants
example/
  proto/           metadata_options, process_context, sys1, sys2, sys3
  src/plugin/      protoc-gen-meta.cc        (codegen)
  src/common/      url_encode, sha256, metadata_sink, process_context_emit, process_context_parser
  sender/          unified_sender.cc         (one Send<>() for all systems)
  receiver/        receiver_verify.cc
  tests/           test_projection.cc
  build.sh  CMakeLists.txt
```
