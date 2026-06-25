# Header Kit — gRPC Routing Metadata Generator

Implements the sender-side metadata projection defined in **`design-docs/02_grpc_routing_metadata_spec.md`**. Generates, from the proto body, the routing headers (Layer 1+2) and the repeated process-context projection (Layer 3) with integrity digest and overflow handling — so the body stays the single source of truth and headers cannot drift.

> Contract authority is `02_grpc_routing_metadata_spec.md` (+ `00_design_assumptions.md`). This kit *implements* that contract; it does not define it. Metadata keys, layers, overflow limits, and the digest rule come from the spec — this kit references, never redefines them.

## Contents

```
proto/
  metadata_options.proto     custom options: (routing.header) for L1/2 single-value
                             headers; (routing.ctx) for L3 process-context fields
  example_apc_batch.proto    worked example (slim spec §8.1 APC batch control)
plugin/
  protoc-gen-meta.cc         protoc plugin: reads options -> generates ApplyMeta()
common/
  url_encode.h               shared URL-encode (urlencoded-query-string-v1)
  sha256.h                   shared SHA-256 for x-process-context-digest
```

## What the generated `ApplyMeta()` does

- Emits Layer 1 + Layer 2 headers as **single-valued** metadata (`x-target-system`, `x-tool-id`, `x-route-profile`, …); enforces `required` at runtime.
- Emits Layer 3 as **repeated `x-process-context`**, one per context, each a key-sorted URL-encoded query string.
- Computes `x-process-context-count`, `-format`, and a `sha256:` `-digest` over the canonical (key-sorted, newline-joined) projection — order-independent.
- Applies the **overflow policy** (spec Appendix C, v1 = 25): beyond the limit it sets `x-process-context-overflow: true` and suppresses projection; backend reads full detail from the body.

## Build & use

```sh
# 1. compile the options proto
protoc --cpp_out=. proto/metadata_options.proto
# 2. build the plugin (needs metadata_options.pb.cc to read the extensions)
g++ -std=c++17 plugin/protoc-gen-meta.cc metadata_options.pb.cc \
    $(pkg-config --cflags --libs protobuf) -lprotoc -o protoc-gen-meta
# 3. generate the builder for your request proto
protoc --plugin=protoc-gen-meta --meta_out=. -I. -Iproto your_request.proto
# 4. at send time
#    grpc::ClientContext ctx; ApplyMeta(req, &ctx); stub->Rpc(&ctx, req, &resp);
```
Include path must contain `common/` (for `url_encode.h`, `sha256.h`).

## Verification status (protobuf 3.21.12)

| Check | Status |
|---|---|
| Options proto compiles | ✅ |
| Plugin builds against libprotoc | ✅ |
| Generated code compiles & runs | ✅ |
| Output matches slim spec §8.1 (APC batch, 2 contexts) | ✅ byte-checked |
| Layer 1/2 single-valued; chamber NOT a routing key | ✅ |
| L3 repeated, key-sorted, order-independent digest | ✅ |
| SHA-256 matches known `abc` vector | ✅ |
| Overflow (>25) sets flag + suppresses projection | ✅ |
| Non-scalar annotation fails at protoc build | ✅ |

## Alignment notes

- This kit follows the **batch** model (repeated process-context in one tx), not fan-out.
- Chamber is projected inside Layer 3 only; it is never a routing key (see `00_design_assumptions` B-2).
- Open items inherited from the spec: digest consumer (OPEN-B), de-batch under overflow (OPEN-C), APISIX header-to-variable naming (OPEN-D).
