---
baseline_commit: b7ad9f91c79d4d3934e76eb2b7c39e61d57b17d6
---

# Story 1.6: `ProjectMeta` self-times and reports `duration`

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As an operator/tracer,
I want every projection to report its own measured duration,
so that I get a per-call latency signal with no coupling to the kit.

## Acceptance Criteria

1. **AC1 — `ProjectMeta` self-times its projection.** Given `ProjectMeta`, when it runs, then it measures its own projection time with a steady clock and populates `ProjResult.duration` (> 0). (FR7, AD-6)

2. **AC2 — One timing point; `Send` does not time.** Given the demo (`unified_sender`) calling `routingmeta::Send`, when it runs, then `Send` does NOT time the projection; it only propagates `ProjResult.duration`. (one timing point; AD-6)

3. **AC3 — Kit does no logging/metrics.** Given the kit, when inspected, then it performs no logging or metrics itself — `duration` is read by the caller. (NFR7)

## Tasks / Subtasks

- [x] **Task 1 — Emit self-timing in the generated `ProjectMeta` (AC: 1)** — edit `example/src/plugin/protoc-gen-meta.cc`
  - [x] Add `#include <chrono>` to the generated `.proj.cc` include block (currently `protoc-gen-meta.cc:202`, the `"#include <vector>\n#include <string>\n\n"` Print).
  - [x] After the emitted `routingmeta::ProjResult result;` (currently `:213`), emit `const auto _proj_t0 = std::chrono::steady_clock::now();`.
  - [x] Before the emitted `return result;` (currently `:261`), emit `result.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _proj_t0);`.
  - [x] Use `steady_clock` (monotonic) and `duration_cast<nanoseconds>` — the cast keeps it portable regardless of `steady_clock::duration`'s native period. The timing brackets the whole projection (scalars + context emit), not the trivial `result` construction.

- [x] **Task 2 — Update the `duration` field comment (AC: 1, 3)** — edit `example/src/common/proj_result.h`
  - [x] Change the `duration{}` comment (`proj_result.h:24`, "self-timed by ProjectMeta (story 1.6); 0 until then") to state it is now populated by `ProjectMeta`'s self-timing, read by the caller (no kit logging/metrics — NFR7). Field type and default unchanged.

- [x] **Task 3 — Guardrail tests (AC: 1, 2)** — edit `example/tests/test_projection.cc`
  - [x] On a real projection (the existing sys1 2-context case, `r = ProjectMeta(sys1Req(2), sink)`), assert `r.duration.count() > 0` — a multi-context projection (string build + url-encode + sha256) always takes measurable time, so this is robust, not flaky.
  - [x] On the existing `routingmeta::Send` guardrail (happy-path sys3), assert the returned `r.duration.count() > 0` — proves `Send` propagates `ProjectMeta`'s `duration` (it does not zero or re-time it). (AC2)
  - [x] Do NOT assert an upper bound here — sub-millisecond proof is Story 1.7's bench.

- [x] **Task 4 — Build & verify (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` → links; `./build/test_projection` → `ALL TESTS PASSED`.
  - [x] `./build/unified_sender` → the per-call `duration=…ns` lines now show a NON-zero value (the empty-mask block's `-> ok=false … duration=…ns` is no longer `0ns`). Wire output (headers/digests/byte counts) byte-identical (CR1 — duration is in-process only, never a header).
  - [x] `grep -rn "steady_clock\|chrono::now\|::now()" src/common/send.h` → none (Send does not time — AC2). `grep -rn "printf\|cout\|log" src/common` → no kit logging (NFR7); the only prints are in the demo/tests.

### Review Findings

_Code review 2026-06-28 (Blind Hunter + Edge Case Hunter + Acceptance Auditor). 0 patch, 0 decision-needed, 1 deferred, 2 dismissed. Acceptance Auditor: PASS — 3/3 ACs, scope intact. Edge Case Hunter: every generated variant sets `duration` before its single return (sys1 1/1, sys2 5/5, sys3 10/10), no early-return skips it, `send.h` does not time, timing uses only locals (re-entrant). All reviewers agree: no shipping-code bug._

- [x] [Review][Defer] The `duration.count() > 0` test assertion is clock-granularity-fragile [grpc-routing-meta/example/tests/test_projection.cc:55,208] — deferred to **Story 1.7** (bench owns rigorous timing). `> 0` encodes "timing ran" with no floor; on a coarse `steady_clock` it could round to 0. Safe on every in-scope platform — Linux CI (`steady_clock` = ns resolution, AD-14) and the macOS dev host (the asserted projections do sha256 + 14 url-encodes, ≫ the ~41 ns local granularity) — and AC1 literally specifies `> 0`. When 1.7 adds `bench_projection`, use resolution-robust timing checks; a future coarse-clock port (e.g. Windows, out of scope) would also revisit this.

_Dismissed (2):_
- _Blind Hunter "`Send` doesn't propagate `duration`" — false positive from a context-free review. `routingmeta::Send` is `FillCommon(rt, sink); return ProjectMeta(req, sink);` — it returns `ProjectMeta`'s `ProjResult` (with its timed `duration`) unchanged; the Edge Case Hunter and Acceptance Auditor (repo access) confirmed propagation, and `test_projection.cc:208` passes._
- _"`assert` voids under `NDEBUG`" — pre-existing project-wide convention (the whole zero-test-deps harness is `assert`-based); `build.sh` compiles `-O2 -Wall` with no `-DNDEBUG`, so the asserts are live. Not introduced by 1.6._

## Dev Notes

### Method (Amelia)
Red → green: add the `r.duration.count() > 0` assertion first → it fails (duration is still 0) → emit the self-timing in the plugin → green. Done when `test_projection` passes and `unified_sender` shows non-zero durations with unchanged wire output.

### The single timing point (AD-6)
- ONLY `ProjectMeta` times. It brackets its own projection with `steady_clock::now()` at entry (after `result` is constructed) and exit (before `return`), writing the delta into `ProjResult.duration`.
- `Send` (`src/common/send.h`) MUST NOT time — it returns `ProjectMeta`'s `ProjResult` unchanged, so `duration` propagates with no second measurement. It already does exactly this (`return ProjectMeta(req, sink);`) — **no change to `send.h`**. Adding a clock in `Send` would create two inconsistent timing points; do not.
- The kit emits no logs/metrics (NFR7); it only populates the field. The caller (demo/operator) reads it.

### Why `steady_clock` + `duration_cast<nanoseconds>`
- `steady_clock` is monotonic — correct for measuring elapsed time (system_clock can jump). `proj_result.h` already `#include`s `<chrono>` and types `duration` as `std::chrono::nanoseconds`.
- `steady_clock::now() - _proj_t0` yields `steady_clock::duration`, whose period is implementation-defined. `duration_cast<std::chrono::nanoseconds>` converts portably (an implicit assignment could fail to compile on an implementation whose `steady_clock::duration` is coarser than `nanoseconds`).
- `> 0` is reliable for a real projection (microseconds of work). Do not try to force `> 0` for a hypothetical zero-work projection — the plugin only emits `ProjectMeta` for messages that actually project something.

### Current state of the files this story changes (read before editing)
- **`example/src/plugin/protoc-gen-meta.cc`** — emits the generated `ProjectMeta` inside `namespace routingmeta` (Story 1.3). The `.proj.cc` body opens with `routingmeta::ProjResult result;` (`:213`) and ends `return result;` (`:261`); includes block at `:202` has `<vector>`/`<string>` (no `<chrono>` yet). The scalar branches (1.1) and the overflow→Issue context block (1.2) sit between; the timing wraps all of it.
- **`example/src/common/proj_result.h`** — `std::chrono::nanoseconds duration{};` (`:24`), default 0; `#include <chrono>` present. Only the comment changes.
- **`example/sender/unified_sender.cc`** — already prints `duration=%lldns` for the empty-mask block (Story 1.4). After this story that value is non-zero. No code change needed (the field is now populated upstream).
- **`example/tests/test_projection.cc`** — has the sys1 2-context happy-path block and the `routingmeta::Send` guardrail (Story 1.4); both already capture `auto r = …`. Add the `duration > 0` asserts there.
- **`example/src/common/send.h`** — `return ProjectMeta(req, sink);`; no clock. Unchanged.

### What must be preserved (system still works end-to-end)
- **Wire frozen (CR1/AD-9):** `duration` is an in-process `ProjResult` field, NEVER emitted as a header. Headers, digests, count/format/overflow, and `unified_sender` byte counts are byte-identical.
- **One timing point (AD-6):** do not time in `Send` or anywhere but `ProjectMeta`. `bench_projection` (Story 1.7) will measure `ProjectMeta` directly.
- **No new runtime deps (NFR3):** `<chrono>` is stdlib. No OpenSSL, no metrics lib.
- **Re-entrancy (NFR5/AD-12):** the timing uses only locals (`_proj_t0`), no shared/static state — `ProjectMeta` stays pure and re-entrant.

### Guardrails (do NOT do in this story)
- Do NOT add `bench_projection` or any sub-millisecond assertion (Story 1.7).
- Do NOT time inside `Send`, `FillCommon`, or `EmitProcessContexts` — only `ProjectMeta`.
- Do NOT add logging/metrics to the kit (NFR7) or change the `duration` field type.
- Do NOT emit `duration` (or any timing) on the wire.

### Exact shape to produce (generated `ProjectMeta`, reference)
```cpp
routingmeta::ProjResult ProjectMeta(const <pkg>::<Msg>& req, routingmeta::MetadataSink& sink) {
  routingmeta::ProjResult result;
  const auto _proj_t0 = std::chrono::steady_clock::now();
  … scalar branches … context block …
  result.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - _proj_t0);
  return result;
}
```
(plus `#include <chrono>` in the generated `.proj.cc`).

### Testing standards
- `example/tests/test_projection.cc`, plain `assert`, `ALL TESTS PASSED`, via `example/build.sh`. Assert `duration > 0` on a real multi-context projection and on a `Send` call (propagation). No upper-bound assertion (that's the bench, 1.7).

### Project Structure Notes
- No new files. Edits: `src/plugin/protoc-gen-meta.cc`, `src/common/proj_result.h`, `tests/test_projection.cc`. `unified_sender.cc`/`send.h` unchanged.

### Previous story intelligence (Stories 1.1–1.5)
- 1.1 added `ProjResult.duration` (default 0). 1.4 made `Send` propagate it and the demo print `duration=…ns` (showed `0ns` — explicitly "until 1.6"). This story populates it; the demo line becomes non-zero with no demo change. 1.3's namespace wrap means the generated body is inside `namespace routingmeta` — `std::chrono::…` is fully qualified, so it resolves fine there. 1.7's bench will measure `ProjectMeta` directly and assert sub-ms (depends on this).

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.6] — user story + 3 ACs.
- [Source: ARCHITECTURE-SPINE.md#AD-6] — `duration` self-timed by `ProjectMeta`; `bench_projection` measures it directly; `Send` propagates, does not separately time (one timing point).
- [Source: ARCHITECTURE-SPINE.md#NFR7] — kit performs no logging/metrics; caller reads `issues`/`duration`.
- [Source: ARCHITECTURE-SPINE.md#AD-12] — `ProjectMeta` pure/re-entrant; the timing uses only locals.
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:202,213,261] — generated `.proj.cc` includes, `result` decl, `return result;`.
- [Source: grpc-routing-meta/example/src/common/proj_result.h:24] — `duration` field + comment.
- [Source: grpc-routing-meta/example/src/common/send.h] — propagates `ProjResult` (no timing — leave as-is).
- [Source: grpc-routing-meta/example/sender/unified_sender.cc] — already prints `duration=…ns` (becomes non-zero).

### Latest tech notes
No external research. `std::chrono::steady_clock` + `duration_cast` is standard C++ time measurement; monotonic, ns-resolution on Linux/macOS (the CI matrix). C++17, protobuf 3.20/3.21 pinned. No new deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer) — engineering delegated to a general-purpose subagent; main loop did setup, verification, and BMad bookkeeping.

### Debug Log References

- Implementation delegated to a subagent (per user request); main loop independently re-verified before marking review.
- Red (subagent): with the `duration.count() > 0` asserts added but the plugin unchanged, `test_projection` aborts at `test_projection.cc:55` (`r.duration.count() > 0`).
- Green (independently re-run in main loop): clean `./build.sh` OK; `test_projection` → `ALL TESTS PASSED`; generated `sys1.proj.cc` shows `#include <chrono>` (:6), `_proj_t0 = steady_clock::now()` (:14) after `result`, and `result.duration = duration_cast<nanoseconds>(steady_clock::now() - _proj_t0)` (:31) before return.
- Scope verified: `git diff --stat` shows exactly 3 files (plugin, proj_result.h, test); `send.h`/`unified_sender.cc` untouched. `grep steady_clock src/common/send.h` → none (AC2). `unified_sender` empty-mask block now prints `duration=542ns` (was `0ns`); byte counts `875/700/449/499/513/514` unchanged (CR1); `receiver_verify` digest OK.

### Completion Notes List

- Ultimate context engine analysis completed - comprehensive developer guide created.
- **AC1** — Generated `ProjectMeta` brackets its projection with `std::chrono::steady_clock` and writes `result.duration = duration_cast<nanoseconds>(...)`; test asserts `duration.count() > 0` on the sys1 2-context projection.
- **AC2** — `send.h` unchanged (no clock) — one timing point; the `routingmeta::Send` guardrail asserts the returned `duration.count() > 0`, proving `Send` propagates `ProjectMeta`'s timing unmodified.
- **AC3** — Kit emits no logging/metrics (only the demo/tests print); `proj_result.h` comment updated to say `duration` is populated by `ProjectMeta` and read by the caller (NFR7). Field type/default unchanged. Re-entrancy preserved — timing uses only the local `_proj_t0` (NFR5/AD-12).
- Wire frozen (CR1): `duration` is in-process only, never a header; demo digests/byte counts byte-identical.
- Scope held: no bench (1.7), no timing in `Send`/`FillCommon`/`EmitProcessContexts`, no logging.

### File List

- `grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc` (MODIFIED — generated `ProjectMeta` self-times via `steady_clock`; `#include <chrono>` in `.proj.cc`)
- `grpc-routing-meta/example/src/common/proj_result.h` (MODIFIED — `duration` field comment)
- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — `duration > 0` asserts on a real projection and on `Send`)

## Change Log

- 2026-06-28 — Story 1.6 implemented: generated `ProjectMeta` self-times its projection with `steady_clock` and populates `ProjResult.duration` (>0); `Send` propagates it untimed (one timing point, AD-6); kit does no logging (NFR7). Wire byte-identical (CR1). All ACs met; full suite green (FR7). Engineering done by a subagent, independently verified in the main loop.
