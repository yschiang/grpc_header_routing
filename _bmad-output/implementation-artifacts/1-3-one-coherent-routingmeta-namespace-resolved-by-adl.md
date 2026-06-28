---
baseline_commit: e4a977342f09909919ac77ed655331204d183bcf
---

# Story 1.3: One coherent `routingmeta` namespace, resolved by ADL

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sender developer,
I want the generated `ProjectMeta` (and `FillCommon`/`Runtime`) to live in `namespace routingmeta` and resolve by ADL on the sink,
so that my call site needs no per-type qualification and can't be broken by a proto's package declaration.

## Acceptance Criteria

1. **AC1 — Generated `ProjectMeta` lives in `routingmeta`.** Given the plugin output, when code is generated, then `ProjectMeta` is emitted inside `namespace routingmeta`, and each `*.proj.h` `#include`s the kit headers and wraps its declarations in `routingmeta`. (AD-2)

2. **AC2 — `Runtime` + `FillCommon` live in `routingmeta`.** Given `common_headers.h`, when the kit builds, then `struct Runtime` and `FillCommon` are declared in `namespace routingmeta` (no longer at global scope). (AD-2)

3. **AC3 — Unqualified calls resolve by ADL on the sink.** Given a caller that calls unqualified `ProjectMeta(req, sink)` and `FillCommon(rt, sink)`, when it compiles, then resolution is by ADL on the `routingmeta::MetadataSink` argument **and** does not depend on the request's package namespace. (AD-3)

4. **AC4 — No wire change, everything links.** Given the build, when binaries are run, then no wire bytes change (CR1) and all binaries link. `test_projection` prints `ALL TESTS PASSED`; `unified_sender`/`receiver_verify` produce byte-identical output (same digests).

## Tasks / Subtasks

- [x] **Task 1 — Wrap generated `ProjectMeta` in `namespace routingmeta` (AC: 1, 3)** — edit `example/src/plugin/protoc-gen-meta.cc`
  - [x] `.proj.h` emit block: keep the `#include`s OUTSIDE the namespace (they must not nest), then emit `namespace routingmeta {` before the per-message declaration loop and `}  // namespace routingmeta` after it. The existing `routingmeta::ProjResult` / `routingmeta::MetadataSink` text stays valid inside the namespace.
  - [x] `.proj.cc` emit block: same — `#include`s outside, then `namespace routingmeta {` before the per-message definition loop and `}  // namespace routingmeta` after. Body references (`routingmeta::UrlEncode`, `routingmeta::EmitProcessContexts`, `routingmeta::Issue`, `routingmeta::ProjResult`) remain valid.
  - [x] Do NOT change the function signatures, bodies, or the `req` parameter's fully-qualified type — only add the namespace wrapper.

- [x] **Task 2 — Move `Runtime` + `FillCommon` into `namespace routingmeta` (AC: 2, 3)** — edit `example/src/common/common_headers.h`
  - [x] Wrap `struct Runtime { … }` and `inline void FillCommon(const Runtime& rt, routingmeta::MetadataSink& sink) { … }` in `namespace routingmeta { … }` (after the `#include`s). Keep `routingmeta::MetadataSink` qualified (valid inside the namespace) or drop the qualifier — either compiles.

- [x] **Task 3 — Qualify `Runtime` type-name uses at call sites (AC: 3, 4)**
  - [x] `example/sender/unified_sender.cc`: qualify every `Runtime` **type name** to `routingmeta::Runtime` — the `Send` template param (`unified_sender.cc:33`, `const Runtime& rt` → `const routingmeta::Runtime& rt`), the local at `:59` (`const Runtime rt{…}`), and the inline temporaries at `:77`, `:85`, `:95` (`Runtime{…}` → `routingmeta::Runtime{…}`). Leave the `FillCommon(rt, sink)` (`:34`) and `ProjectMeta(req, sink)` (`:35`) **calls unqualified** — they resolve by ADL. Do NOT move `Send` into the kit (that is Story 1.4).
  - [x] `example/tests/test_projection.cc:186`: `FillCommon(Runtime{"CORR-X", "F18", "ETCH01"}, sink)` → qualify the type only: `FillCommon(routingmeta::Runtime{"CORR-X", "F18", "ETCH01"}, sink)`. Leave `FillCommon` unqualified (ADL on the `Runtime` + sink args). Leave all `ProjectMeta(req, sink)` calls unqualified.
  - [x] `example/receiver/receiver_verify.cc`: **no change** — its `ProjectMeta(req, sink)` (`:31`) already resolves by ADL on the `routingmeta::VectorSink` argument once `ProjectMeta` is in `routingmeta`. (Verify it still builds; do not touch it.)

- [x] **Task 4 — Build & verify (AC: 1, 2, 3, 4)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` — rebuild plugin, regenerate `*.proj.*` (now namespaced), link all binaries + the negative-codegen gate stays green.
  - [x] `./build/test_projection` → `ALL TESTS PASSED` (the `FillCommon`/`ProjectMeta` unqualified calls now resolve by ADL — this is the AC3 proof).
  - [x] `./build/unified_sender` and `./build/receiver_verify` → run clean; output and digests byte-identical to the pre-change run (CR1). Confirm a generated `*.proj.h` shows `namespace routingmeta {` wrapping the `ProjectMeta` declarations.

## Dev Notes

### Method (Amelia)
This is a refactor — the test is "it still builds, links, and produces byte-identical wire output, with the call sites still unqualified." Red/green here is the build: make the namespace move (Tasks 1–2), watch the unqualified `Runtime{…}` type uses fail to compile (red), qualify them (Task 3), reach green. Done when `ALL TESTS PASSED` and digests are unchanged.

### The load-bearing detail: ADL applies to function calls, NOT type names
- **Function calls resolve by ADL and stay unqualified.** After the move, `ProjectMeta` and `FillCommon` live only in `routingmeta`. A caller writing unqualified `ProjectMeta(req, sink)` resolves it because one argument — `sink` — is a `routingmeta::MetadataSink`, so argument-dependent lookup searches `namespace routingmeta` and finds it. This holds regardless of the request's package (`sys1::v1`, `sys3::v1`, …) — that is exactly AD-3 ("resolution MUST NOT depend on the request's package namespace"). `FillCommon(rt, sink)` resolves the same way (ADL on both `routingmeta::Runtime` and `routingmeta::MetadataSink`).
- **Type names do NOT get ADL — they must be qualified.** `Runtime{…}` and `const Runtime&` are *type* uses; once `Runtime` moves into `routingmeta`, an unqualified `Runtime` no longer names anything at global/caller scope and the build breaks. Every `Runtime` type reference at a call site must become `routingmeta::Runtime`. This is THE thing to get right — it is the only reason any caller file changes.

### Current state of the files this story changes (read before editing)
- **`example/src/plugin/protoc-gen-meta.cc`** — emits `*.proj.h` (declarations, `:186`) and `*.proj.cc` (definitions, `:209`) at **global** scope today, each function already returning `routingmeta::ProjResult` and taking `routingmeta::MetadataSink&`. The `#include`s are printed first in each file; the per-message loop prints one `ProjectMeta` per message (skipping messages with neither projected scalars nor a context field).
- **`example/src/common/common_headers.h`** — `struct Runtime { correlation_id; site_id; tool_id; }` and `inline void FillCommon(const Runtime&, routingmeta::MetadataSink&)` at **global** scope (`:14`, `:20`). Both move into `routingmeta`.
- **`example/sender/unified_sender.cc`** — demo `Send` template (`:33`, global) takes `const Runtime& rt`; `main()` builds `Runtime` values at `:59/77/85/95`. Calls `FillCommon`/`ProjectMeta` unqualified (`:34/35`).
- **`example/tests/test_projection.cc`** — one `Runtime` use (`:186`, inside the common-headers test) `FillCommon(Runtime{…}, sink)`; many unqualified `ProjectMeta(req, sink)` calls.
- **`example/receiver/receiver_verify.cc`** — one unqualified `ProjectMeta(req, sink)` (`:31`); no `Runtime`/`FillCommon`. Needs no edit (ADL resolves the call post-move).

### What must be preserved (system still works end-to-end)
- **Wire frozen (CR1/AD-9):** namespacing is compile-time only; the emitted headers, values, digest, count/format, overflow flag are byte-identical. `unified_sender`/`receiver_verify` digests must match the pre-change run exactly.
- **Unqualified call sites stay unqualified (AD-3):** do NOT "fix" the `ProjectMeta(req, sink)` / `FillCommon(rt, sink)` calls by adding `routingmeta::` — the whole point is that ADL resolves them. Qualifying them would defeat the AC3 demonstration.
- **Negative-codegen gate stays green:** the plugin's `Validate()` and the `tests/negative/*` rejection are unaffected; the wrapper goes around output emission, not validation.
- **Return value still discardable (CR2/AD-10):** unchanged.

### Guardrails (do NOT do in this story)
- Do NOT move `Send` into the kit or change its body (Story 1.4) — only qualify its `Runtime` parameter type so the demo keeps compiling.
- Do NOT single-source the HPACK constant (Story 1.5), populate `duration` (1.6), or add the bench (1.7).
- Do NOT add a `using namespace routingmeta;` to dodge the type qualification — qualify `Runtime` explicitly (a blanket using-directive hides the ADL point and risks ambiguity).
- Do NOT change any proto `package` or the `ns_of()` logic — the request's package is irrelevant to resolution (that's the point).

### Exact shape to produce (reference)
Generated `*.proj.h`:
```cpp
#pragma once
#include "<base>.pb.h"
#include "common/metadata_sink.h"
#include "common/proj_result.h"

namespace routingmeta {
routingmeta::ProjResult ProjectMeta(const <pkg>::<Msg>& req, routingmeta::MetadataSink& sink);
// … one per message …
}  // namespace routingmeta
```
`common_headers.h`:
```cpp
namespace routingmeta {
struct Runtime { std::string correlation_id, site_id, tool_id; };
inline void FillCommon(const Runtime& rt, routingmeta::MetadataSink& sink) { /* unchanged */ }
}  // namespace routingmeta
```
Call sites: `FillCommon(routingmeta::Runtime{…}, sink)` and `ProjectMeta(req, sink)` — call names unqualified, `Runtime` type qualified.

### Testing standards
- Same harness (`example/tests/test_projection.cc`, plain `assert`, `ALL TESTS PASSED`), built by `example/build.sh`. AC3 is proven by the existing unqualified calls compiling + resolving after the move; no new assertion logic is required. Wire-identity (CR1) is proven by unchanged `unified_sender`/`receiver_verify` digests.

### Project Structure Notes
- No new files. Edits: `src/plugin/protoc-gen-meta.cc`, `src/common/common_headers.h`, `sender/unified_sender.cc`, `tests/test_projection.cc`. `receiver/receiver_verify.cc` unchanged.

### Previous story intelligence (Stories 1.1, 1.2)
- 1.1 pivoted generated `ProjectMeta` to return `routingmeta::ProjResult`; 1.2 made `EmitProcessContexts` return a `bool` overflow signal mapped to `Issue{Overflow}`. Both kept `ProjectMeta` at **global** scope — this story moves it into `routingmeta` (the spine flagged it global-must-move in AD-2). The function bodies from 1.1/1.2 are unchanged; only the enclosing namespace changes.
- Pattern continuity: the generated code already references kit symbols as `routingmeta::…`; wrapping in `namespace routingmeta` keeps those valid with no body edits.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.3] — user story + 4 ACs.
- [Source: ARCHITECTURE-SPINE.md#AD-2] — one coherent lib namespace; generated `ProjectMeta` + `Runtime`/`FillCommon` move into `routingmeta`; `*.proj.h` wraps output + includes kit headers.
- [Source: ARCHITECTURE-SPINE.md#AD-3] — unqualified `ProjectMeta`/`Send` resolve by ADL on the always-`routingmeta` sink; resolution independent of the request's package.
- [Source: ARCHITECTURE-SPINE.md#AD-9] — wire frozen (CR1).
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:175-188,191-256] — `.proj.h`/`.proj.cc` emit blocks to wrap.
- [Source: grpc-routing-meta/example/src/common/common_headers.h:14,20] — `Runtime` + `FillCommon` to move.
- [Source: grpc-routing-meta/example/sender/unified_sender.cc:33,59,77,85,95] — `Runtime` type refs to qualify; `:34,35` calls stay unqualified.
- [Source: grpc-routing-meta/example/tests/test_projection.cc:186] — `Runtime{…}` to qualify.
- [Source: grpc-routing-meta/example/receiver/receiver_verify.cc:31] — unqualified `ProjectMeta`, no edit (ADL).

### Latest tech notes
No external research. ADL (argument-dependent lookup / Koenig lookup) is standard C++ name resolution — function calls search the namespaces of their argument types; type names do not. C++17, protobuf 3.20/3.21 pinned. No new APIs or deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer)

### Debug Log References

- Captured baseline `unified_sender` digests pre-change; after the move `diff` of the signature lines (digests/overflow/mask-id) → IDENTICAL (CR1 held).
- Red→green was the build: wrapping `Runtime` in `routingmeta` broke the unqualified `Runtime` type uses; qualifying them (Task 3) reached green. Unqualified `ProjectMeta`/`FillCommon` calls compiled unchanged — ADL on the `routingmeta::` sink/Runtime args (AC3 proof).
- Verified generated `*.proj.h` wraps decls in `namespace routingmeta {` with `#include`s outside; `ProjectMeta` decl+def both inside the namespace.

### Completion Notes List

- Ultimate context engine analysis completed - comprehensive developer guide created.
- **AC1** — Plugin now emits `namespace routingmeta { … }` around the `ProjectMeta` declarations (`.proj.h`) and definitions (`.proj.cc`), with the kit `#include`s kept outside the namespace. Confirmed in generated `sys1/sys2/sys3.proj.{h,cc}`.
- **AC2** — `struct Runtime` and `FillCommon` moved into `namespace routingmeta` in `common_headers.h`.
- **AC3** — Call sites keep `ProjectMeta(req, sink)` / `FillCommon(rt, sink)` **unqualified**; they resolve by ADL on the `routingmeta::MetadataSink` (and `routingmeta::Runtime`) arguments, independent of the request's package (`sys1::v1`/`sys3::v1`/…). Proven by the build linking with unqualified calls in `unified_sender.cc`, `receiver_verify.cc`, and `test_projection.cc`. Only `Runtime` **type-name** uses were qualified (`routingmeta::Runtime`) — ADL doesn't apply to type names.
- **AC4** — `ALL TESTS PASSED`; `unified_sender`/`receiver_verify` wire output byte-identical to baseline (digests unchanged) → CR1; negative-codegen gate green; all binaries link.
- `receiver/receiver_verify.cc` needed NO change — its unqualified `ProjectMeta(req, sink)` resolves by ADL on `routingmeta::VectorSink`.
- Scope held: `Send` stays a global demo template (only its `Runtime` param qualified) — promotion to the kit is Story 1.4; HPACK single-source (1.5), `duration` (1.6) untouched.

### File List

- `grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc` (MODIFIED — wrap generated `ProjectMeta` decls/defs in `namespace routingmeta`)
- `grpc-routing-meta/example/src/common/common_headers.h` (MODIFIED — `Runtime` + `FillCommon` into `namespace routingmeta`)
- `grpc-routing-meta/example/sender/unified_sender.cc` (MODIFIED — qualify 5 `Runtime` type refs; calls left unqualified)
- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — qualify 1 `Runtime` type ref)

## Change Log

- 2026-06-28 — Story 1.3 implemented: generated `ProjectMeta` + `Runtime`/`FillCommon` moved into `namespace routingmeta`; unqualified call sites resolve by ADL on the `routingmeta::` sink/Runtime args (AD-2/AD-3). Wire byte-identical (CR1); `receiver_verify.cc` unchanged. All ACs met; full suite green, no regression.
