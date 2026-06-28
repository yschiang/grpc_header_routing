---
stepsCompleted: [step-01-validate-prerequisites, step-02-design-epics, step-03-create-stories, step-04-final-validation]
inputDocuments:
  - _bmad-output/planning-artifacts/PRD.md
  - _bmad-output/planning-artifacts/architecture/architecture-gprc_header_routing-2026-06-27/ARCHITECTURE-SPINE.md
---

# grpc-routing-meta - Epic Breakdown

## Overview

This document provides the complete epic and story breakdown for grpc-routing-meta
(`gprc_header_routing`), decomposing the requirements from the PRD and the Architecture
Spine into implementable stories. There is **no UX contract** — this is headless
platform/infrastructure software (PRD §2: no UI).

## Requirements Inventory

### Functional Requirements

FR1: Failure-as-data — `ProjectMeta` (and the demo `Send`) MUST NOT throw on a data condition; both return/propagate `routingmeta::ProjResult { bool ok; std::vector<Issue> issues; std::chrono::nanoseconds duration; }`. (supersedes OVERVIEW §4 "throw")
FR2: Missing required scalar — when a `required` `(routing.project)` scalar (today sys3 `x-mask-id`) is empty, `ProjectMeta` records `Issue{MissingRequired,"x-mask-id"}` with `ok=false`, emits `x-routing-error: missing:x-mask-id`, and does NOT emit the empty scalar header. No throw.
FR3: Overflow as non-blocking data — when projecting would exceed any of total kit metadata > 7168 B, count > 25, or one context value > 512 B: emit `x-process-context-overflow: true`, suppress context lines + digest, still emit `count` + `format`, record a non-blocking `Issue{Overflow}`, leave `ok=true`.
FR4: Build-time gate (fail loud) — the plugin rejects, with non-zero `protoc` exit, any `(routing.project)` not on a non-repeated scalar leaf (repeated, message-typed, or under a repeated field) and any duplicate projected key. (present — keep)
FR5: Receiver gate — the receiver recomputes `x-process-context-digest` over the canonical (`\n`-joined) contexts and rejects on mismatch. (present — keep)
FR6: One sender path in the kit — `Send` MUST be a single branchless template defined in the kit (`routingmeta::Send`) = `FillCommon` + generated `ProjectMeta` + timing → `ProjResult`, with `ProjectMeta` selected by request type via ADL on the `routingmeta::MetadataSink` arg and ZERO per-system branching; it MUST NOT throw on a data condition (it propagates `ProjResult`). The caller owns only the abort/proceed decision (reads `ok`/`issues`). (BRIEF E; plan.md P0.3; SPEC §7; CONTEXT inv. 10; AD-4.)
FR7: Perf trace — every `ProjResult.duration` is populated with the measured projection time; a `bench_projection` binary prints per-call time for 1/2/25/60 contexts and asserts each is sub-millisecond.
FR8: Canonical projection (regression-guarded) — per-context value = `(routing.pctx)` fields key-sorted (`ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech`), `&`-joined as `Key=UrlEncode(Value)` (RFC 3986 unreserved verbatim, else `%XX` uppercase, space `%20`), empty field → `Key=`; digest = `sha256:` + hex over `\n`-joined contexts. (present — keep + harden tests)

### NonFunctional Requirements

NFR1: Portability — no machine-specific paths; CMake uses `find_package(Protobuf)`; `build.sh` discovers the toolchain (env override → derive); builds on a stock Linux toolchain with gcc or clang.
NFR2: Proven matrix — CI builds + tests on Linux × {gcc, clang} × {protobuf 3.20, 3.21}, runs the negative-codegen gate, runs all three binaries plus the bench.
NFR3: No new runtime dependencies — keep the hand-rolled `url_encode` and `sha256` (harden their tests, do not swap in OpenSSL); gRPC stays optional.
NFR4: Determinism — canonical encoding is byte-identical run-to-run; digest reproducible; key order fixed.
NFR5: Thread-safety — `ProjectMeta` is pure (reads request, writes a per-call sink; no shared mutable state) and therefore re-entrant; documented as an invariant.
NFR6: Centralized policy — the three numeric limits (7168/25/512) and the digest/overflow policy live only in `process_context_emit.h`; exactly one codegen plugin.
NFR7: Observability without coupling — the kit performs no logging or metrics itself; callers read `issues` (logs/metrics) and `duration` (tracing) from the returned result.

### Additional Requirements

_Compatibility / brownfield (PRD §3.3):_
- CR1: Wire contract unchanged — no SPEC byte changes; the only new header is `x-routing-error` (already provisional in SPEC §2/§7), emitted only on a missing required scalar; digest/encoding/overflow/count semantics unchanged.
- CR2: Projection API surface evolves additively — the generated entry point changes from returning nothing to returning `ProjResult`; source compatibility preserved for callers that ignore it. (type placement + overload resolution = architecture, AD-3/AD-10)
- CR3: `refs/` is read-only — live docs to update are the `grpc-routing-meta/` copies (`CONTEXT.md`, `OVERVIEW.zh.md`, `README.md`); `refs/` and `SPEC.md` are not edited. No push to any remote.

_Hardening (PRD §3.4 — plan.md P1, all four, trimmed):_
- HR1: Crypto/encoding vectors — SHA-256 known-answer vectors (empty, 55/56-byte boundary, multi-block) + URL-encode round-trip across reserved / space / high-byte. (not a fuzz rig)
- HR2: Parser robustness — receiver-side negative no-crash cases; a malformed `%`-escape is passed through literally (SPEC §6); document lenient parse + dup-key-last-wins. (not a full fuzz rig)
- HR3: Thread-safety — document `ProjectMeta` re-entrancy (NFR5) + one concurrent test.
- HR4: gRPC adapter in CI — compile-smoke the `ROUTINGMETA_WITH_GRPC` GrpcSink adapter in CI. (compile only — not a live server)

_Architecture-derived technical requirements (from the Architecture Spine — shape stories):_
- AR1 (no starter template — brownfield): build on the existing `grpc-routing-meta/example/` tree; there is **no** greenfield scaffold. Epic 1 Story 1 is NOT a project-init story.
- AR2 (namespace + ADL — AD-2/AD-3): move the generated `ProjectMeta` **and** `struct Runtime`/`FillCommon` (currently global) into `namespace routingmeta`; the consumer's unqualified `ProjectMeta`/`FillCommon` calls resolve by ADL on the always-`routingmeta` `MetadataSink` arg, independent of the request's package.
- AR3 (lib boundary — AD-4): kit public surface = `routingmeta::Send` + generated `ProjectMeta` + `FillCommon` + `ProjResult`/`Issue` + policy; the demo calls `Send`.
- AR4 (self-timed duration — AD-6): `ProjectMeta` self-times its own projection and populates `ProjResult.duration`; `bench_projection` measures `ProjectMeta` directly; the demo `Send` does not time.
- AR5 (single-source policy — AD-7): the HPACK per-entry overhead (`32`, today duplicated as a literal `+32`) collapses to one definition in the leaf `metadata_sink.h`, referenced by `process_context_emit.h` (respects include direction).
- AR6 (toolchain discovery — AD-13): `build.sh` uses `PROTOC`/`CXX` env overrides + `pkg-config protobuf` for include/lib flags (append `-lprotoc` for the plugin); CMake uses `find_package(Protobuf)`. Kill the hardcoded anaconda path.
- AR7 (CI pinning — AD-14): GitHub Actions; each protobuf version (3.20.3, 3.21.12) is built **from source at a pinned tag**, installed to a job-local prefix, cached on the tag; each matrix job runs build + negative-codegen gate + the three binaries + `bench_projection` + gRPC compile-smoke. No push → the workflow mirrors exactly the locally-verified steps.
- AR8 (docs truth — criterion I): update the live `grpc-routing-meta/` doc copies — CONTEXT.md inv. 9 "throws" → "records `MissingRequired` in `ProjResult`"; OVERVIEW.zh.md "throw" → ProjResult and digest "tamper" → integrity-only; README. (`refs/` untouched per CR3.)

### UX Design Requirements

None — this is headless platform/infrastructure software with no UI (PRD §2). No UX contract exists or applies.

### FR Coverage Map

FR1: Epic 1 (cluster 1) — failure-as-data `ProjResult` pivot, no throw
FR2: Epic 1 (cluster 1) — missing-required → `x-routing-error` + `Issue{MissingRequired}`, `ok=false`
FR3: Epic 1 (cluster 1) — overflow non-blocking → `Issue{Overflow}` + `x-process-context-overflow`
FR4: Epic 1 (cluster 4) — build-time negative-codegen gate: fixtures + asserts (run in CI via cluster 3)
FR5: Epic 1 (cluster 4) — receiver digest gate regression (behavior present)
FR6: Epic 1 (cluster 1) — one branchless `routingmeta::Send` in the kit
FR7: Epic 1 (cluster 2) — `duration` populated + `bench_projection` sub-ms
FR8: Epic 1 (cluster 4) — canonical projection, regression-guarded

All 8 FRs map to Epic 1. NFR1/2 → cluster 3; NFR3/4/5 → cluster 4; NFR6/7 → cluster 1/2. CR1 → cluster 1; CR2 → cluster 1; CR3 → cross-cutting (cluster 5 doc-edits honor it). HR1/2/3 → cluster 4; HR4 → cluster 3.

## Epic List

### Epic 1: Productionize `grpc-routing-meta` (example-grade → production-grade)

The kit demonstrably delivers every BRIEF benefit A–I at production grade: failures never silent, performance proven sub-millisecond, builds anywhere on a proven toolchain matrix, every invariant locked by a test, and docs true to the code. `refs/` stays read-only; no push to any remote. The architecture is locked (Architecture Spine, `status: final`), so this is delivered as **one epic with ordered story clusters** — no inter-epic risk boundary exists. Story order carries the single load-bearing dependency: the `ProjResult` pivot (cluster 1) lands first because the plugin, sender, tests, and docs all pivot on it.

**FRs covered:** FR1, FR2, FR3, FR4, FR5, FR6, FR7, FR8 (all)

**Ordered story clusters** (stories detailed in the per-epic section below):

1. **Failure-as-data + one sender path** — FR1, FR2, FR3, FR6 + AR2 (namespace+ADL), AR3 (boundary), AR5 (HPACK single-source). Criteria C, E; contributes F/NFR6/NFR7. *First — load-bearing.*
2. **Perf observability** — FR7 + AR4 (self-timed `duration`). Criterion H. *Needs cluster-1 `ProjResult.duration`.*
3. **Portable build + CI matrix** — NFR1, NFR2, HR4 + AR6 (toolchain discovery), AR7 (CI pinning). Criteria A, B.
4. **Invariant test hardening** — FR4, FR5, FR8 + HR1, HR2, HR3. Criteria G, D; NFR3/4/5. *Asserts live in the existing `test_projection` binary.*
5. **Docs truth** — AR8. Criterion I. *Live `grpc-routing-meta/` copies only; `refs/` untouched (CR3).*

### Epic 2: Live gRPC demo (real-wire showcase)

A runnable client + server that send real gRPC traffic carrying the projected routing-meta headers and verify them on receipt — elevating HR4 from compile-smoke (Story 1.9) to a live, self-checking end-to-end demo. Adds no wire bytes (CR1) and keeps the core build gRPC-free (NFR3): the demo is opt-in. Ships after Epic 1; builds on the locked kit surface (`routingmeta::Send`, `GrpcSink`, `VerifyDigest`) and the service definitions already in the protos.

**FRs covered:** none new — demonstrates FR2/FR3/FR5/FR6/FR8 over a real channel.

**Stories:**

1. **Real-wire gRPC demo** — client + server + `run.sh` + `DEMO.md`; the server reuses the receiver digest gate over `client_metadata()`.

## Epic 1: Productionize `grpc-routing-meta` (example-grade → production-grade)

The kit demonstrably delivers every BRIEF benefit A–I at production grade. Stories are ordered so none depends on a later one; the `ProjResult` pivot (1.1) lands first because the plugin, sender, tests, and docs all pivot on it. `refs/` is read-only; no push to any remote.

### Story 1.1: `ProjectMeta` returns `ProjResult`, no throw

As a sender developer,
I want `ProjectMeta` to report a missing required field as structured data instead of throwing,
So that a bad request is caught at the source and surfaced — never silent, never an exception I must catch.

**Acceptance Criteria:**

**Given** the new `src/common/proj_result.h` defining `ProjResult{ bool ok; std::vector<Issue> issues; std::chrono::nanoseconds duration; }` and `Issue{ Kind(MissingRequired|Overflow), std::string key }`,
**When** the kit builds,
**Then** the generated `ProjectMeta` is declared returning `routingmeta::ProjResult` (was `void`)
**And** no `throw` exists on a data condition in the generated path. (FR1, AD-5)

**Given** a sys3 request with an empty required `x-mask-id`,
**When** `ProjectMeta` runs,
**Then** it returns `ok=false` with one `Issue{MissingRequired, "x-mask-id"}`, emits header `x-routing-error: missing:x-mask-id`,
**And** does NOT emit the empty `x-mask-id` header. (FR2, SPEC §7)

**Given** a fully-populated request,
**When** `ProjectMeta` runs,
**Then** `ok=true`, `issues` is empty, and all expected headers are emitted unchanged. (CR1 — no wire change)

**Given** `test_projection`,
**When** it runs,
**Then** the former "asserts threw" case is replaced by asserting the `ProjResult` / `x-routing-error` behavior
**And** ALL TESTS PASSED. (BRIEF Verify)

### Story 1.2: Overflow surfaces as non-blocking data

As a gateway operator,
I want an oversized projection flagged explicitly and non-blocking,
So that routing still proceeds but the overflow is observable, never silently dropped.

**Acceptance Criteria:**

**Given** a request whose projection would exceed any of total kit metadata > 7168 B, count > 25, or one context value > 512 B,
**When** `ProjectMeta` runs,
**Then** it emits `x-process-context-overflow: true`, suppresses the context lines and the digest, still emits `count` and `format`,
**And** records a non-blocking `Issue{Overflow}` while leaving `ok=true`. (FR3, SPEC §5.4, AD-5)

**Given** the same request,
**When** the caller inspects the returned `ProjResult`,
**Then** `issues` contains exactly one `Overflow` issue and `ok` is true.

**Given** `test_projection`,
**When** it runs,
**Then** an overflow case asserts the above and ALL TESTS PASSED.

### Story 1.3: One coherent `routingmeta` namespace, resolved by ADL

As a sender developer,
I want the generated `ProjectMeta` (and `FillCommon`/`Runtime`) to live in `namespace routingmeta` and resolve by ADL on the sink,
So that my call site needs no per-type qualification and can't be broken by a proto's package declaration.

**Acceptance Criteria:**

**Given** the plugin output,
**When** code is generated,
**Then** `ProjectMeta` is emitted inside `namespace routingmeta`, and each `*.proj.h` `#include`s the kit headers and wraps its output in `routingmeta`. (AD-2)

**Given** `common_headers.h`,
**When** the kit builds,
**Then** `struct Runtime` and `FillCommon` are declared in `namespace routingmeta` (no longer at global scope). (AD-2)

**Given** a caller that calls unqualified `ProjectMeta(req, sink)` and `FillCommon(rt, sink)`,
**When** it compiles,
**Then** resolution is by ADL on the `routingmeta::MetadataSink` argument
**And** does not depend on the request's package namespace. (AD-3)

**Given** the build,
**When** binaries are run,
**Then** no wire bytes change (CR1) and all binaries link.

### Story 1.4: One branchless `routingmeta::Send` lives in the kit

As a sender developer,
I want a single branchless `Send<>()` serving sys1/sys2/sys3 with zero per-system code that propagates `ProjResult` and never throws,
So that all three systems go out one identical path and I own the abort/proceed decision.

**Acceptance Criteria:**

**Given** the kit,
**When** searched,
**Then** the kit declares a single branchless `routingmeta::Send` (= `FillCommon` + `ProjectMeta`, returns `ProjResult`, no-throw); `unified_sender.cc` calls it. (FR6, AD-4)

**Given** the demo `Send<>()`,
**When** inspected,
**Then** it is a single template = `FillCommon` + `ProjectMeta`, with zero `if (system==…)` branching, serving sys1/sys2/sys3. (BRIEF E, CONTEXT inv. 10)

**Given** a request with an empty required scalar,
**When** the demo `Send` runs,
**Then** it does not throw; it propagates the `ProjResult` (`ok=false` + `x-routing-error`) for the caller to act on. (FR1 binds `Send` too; AD-5)

**Given** `./build/unified_sender`,
**When** run,
**Then** it prints 3 system blocks and the empty sys3 mask surfaces `x-routing-error` + a duration. (BRIEF Verify line 2)

### Story 1.5: Single-source the HPACK overhead constant

As a platform/contract maintainer,
I want the HPACK per-entry overhead defined exactly once,
So that the byte-budget math cannot drift between files.

**Acceptance Criteria:**

**Given** `metadata_sink.h` and `process_context_emit.h`,
**When** the kit builds,
**Then** `kHpackEntryOverhead = 32` is defined exactly once — in the leaf `metadata_sink.h` — and referenced by `process_context_emit.h`
**And** no literal `+32` duplicate remains. (AD-7, respects include direction)

**Given** the three limits 7168 / 25 / 512,
**When** searched,
**Then** they live only in `process_context_emit.h`. (NFR6, F)

**Given** the byte-accounting,
**When** a context is added,
**Then** the size math uses the single constant and the existing overflow thresholds are unchanged.

### Story 1.6: `ProjectMeta` self-times and reports `duration`

As an operator/tracer,
I want every projection to report its own measured duration,
So that I get a per-call latency signal with no coupling to the kit.

**Acceptance Criteria:**

**Given** `ProjectMeta`,
**When** it runs,
**Then** it measures its own projection time with a steady clock and populates `ProjResult.duration` (> 0). (FR7, AD-6)

**Given** the demo (`unified_sender`) calling `routingmeta::Send`,
**When** it runs,
**Then** `Send` does NOT time the projection; it only propagates `ProjResult.duration`. (one timing point; AD-6)

**Given** the kit,
**When** inspected,
**Then** it performs no logging or metrics itself — `duration` is read by the caller. (NFR7)

### Story 1.7: `bench_projection` proves sub-millisecond

As a build/release engineer,
I want a bench that proves projection is sub-ms across realistic sizes,
So that the "sub-ms / perf observed" claim (criterion H) is evidence, not assertion.

**Acceptance Criteria:**

**Given** a new `tests/bench_projection`,
**When** built and run,
**Then** it prints per-call projection time for 1, 2, 25, and 60 contexts. (FR7, BRIEF H)

**Given** each measured time,
**When** the bench asserts,
**Then** each is sub-millisecond and the binary exits non-zero if any exceeds 1 ms.

**Given** `build.sh`,
**When** run,
**Then** `bench_projection` builds and links alongside the other binaries.

### Story 1.8: Portable build (no machine-specific paths)

As a build/release engineer,
I want the build to work on a stock Linux toolchain with no hardcoded paths,
So that anyone can build the kit.

**Acceptance Criteria:**

**Given** `build.sh`,
**When** inspected,
**Then** it contains no hardcoded anaconda/developer path; it uses `PROTOC` (default `protoc` on PATH) and `CXX` (default `c++`) env overrides, and derives protobuf include/lib flags from `pkg-config protobuf`, appending `-lprotoc` for the plugin. (AD-13)

**Given** `CMakeLists.txt`,
**When** inspected,
**Then** it uses `find_package(Protobuf)` with no absolute toolchain paths. (NFR1)

**Given** `cd grpc-routing-meta/example && ./build.sh` on a stock toolchain,
**When** run,
**Then** the plugin builds, codegen runs, binaries link, and the negative gate is green. (BRIEF Verify line 1)

### Story 1.9: Proven on a pinned CI matrix

As a build/release engineer,
I want CI to prove the kit on a real matrix,
So that it cannot silently bind to one compiler or protobuf version.

**Acceptance Criteria:**

**Given** `.github/workflows/`,
**When** inspected,
**Then** a GitHub Actions workflow defines a matrix Linux × {gcc, clang} × {protobuf 3.20.3, 3.21.12}, building each protobuf from source at its pinned tag into a job-local prefix, cached on the tag. (AD-14)

**Given** each matrix job,
**When** it runs,
**Then** it performs: build + negative-codegen gate + `unified_sender` + `receiver_verify` + `test_projection` + `bench_projection` + a gRPC compile-smoke of the `ROUTINGMETA_WITH_GRPC` GrpcSink adapter. (NFR2, HR4)

**Given** the workspace forbids push,
**When** the workflow is authored,
**Then** it mirrors exactly the locally-verified steps and is validated by local-path equivalence, not a remote green run. (PRD §6)

### Story 1.10: Build-time negative-codegen gate, proven by fixtures

As a system provider,
I want a malformed annotation to fail the build,
So that a bad `(routing.project)` never reaches the Sender.

**Acceptance Criteria:**

**Given** `tests/negative/*.proto` fixtures,
**When** present,
**Then** they cover `(routing.project)` on a repeated field, on a message-typed field, on a field under a repeated, and a duplicate projected key. (AD-8, SPEC §9)

**Given** each negative fixture,
**When** `protoc --meta_out` runs in `build.sh` and CI,
**Then** codegen fails with non-zero exit and the build asserts the rejection. (FR4)

**Given** a valid proto,
**When** codegen runs,
**Then** it succeeds (no false positive).

### Story 1.11: Receiver digest gate regression

As a receiver,
I want to recompute and compare the digest,
So that any header↔body drift is rejected.

**Acceptance Criteria:**

**Given** a correctly projected context set,
**When** `receiver_verify` recomputes `x-process-context-digest` over the `\n`-joined contexts,
**Then** it matches and accepts. (FR5, BRIEF Verify line 3)

**Given** a tampered or mismatched context set,
**When** the receiver verifies,
**Then** it rejects on digest mismatch. (AD-9)

**Given** `test_projection` / `receiver_verify`,
**When** run,
**Then** both the accept and reject paths are asserted.

### Story 1.12: Canonical projection + crypto vectors, regression-guarded

As a platform/contract maintainer,
I want byte-exact encoding and digest locked by tests,
So that projection cannot drift run-to-run or release-to-release.

**Acceptance Criteria:**

**Given** a known request,
**When** projected,
**Then** each context = `(routing.pctx)` fields key-sorted (`ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech`), `&`-joined as `Key=UrlEncode(Value)`, empty field → `Key=`, `/` → `%2F`, asserted byte-for-byte against a golden. (FR8, SPEC §5–6)

**Given** the same input projected twice,
**When** compared,
**Then** the output is byte-identical. (NFR4 determinism)

**Given** SHA-256 known-answer vectors (empty, 55- and 56-byte boundary, multi-block) and URL-encode round-trips (reserved, space, high-byte),
**When** asserted,
**Then** each matches its expected value. (HR1)

### Story 1.13: Parser robustness (lenient, no-crash)

As a receiver,
I want lenient parsing that never crashes on malformed input,
So that a bad header degrades gracefully per spec.

**Acceptance Criteria:**

**Given** a context value with a malformed `%`-escape,
**When** the receiver parses it,
**Then** the malformed escape is passed through literally (SPEC §6) and parsing does not crash. (HR2)

**Given** duplicate keys within a context,
**When** parsed,
**Then** last-wins, and this rule plus the lenient-parse rule are documented.

**Given** a set of negative / garbled inputs,
**When** parsed,
**Then** no crash occurs on any of them.

### Story 1.14: Thread-safe, re-entrant projection

As a sender developer,
I want `ProjectMeta` to be safe to call concurrently,
So that a multithreaded sender is correct without locks.

**Acceptance Criteria:**

**Given** `ProjectMeta`,
**When** reviewed,
**Then** it reads the request and writes only a per-call sink with no shared mutable state, and its re-entrancy is documented as an invariant. (AD-12, NFR5)

**Given** N threads projecting concurrently,
**When** a concurrent test runs,
**Then** results are identical to the single-threaded projection and there is no data race (clean across repeated runs / under sanitizer). (HR3)

### Story 1.15: Live docs match the code

As a new adopter,
I want the docs to describe the behavior the code actually has,
So that onboarding is not misled by stale claims.

**Acceptance Criteria:**

**Given** the live `grpc-routing-meta/` copy of `CONTEXT.md`,
**When** updated,
**Then** inv. 9 "throws" is rewritten to "records `MissingRequired` in `ProjResult` (`ok=false`) and emits `x-routing-error: missing:<key>`, suppressing the empty scalar." (criterion I)

**Given** the live `OVERVIEW.zh.md`,
**When** updated,
**Then** the "throw" failure language becomes `ProjResult`, and the digest "tamper" framing becomes integrity-only (a body editor recomputes the digest; it is not security).

**Given** the live `README.md`,
**When** updated,
**Then** the `Send` / `ProjResult` usage and the `routingmeta::Send`-in-the-kit boundary are described.

**Given** `refs/`,
**When** the doc work is complete,
**Then** nothing under `refs/` (or `SPEC.md`) has been edited. (CR3)

## Epic 2: Live gRPC demo (real-wire showcase)

Take HR4 from a compile-only smoke to a live, self-checking demonstration: real gRPC traffic carries the projected headers across a localhost hop, and a server re-runs the existing digest gate on what it receives. The epic introduces no new wire contract and no new runtime dependency in the core kit — gRPC stays opt-in (CR1, NFR3). It builds entirely on Epic 1's locked surface (`routingmeta::Send`, `GrpcSink`, `VerifyDigest`/`ParseContext`) and the `Sys1/2/3Service` definitions already present in the protos (the build simply never ran `--grpc_out`).

### Story 2.1: Real-wire gRPC demo (live client + server carrying projected routing-meta)

As an adopter evaluating the kit,
I want a runnable gRPC client + server that send real traffic carrying the projected routing-meta headers and verify them on receipt,
So that I can see — not just take on faith — that body-authoritative projection survives an actual gRPC hop and that the digest gate catches header/body drift on the wire.

**Acceptance Criteria:**

**Given** a `grpc_server` implementing `Sys1Service` (and `Sys3Service.Submit05` for the missing-required path),
**When** a client RPC arrives,
**Then** the server reads `grpc::ServerContext::client_metadata()`, reconstructs the ordered `x-process-context` values + `x-process-context-digest`, and calls `routingmeta::VerifyDigest` (reused from `process_context_parser.h`, not reimplemented), logging accept/reject. (FR5 over the wire)

**Given** a well-formed request projected via `GrpcSink` + `routingmeta::Send`,
**When** the client calls the RPC over a real localhost channel,
**Then** the server logs `digest check: OK` and accepts.

**Given** a context tampered after projection (header/body drift),
**When** the server verifies,
**Then** `VerifyDigest` returns `ok=false` and the server rejects it (documented status/flag). (AD-9 over the wire)

**Given** the empty-mask `Sys3Service.Submit05` and an overflow request,
**When** the client sends them,
**Then** `x-routing-error: missing:x-mask-id` (`ok=false`, still delivered) and `x-process-context-overflow: true` (server logs "no digest provided") are observed on the wire. (FR2, FR3; CR1 — no new bytes)

**Given** `demo/run.sh`,
**When** run where `grpc++` + `grpc_cpp_plugin` are present,
**Then** it generates the `--grpc_out` stubs, builds with `-DROUTINGMETA_WITH_GRPC`, runs server+client, and exits non-zero unless the good case verifies AND the tamper case is rejected; absent gRPC it prints an install hint and exits non-zero — fail loud, never a fake pass. (NFR3 opt-in; BRIEF criterion C)

**Given** `demo/DEMO.md`,
**When** read,
**Then** it states what the demo proves, prerequisites, how to run, annotated expected output, and the HR4 (compile-smoke → live) relation; nothing under `refs/` is edited. (CR3)

**Story file:** `_bmad-output/implementation-artifacts/2-1-real-wire-grpc-demo.md`
