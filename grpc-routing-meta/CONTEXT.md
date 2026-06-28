# CONTEXT — grpc-routing-meta design summary

The one-page contract behind the `example/` kit. Written to be used for **writing
tests and doing code review** — every numbered invariant below is something to
assert or check. Full background (spec, domain model) is in `archive/`.

> **Normative wire rules live in [`SPEC.md`](SPEC.md)** (the byte-level header contract).
> This document is project context + the testable invariants; where it and SPEC.md
> disagree, SPEC.md wins.

## What it is

A sender-side kit that **projects routing metadata out of the gRPC request body**,
so APISIX can route / preprocess without parsing the body, and the headers can
never drift from the body. A `protoc` plugin generates the projection; one
`Send<>()` serves every system.

## Glossary

| Term | Meaning |
|---|---|
| **body-authoritative** | The protobuf body is the single source of truth. Every header is a projection *of* the body; a header value never exists without a body source. |
| **projection** | Body → header transformation done at the sender by generated `ProjectMeta()`. |
| **common headers** | The 6 sender-known headers filled identically for every system (`FillCommon`). |
| **process-context** | One process-execution context (lot+chamber+recipe+…). A request carries `repeated ProcessContext`; the universal Layer-3 projection. |
| **domain scalar** | A single body-derived routing value specific to one domain, e.g. `x-mask-id` (sys3 only), tagged `(routing.project)`. |
| **canonical form** | The deterministic string a context projects to: fields key-sorted, `&`-joined, values URL-encoded. The digest is computed over this. |
| **overflow** | Projection too large for gRPC metadata → emit an explicit `x-process-context-overflow: true` instead of the context lines. |

## Systems (3) and methods (16)

| System | proto / package | methods | Layer-3 shape |
|---|---|---:|---|
| **sys1** | `sys1.proto` / `sys1.v1` | 1 (`Calculate`) | process-context, **batch** (N contexts) |
| **sys2** | `sys2.proto` / `sys2.v1` | 5 (`Verify`/`Download`/`Qualify`/`Upload`/`List`) | process-context, often **sparse** / `count=0` |
| **sys3** | `sys3.proto` / `sys3.v1` | 10 (`Submit01..10`) | **domain scalar** `x-mask-id` (heterogeneous nested paths) + process-context |

All three import the shared `process_context.proto` (`common.v1.ProcessContext`),
so the schema cannot diverge.

## Header model — what the sender emits

| Layer | Headers | Filled by |
|---|---|---|
| common | `x-request-id`, `x-correlation-id`, `x-contract-version`, `x-source-system`, `x-site-id`, `x-tool-id` | `FillCommon` — uniform, every system |
| process-context | `x-process-context-count`, `-format`, `-digest`, `x-process-context` (repeated), `-overflow` | generated `ProjectMeta` → `EmitProcessContexts` |
| domain scalar | `x-mask-id` | generated `ProjectMeta` (sys3 only) |

## Invariants (assert / review against these)

1. **Body-authoritative.** Every emitted header value comes from the body. An empty body field projects as `Key=` (present-but-empty) — still a faithful projection, not drift.
2. **Common = 6, uniform.** `FillCommon` emits exactly the 6 common headers with no per-system branching.
3. **Shared 7-field schema.** `ProcessContext` = `LotID, RecipeID, Tech, PartID, StageID, OperationNO, ChamberId`, identical for every system.
4. **Canonical encoding.** Each `x-process-context` = fields **sorted by key**, `&`-joined, each value URL-encoded (RFC 3986 unreserved kept, else `%XX`, space→`%20`). Sorted key order: `ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech`.
5. **Dynamic count.** `x-process-context-count` = the body's `repeated` size (0..N) — always, even on overflow. Nothing is hard-coded.
6. **Digest = consistency.** When contexts are emitted, `x-process-context-digest = "sha256:" + sha256(contexts joined by '\n')`. The receiver recomputes and compares; mismatch ⇒ header/body drift or tamper.
7. **count=0.** Emit `count` + `format` only — no digest, no context lines.
8. **Size guard (never silent).** Emit `x-process-context-overflow: true` (suppress lines + digest) iff `count > 25` **OR** any single context `> 512 B` **OR** total metadata `> 7168 B`. Total is measured by the byte-tracking `MetadataSink` using gRPC's accounting (`name + value + 32` per header). `count` + `format` are still emitted. The byte check is independent of the count cap — 25 wide contexts can exceed 7 KB.
9. **Domain scalar.** `x-mask-id` is projected via `(routing.project)`, reached by a nested-path walk, and `required` — when empty, `ProjectMeta` records `Issue{MissingRequired, "x-mask-id"}` in its `ProjResult` (`ok=false`) and emits `x-routing-error: missing:x-mask-id`, suppressing the empty `x-mask-id` (it does **not** throw and does **not** emit an empty scalar). `(routing.project)` is rejected at codegen time unless it sits on a **non-repeated scalar leaf** (a repeated or message-typed tag, or one reached under a repeated field, fails `Validate` loudly — see `tests/negative/`). The 7 KB guard (8) counts the scalar's bytes when contexts follow it, but does not itself bound the scalar's length; domain scalars are short identifiers by contract.
10. **One sender.** `Send(req, rt, sink) = FillCommon(rt, sink); ProjectMeta(req, sink)`. No system/method branching; the `ProjectMeta` overload is chosen by request type. Adding a system/method changes only a proto + the build list.

## Code map

| Path | Role |
|---|---|
| `example/proto/metadata_options.proto` | `(routing.project)` / `(routing.pctx)` field options |
| `example/proto/process_context.proto` | shared `ProcessContext` (7 fields) + `Ack` |
| `example/proto/{sys1,sys2,sys3}.proto` | the three systems |
| `example/src/plugin/protoc-gen-meta.cc` | codegen: emits `ProjectMeta()` (scalar walk + context loop → helper) |
| `example/src/common/metadata_sink.h` | `MetadataSink` (byte-tracking) + `VectorSink` / `GrpcSink` |
| `example/src/common/process_context_emit.h` | **policy**: count / format / digest / overflow / 7 KB guard |
| `example/src/common/{url_encode,sha256}.h` | encode + digest primitives |
| `example/src/common/process_context_parser.h` | receiver-side parse + `VerifyDigest` |
| `example/sender/unified_sender.cc` | `FillCommon` + `Send<>()` + demo of all shapes |
| `example/receiver/receiver_verify.cc` | round-trip digest check |
| `example/tests/test_projection.cc` | invariants 3–9 |
| `example/tests/negative/*.proto` | malformed `(routing.project)` fixtures — codegen must reject (invariant 9, fail-loud) |

## Review / test checklist

- [ ] Encoding (4): `/`→`%2F`, space→`%20`, unreserved untouched; decode round-trips.
- [ ] Key-sort (4): `ChamberId` precedes `LotID` in every context line.
- [ ] Dynamic count (5): 0, 1, 2, N contexts → matching `x-process-context-count`.
- [ ] Digest round-trip (6): sender→receiver `VerifyDigest` ok; tamper ⇒ fail.
- [ ] count=0 (7): no digest, no lines.
- [ ] Overflow by count (8): >25 ⇒ flag, no lines/digest.
- [ ] Overflow by bytes (8): ≤25 contexts but >7 KB ⇒ flag (independent of count).
- [ ] Domain scalar (9): nested path reached; empty ⇒ `ok=false` + `x-routing-error: missing:x-mask-id`, scalar suppressed (no throw).
- [ ] Codegen fail-loud (9): malformed `(routing.project)` — repeated, message-typed, under-repeated, or duplicate key — makes `Validate` reject with a non-zero protoc exit (`tests/negative/`, asserted in `build.sh`).
- [ ] Uniform sender (2,10): common 6 present and identical across systems; selectors absent.

Build & run: `cd example && ./build.sh` (or `cmake -S example -B example/build`), then
`./build/unified_sender`, `./build/receiver_verify`, `./build/test_projection`.
