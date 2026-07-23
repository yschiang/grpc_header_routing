# grpc-routing-meta

A C++ kit that **projects routing metadata out of the gRPC request body** so APISIX
can route / preprocess without parsing the body — and so the headers can never
drift from it (the body is the single source of truth). A `protoc` plugin generates
the projection; one **unified sender** serves every system.

- **採用教學（怎麼接上你的系統）:** [`TUTORIAL.zh.md`](TUTORIAL.zh.md) — 帶回 local env、
  跑懂 sys2 RMS、一步步接真實 proto，含舊 toolchain 相容性。**RMS / sender owner 從這裡進。**
- **中文設計總覽（為什麼這樣設計）:** [`OVERVIEW.zh.md`](OVERVIEW.zh.md) — 目的、
  hardcode vs autogen 比較、error control。
- **Wire contract:** [`SPEC.md`](SPEC.md) — the normative byte-level header spec.
- **Design contract:** [`CONTEXT.md`](CONTEXT.md) — one page of glossary + testable
  invariants (read this before writing tests or reviewing code).
- **End-to-end walkthrough（證明每個行為）:** [`DEMO.md`](DEMO.md) — copy-paste session
  mapped to the BRIEF A–I acceptance criteria. **Reviewer / 驗收者從這裡進。**
- **Runnable kit:** [`example/`](example/).

## Four systems, one sender

| System | proto | methods | Projects |
|---|---|---:|---|
| **sys1** | `example/proto/sys1.proto` | 1 | process-context, batch (N contexts) |
| **sys2** | `example/proto/sys2.proto` | 5 | the RMS: `x-recipe-id` scalar + per-lot contexts (shared **and** own-message pctx, incl. real-shape camelCase `rqst_RMS_GetRecipeSet`) |
| **sys3** | `example/proto/sys3.proto` | 10 | domain scalar `x-mask-id` (nested paths) + process-context |
| **sys4** | `example/proto/sys4.proto` | 1 (`UpdateMaterial`) | process-context LotID-only, producer-normalized from 3 exclusive batch shapes |

All four import the shared `example/proto/process_context.proto`, so the 7-field
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
Adding a 4th system (or a 16th method) = one proto + one line each in
`build.sh` (`SYSTEMS`) and `CMakeLists.txt` (`foreach`) — walkthrough in TUTORIAL §3.

**Wiring contract.** Call `FillCommon(rt, sink)` then `ProjectMeta(req, sink)` on the
*same* sink; read the returned `ProjResult` and decide abort/proceed yourself. Those two
calls — plus `ProjResult` and `MetadataSink` — are the whole kit surface. `Send` above is
a Sender-owned convenience wrapper, **not** part of the kit (see
[`docs/adr/0001-send-ownership-stays-in-sender.md`](docs/adr/0001-send-ownership-stays-in-sender.md)).

`ProjectMeta` takes an optional third argument `emit_digest` (default `true`): pass
`false` to omit the `x-process-context-digest` header for that call — count, format,
context lines, and the overflow decision are unchanged. The receiver verifies **if
present**: an absent digest is skipped, not treated as drift (see SPEC §5.3 /
[`docs/adr/0002-optional-process-context-digest.md`](docs/adr/0002-optional-process-context-digest.md)).

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
(sys2 `x-recipe-id`, sys3 `x-mask-id`) sets `ok=false`, records a `MissingRequired` issue,
and emits `x-routing-error: missing:x-recipe-id` (the empty header is not sent). Overflow is a
non-blocking issue (`ok` stays true). The caller inspects `issues` and decides; the kit
logs nothing — the lib populates + reports, the Sender orchestrates.

For a batch whose elements all repeat the **same** scalar (e.g. N jobs, one mask id),
tag the field `uniform_across_repeated: true`: codegen projects element[0] and verifies
every element agrees — divergence is a blocking `Inconsistent` issue +
`x-routing-error: inconsistent:x-mask-id`, never a silent first-pick (SPEC §4.1,
[`docs/adr/0003`](docs/adr/0003-uniform-across-repeated-projection.md)).

## Build & run

```sh
cd example
cmake -S . -B build && cmake --build build -j     # canonical, portable
# or, when cmake is unavailable:
./build.sh                                         # direct protoc + clang, same steps

./build/unified_sender     # prints the metadata each system attaches
./build/receiver_verify    # parses + verifies the digest if present (round-trip)
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
TUTORIAL.zh.md     採用教學：跑懂 sys2 → 接你自己的 proto → build → test
SPEC.md            normative wire contract (byte-level header spec)
CONTEXT.md         design summary + testable invariants
OVERVIEW.zh.md     中文設計總覽（why：比較、代價、效益）
DEMO.md            end-to-end runnable walkthrough (BRIEF A–I acceptance evidence)
example/
  proto/           metadata_options, process_context, sys1, sys2, sys3
  src/plugin/      protoc-gen-meta.cc        (codegen)
  src/common/      url_encode, sha256, metadata_sink, process_context_emit,
                   process_context_parser, proj_result, common_headers
  sender/          unified_sender.cc         (one Send<>() for all systems)
  receiver/        receiver_verify.cc
  tests/           test_projection.cc, bench_projection.cc, negative/ (codegen-reject fixtures)
  grpc_demo/       real-wire gRPC round-trip (optional; see DEMO.md §5)
  build.sh  CMakeLists.txt
```
