---
baseline_commit: f4a0c4af6f04e791073c5f53cc434bb12b16f704
---

# Story 1.4: One branchless `routingmeta::Send` lives in the kit

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sender developer,
I want a single branchless `Send<>()` serving sys1/sys2/sys3 with zero per-system code that propagates `ProjResult` and never throws,
so that all three systems go out one identical path and I own the abort/proceed decision.

## Acceptance Criteria

1. **AC1 — Kit ships `routingmeta::Send`; demo calls it.** Given the kit, when searched, then the kit declares a single branchless `routingmeta::Send` (= `FillCommon` + `ProjectMeta`, returns `ProjResult`, no-throw); `unified_sender.cc` calls it (its own local `Send` template is gone). (FR6, AD-4)

2. **AC2 — Branchless, zero per-system code.** Given the `Send<>()` template, when inspected, then it is a single template = `FillCommon` + `ProjectMeta`, with zero `if (system==…)` branching, serving sys1/sys2/sys3. (BRIEF E, CONTEXT inv. 10)

3. **AC3 — No throw; propagates `ProjResult`.** Given a request with an empty required scalar, when `Send` runs, then it does not throw; it propagates the `ProjResult` (`ok=false` + `x-routing-error`) for the caller to act on. (FR1 binds `Send` too; AD-5)

4. **AC4 — Demo proves it.** Given `./build/unified_sender`, when run, then it prints the system blocks and the empty sys3 mask surfaces `x-routing-error` + a duration. (BRIEF Verify line 2)

## Tasks / Subtasks

- [x] **Task 1 — Add the kit `routingmeta::Send` (AC: 1, 2, 3)** — create `example/src/common/send.h`
  - [x] Header in `namespace routingmeta`, `#pragma once`, including `common/common_headers.h` (Runtime + FillCommon), `common/metadata_sink.h`, `common/proj_result.h`.
  - [x] `template <class Req> ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) { FillCommon(rt, sink); return ProjectMeta(req, sink); }`
  - [x] Do NOT `#include` any `*.proj.h` — `ProjectMeta(req, sink)` is a **dependent** call resolved by ADL on the `MetadataSink` argument at the instantiation point (where the caller has included the generated header). This preserves the kit→generated dependency direction (AD-4/AR3) and only works because of Story 1.3's ADL (AD-3).
  - [x] Branchless: one template, no `if (system==…)`, returns `ProjResult` (propagates `ProjectMeta`'s result, incl. `duration`); no `throw` (neither `FillCommon` nor `ProjectMeta` throws on a data condition).

- [x] **Task 2 — Make `unified_sender.cc` a thin demo that calls the kit `Send` (AC: 1, 2, 4)** — edit `example/sender/unified_sender.cc`
  - [x] Delete the local `Send` template (`unified_sender.cc:32-36`) and `#include "common/send.h"`.
  - [x] Leave the five existing `Send(req, rt, sink)` call sites unqualified — they now resolve to `routingmeta::Send` by ADL on the `routingmeta::Runtime`/`routingmeta::MetadataSink` args (AD-3). They may discard the return (CR2).
  - [x] Add a demo block: a sys3 `Submit05Request` with an **empty** mask, call `auto r = Send(req, routingmeta::Runtime{…}, sink);` (no throw), `dump(...)` the sink (shows `x-routing-error`), then print `r.ok` / `r.issues.size()` / `r.duration.count()` so the empty mask surfaces `x-routing-error` + a duration (AC4 / BRIEF Verify line 2). `duration` is 0 until Story 1.6 — that is expected; `Send` only **propagates** it (AD-6: Send does not time).

- [x] **Task 3 — Guardrail test for Send (AC: 2, 3)** — edit `example/tests/test_projection.cc`
  - [x] `#include "common/send.h"`.
  - [x] Happy path: `routingmeta::Send(populatedSubmit05, routingmeta::Runtime{…}, sink)` → assert `ok`, `issues` empty, `x-mask-id` present (ProjectMeta ran) AND a common header e.g. `x-tool-id` present (FillCommon ran) — proves `Send = FillCommon + ProjectMeta`.
  - [x] Failure-as-data: `routingmeta::Send(emptyMaskSubmit05, routingmeta::Runtime{…}, sink)` must NOT throw; assert `!ok`, one `Issue{MissingRequired}`, `x-routing-error == "missing:x-mask-id"` — proves FR1 binds `Send`.
  - [x] Keep it plain `assert`, `ALL TESTS PASSED`.

- [x] **Task 4 — Build & verify (AC: 1, 2, 3, 4)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` — links all binaries (send.h is header-only; no build-script change needed).
  - [x] `./build/test_projection` → `ALL TESTS PASSED` (incl. the new Send guardrail).
  - [x] `./build/unified_sender` → prints the system blocks; the empty-sys3 block shows `x-routing-error: missing:x-mask-id` and a duration line; no crash/throw. The pre-existing blocks' digests are unchanged (CR1).
  - [x] `./build/receiver_verify` → digest OK.

## Dev Notes

### Method (Amelia)
Red → green: write the Task 3 Send guardrail first → it won't compile until `send.h` exists (red) → add `send.h` + wire the demo (green). Done when `test_projection` prints `ALL TESTS PASSED` and `unified_sender` surfaces the empty-mask `x-routing-error` + duration without throwing.

### The load-bearing detail: why `send.h` includes no generated headers
- `Send` is a template; `ProjectMeta(req, sink)` inside it is a **type-dependent** call (depends on `Req`). Its name is resolved by **ADL at the point of instantiation**, not definition. At each call site (`unified_sender.cc`, `test_projection.cc`) the relevant `*.proj.h` is already included, so the generated `routingmeta::ProjectMeta` overload is visible and found via the `routingmeta::MetadataSink` argument.
- Therefore `send.h` must NOT `#include` `sys1.proj.h` etc. — the kit (shared lib) cannot depend on per-system generated code (AD-4/AR3 dependency direction). This is the architectural payoff of Story 1.3 (AD-2/AD-3 namespace+ADL): without it, `Send` in the kit could not find `ProjectMeta`.
- `FillCommon(rt, sink)` is a **non-dependent** call (concrete `Runtime`/`MetadataSink`); it resolves at definition via ordinary lookup (both are in `routingmeta`, included by `send.h`).

### Current state of the files this story changes (read before editing)
- **`example/sender/unified_sender.cc`** — currently defines its OWN `Send` template (`:32-36`, returns `void`, `FillCommon` + `ProjectMeta`) and calls it at `:68/77/85/95/106`. Has `dump()` (`:39-44`) and `fillCtx()` (`:48-56`). The sys3 block (`:89-97`) uses a **populated** mask (`RET-9981`). This file becomes a thin demo: delete the local `Send`, include the kit's, add the empty-mask block. `Runtime` is already `routingmeta::Runtime` (Story 1.3).
- **`example/src/common/common_headers.h`** — `routingmeta::Runtime` + `routingmeta::FillCommon` (moved into the namespace in Story 1.3). `send.h` includes this.
- **`example/src/common/proj_result.h`** — `routingmeta::ProjResult { bool ok; vector<Issue> issues; chrono::nanoseconds duration; }`. `Send` returns this.
- **`example/tests/test_projection.cc`** — plain-assert harness; will gain the Send guardrail + a `send.h` include.
- **`example/src/common/metadata_sink.h`** — `MetadataSink`/`VectorSink` in `routingmeta`; ADL anchor for resolving `ProjectMeta`/`Send`.

### What must be preserved (system still works end-to-end)
- **Wire frozen (CR1/AD-9):** `Send` adds no headers of its own — it is exactly `FillCommon` + `ProjectMeta`. Existing demo blocks' output/digests are byte-identical. The empty-mask block is NEW demo output (the `x-routing-error` it shows is the already-defined missing-required header, not a new wire element).
- **No-throw (FR1/AD-5):** `Send` must propagate `ProjResult` on a data condition, never throw. The empty-mask path is the proof.
- **Branchless (BRIEF E / CONTEXT inv. 10):** one template, zero per-system `if`. Do not add system dispatch.
- **Return discardable (CR2/AD-10):** the five existing call sites discard `Send`'s return and must keep compiling; do not add `[[nodiscard]]`.
- **One timing point (AD-6):** `Send` must NOT time the projection; it only propagates `ProjResult.duration`. (Self-timing is Story 1.6.)

### Guardrails (do NOT do in this story)
- Do NOT self-time / populate `duration` (Story 1.6) — print whatever `ProjResult.duration` carries (0 for now).
- Do NOT single-source the HPACK constant (Story 1.5), add the bench (1.7), or change projection/overflow behavior.
- Do NOT make the kit `Send` depend on generated `*.proj.h` (breaks AD-4 dependency direction) — rely on ADL.
- Do NOT add per-system branching or a non-template overload set; one branchless template only.

### Exact shape to produce (reference)
`src/common/send.h`:
```cpp
#pragma once
#include "common/common_headers.h"   // routingmeta::Runtime + FillCommon
#include "common/metadata_sink.h"     // routingmeta::MetadataSink
#include "common/proj_result.h"       // routingmeta::ProjResult

namespace routingmeta {
// One branchless sender path for every request type: FillCommon + generated
// ProjectMeta, returns ProjResult, no throw on a data condition (AD-4/AD-5).
// ProjectMeta resolves by ADL on the MetadataSink arg at instantiation, so the kit
// needs no per-system generated headers (AD-3) — dependency direction preserved.
template <class Req>
ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  FillCommon(rt, sink);
  return ProjectMeta(req, sink);
}
}  // namespace routingmeta
```
Demo empty-mask block (sketch):
```cpp
sys3::v1::Submit05Request req;                         // mask_id left empty
routingmeta::VectorSink sink;
auto r = Send(req, routingmeta::Runtime{"CORR-sys3-005","F18","LITHO01"}, sink);  // no throw
dump("sys3 Submit05 (EMPTY mask -> x-routing-error)", sink);
std::printf("  -> ok=%s  issues=%zu  duration=%lldns\n\n",
            r.ok ? "true" : "false", r.issues.size(), (long long)r.duration.count());
```

### Testing standards
- `example/tests/test_projection.cc`, plain `assert`, `ALL TESTS PASSED`, built by `example/build.sh`. The Send guardrail proves `Send = FillCommon + ProjectMeta` (both a common header and the projected header present) and the no-throw failure-as-data path.

### Project Structure Notes
- New file `src/common/send.h` (the named kit public entry point per AR3). Header-only — no `build.sh`/`CMakeLists.txt` change. Matches the Structural Seed (`src/common/ … Send (one branchless wrapper, returns ProjResult)`).

### Previous story intelligence (Stories 1.1–1.3)
- 1.1 `ProjResult`/no-throw; 1.2 overflow→`Issue`; **1.3 namespace + ADL** — the direct enabler: `Send` in the kit resolves `ProjectMeta` only because it lives in `routingmeta` and is found by ADL on the sink.
- **Deferred items this story closes:** the 1.1 and 1.2 reviews deferred "caller acts on `ProjResult` / surfaces `x-routing-error`" to Story 1.4 (see `deferred-work.md`). This story's demo empty-mask block + the Send guardrail satisfy that — the caller now reads `ok`/`issues` and the demo surfaces `x-routing-error`.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.4] — user story + 4 ACs.
- [Source: ARCHITECTURE-SPINE.md#AD-4] — one branchless `routingmeta::Send` in the kit; caller owns abort/proceed; `unified_sender.cc` becomes a thin demo calling it.
- [Source: ARCHITECTURE-SPINE.md#AD-3] — ADL resolves `Send`'s unqualified `ProjectMeta` on the sink; independent of request package.
- [Source: ARCHITECTURE-SPINE.md#AD-5] — FR1 binds `Send`: no throw, propagate `ProjResult`.
- [Source: ARCHITECTURE-SPINE.md#AD-6] — one timing point: `Send` does not time, only propagates `duration`.
- [Source: refs/BRIEF.md:64] — Verify line 2: `unified_sender` → 3 system blocks; empty sys3 mask → `x-routing-error` + duration.
- [Source: grpc-routing-meta/example/sender/unified_sender.cc:32-36,89-97] — local `Send` to remove; sys3 block to mirror for the empty case.
- [Source: grpc-routing-meta/example/src/common/common_headers.h] — `routingmeta::Runtime` + `FillCommon`.
- [Source: grpc-routing-meta/example/src/common/proj_result.h] — `ProjResult` return type.
- [Source: _bmad-output/implementation-artifacts/deferred-work.md] — caller-acts-on-result deferral (1.1/1.2 → 1.4).

### Latest tech notes
No external research. The mechanism is standard C++ template two-phase lookup: a dependent unqualified call resolves by ADL at instantiation. C++17, protobuf 3.20/3.21 pinned. No new deps; `send.h` is header-only.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer)

### Debug Log References

- Red: added the Send guardrail + `#include "common/send.h"` → build fails `'common/send.h' file not found`.
- Green: added `src/common/send.h`, removed the demo's local `Send`, added the empty-mask demo block → `ALL TESTS PASSED`.
- Demo empty-mask block prints `x-routing-error: missing:x-mask-id` and `-> ok=false  issues=1  duration=0ns` (no throw). sys1/sys2 digests byte-identical to the 1.3 baseline (CR1). `receiver_verify` OK.
- Verified `send.h` includes only kit headers — no `*.proj.h` (dependency direction AD-4/AR3); the demo's 6 `Send(...)` call sites compile via ADL on the sink (no local `Send`).

### Completion Notes List

- Ultimate context engine analysis completed - comprehensive developer guide created.
- **AC1** — Added `src/common/send.h`: `template<class Req> routingmeta::ProjResult Send(const Req&, const Runtime&, MetadataSink&)` = `FillCommon` + `ProjectMeta`, returns `ProjResult`, no-throw. `unified_sender.cc`'s local `Send` template deleted; it now calls the kit `Send`.
- **AC2** — One branchless template, zero `if (system==…)`; serves sys1/sys2/sys3 (the demo routes all of them through it; the test drives sys3).
- **AC3** — Guardrail test: `routingmeta::Send` on an empty required mask does NOT throw, returns `ok=false` + one `Issue{MissingRequired}` + `x-routing-error: missing:x-mask-id`; happy path returns `ok` with `x-mask-id` (ProjectMeta) AND `x-tool-id` (FillCommon) present → proves `Send = FillCommon + ProjectMeta`.
- **AC4** — `./build/unified_sender` prints the system blocks and a new empty-sys3 block surfacing `x-routing-error: missing:x-mask-id` + a duration line (`duration=0ns` — `Send` only propagates it; self-timing is Story 1.6, AD-6).
- **Closed deferrals:** the 1.1/1.2 review "caller acts on `ProjResult` / surfaces `x-routing-error`" items (deferred-work.md) are satisfied — the demo captures the result and surfaces the error.
- Key mechanism: `ProjectMeta(req, sink)` inside the kit `Send` template is resolved by ADL on the `MetadataSink` arg at instantiation, so `send.h` includes NO generated headers (verified). Wire frozen (CR1); no `[[nodiscard]]` (CR2). Scope held: HPACK (1.5), `duration` self-timing (1.6), bench (1.7) untouched.

### File List

- `grpc-routing-meta/example/src/common/send.h` (NEW — `routingmeta::Send`, branchless, ADL-resolved `ProjectMeta`, no generated-header include)
- `grpc-routing-meta/example/sender/unified_sender.cc` (MODIFIED — removed local `Send`, include kit `send.h`, added empty-mask demo block)
- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — `send.h` include + Send guardrail test)

## Change Log

- 2026-06-28 — Story 1.4 implemented: one branchless `routingmeta::Send` lives in the kit (`src/common/send.h`), = `FillCommon` + `ProjectMeta`, returns `ProjResult`, no-throw; `ProjectMeta` resolved by ADL at instantiation so the kit needs no generated headers. Demo is now a thin caller and surfaces the empty-mask `x-routing-error` + duration. Wire byte-identical (CR1). All ACs met; full suite green (FR6; AD-3/AD-4/AD-5/AD-6). Closes the 1.1/1.2 caller-acts-on-result deferrals.
