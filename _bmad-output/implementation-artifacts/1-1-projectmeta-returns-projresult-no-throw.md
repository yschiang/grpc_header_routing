---
baseline_commit: fc3dd4b9eec1e0bd254da463fd45640d48f017f9
---

# Story 1.1: `ProjectMeta` returns `ProjResult`, no throw

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sender developer,
I want `ProjectMeta` to report a missing required field as structured data instead of throwing,
so that a bad request is caught at the source and surfaced — never silent, never an exception I must catch.

## Acceptance Criteria

1. **AC1 — Result type + signature pivot.** Given the new `src/common/proj_result.h` defining `ProjResult{ bool ok; std::vector<Issue> issues; std::chrono::nanoseconds duration; }` and `Issue{ Kind(MissingRequired|Overflow), std::string key }`, when the kit builds, then the generated `ProjectMeta` is declared returning `routingmeta::ProjResult` (was `void`) **and** no `throw` exists on a data condition in the generated path. (FR1, AD-5)

2. **AC2 — Missing required scalar → data, not exception.** Given a sys3 request with an empty required `x-mask-id`, when `ProjectMeta` runs, then it returns `ok=false` with exactly one `Issue{MissingRequired, "x-mask-id"}`, emits header `x-routing-error: missing:x-mask-id`, **and** does NOT emit the empty `x-mask-id` header. (FR2, SPEC §7)

3. **AC3 — Happy path unchanged (no wire change).** Given a fully-populated request, when `ProjectMeta` runs, then `ok=true`, `issues` is empty, and all expected headers are emitted byte-for-byte unchanged. (CR1, AD-9)

4. **AC4 — Test pivots from throw to result.** Given `test_projection`, when it runs, then the former "asserts threw" case is replaced by asserting the `ProjResult` / `x-routing-error` behavior **and** the binary prints `ALL TESTS PASSED`. (BRIEF Verify)

## Tasks / Subtasks

- [x] **Task 1 — Add the failure-as-data result type (AC: 1)**
  - [x] Create `example/src/common/proj_result.h` in `namespace routingmeta` defining `Issue` (nested unscoped `enum Kind { MissingRequired, Overflow }`, `Kind kind`, `std::string key`) and `ProjResult` (`bool ok = true`, `std::vector<Issue> issues`, `std::chrono::nanoseconds duration{}`).
  - [x] Include only `<chrono>`, `<string>`, `<vector>`. No other kit header depends on it being pulled in transitively — keep it a leaf.
  - [x] Define the `Overflow` enum value now (the type is shared) but do NOT wire any overflow→issue plumbing — that is Story 1.2.

- [x] **Task 2 — Pivot the generated `ProjectMeta` to return `ProjResult`, no throw (AC: 1, 2, 3)** — edit `example/src/plugin/protoc-gen-meta.cc`
  - [x] `.proj.h` emit block: add `#include "common/proj_result.h"` to the generated header includes; change the declaration from `void ProjectMeta(...)` to `routingmeta::ProjResult ProjectMeta(...)` (currently `protoc-gen-meta.cc:185`).
  - [x] `.proj.cc` emit block: remove the generated `#include <stdexcept>` (currently `protoc-gen-meta.cc:199`) — the generated code no longer throws.
  - [x] Change the function open to `routingmeta::ProjResult ProjectMeta(...) {` and emit `routingmeta::ProjResult result;` as the first line (currently `protoc-gen-meta.cc:208`).
  - [x] Replace the **required** branch (currently `protoc-gen-meta.cc:213-218`, throw-then-emit) with: if empty → `result.ok = false; result.issues.push_back({routingmeta::Issue::MissingRequired, "<key>"}); sink.Add("x-routing-error", "missing:<key>");` else → `sink.Add("<key>", routingmeta::UrlEncode(<getter>));`. The empty header is emitted in NEITHER branch.
  - [x] Leave the **optional** branch (`:221`) and the process-context block (`:226-248`, `EmitProcessContexts`) unchanged — overflow stays void here (Story 1.2).
  - [x] Emit `return result;` before the closing brace (currently `:249`).

- [x] **Task 3 — Pivot the test from throw to result (AC: 2, 3, 4)** — edit `example/tests/test_projection.cc`
  - [x] Replace the sys3 required try/catch/`threw` block (currently `test_projection.cc:116-124`) with assertions that `ProjectMeta(empty, s2)` returns `ok=false`, `issues.size()==1`, `issues[0].kind == routingmeta::Issue::MissingRequired`, `issues[0].key == "x-mask-id"`, `s2.Get("x-routing-error") == "missing:x-mask-id"`, and `s2.Get("x-mask-id").empty()`.
  - [x] Capture the return on at least one populated case (e.g. the sys3 populated request at `:111` and/or sys1 at `:52`) and assert `r.ok == true` and `r.issues.empty()` (AC3).
  - [x] Remove the now-unused `#include <stdexcept>` (`test_projection.cc:8`).
  - [x] Other `ProjectMeta(...)` call sites may keep discarding the return (still compiles) — do not churn them.

- [x] **Task 4 — Build, regenerate, verify green (AC: 1, 2, 3, 4)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` — rebuilds `protoc-gen-meta`, reruns codegen (new `*.proj.{h,cc}` signatures), links all binaries.
  - [x] Run `./build/test_projection` → expect `ALL TESTS PASSED`.
  - [x] Run `./build/unified_sender` and `./build/receiver_verify` → confirm no regression (they must still run; `Send` discards the new return value — CR2/AD-10). Sender output bytes/headers unchanged vs. the happy path (CR1).

## Dev Notes

### Method (Amelia)
Red → green → refactor. The "red" here is a **compile + assert** failure: write the Task 3 result-assertions first; they won't compile until `proj_result.h` exists and the plugin emits the new signature. Then implement Tasks 1–2 to reach green. No task is complete until `test_projection` prints `ALL TESTS PASSED` and `unified_sender`/`receiver_verify` still run.

### Current state of the files this story changes (read before editing)

- **`example/src/plugin/protoc-gen-meta.cc`** — the codegen. It does NOT generate hand-written kit code; it emits `*.proj.h`/`*.proj.cc` per proto message.
  - Header decl today (`:185`): `void ProjectMeta(const <ns>::<M>& req, routingmeta::MetadataSink& sink);`
  - Body open today (`:208`): same, as a definition.
  - Required scalar today (`:213-218`): `if (v.empty()) throw std::runtime_error("<key> required");` then `sink.Add("<key>", UrlEncode(v));` **unconditionally** (comment notes "no dead re-check"). This is the example-grade throw FR1/AD-5 removes.
  - Optional scalar (`:221`): `if (!v.empty()) sink.Add(...)` — omit header when empty. **Preserve.**
  - Process-context block (`:226-248`): builds key-sorted `k=v&...` strings, calls `routingmeta::EmitProcessContexts(sink, ctxs)`. **Preserve unchanged** — overflow→`Issue` is Story 1.2.
  - Generated `.proj.cc` includes (`:199`) currently pull `<stdexcept>` only for the throw — drop it.
- **`example/tests/test_projection.cc`** — plain-assert harness, zero deps, prints `ALL TESTS PASSED` (`:166`). The sys3 block (`:107-125`) currently asserts the missing-mask case **threw**. The populated cases call `ProjectMeta(req, sink)` discarding the return.
- **`example/src/common/metadata_sink.h`** — `MetadataSink::Add` tracks running bytes (`+32` HPACK overhead); `VectorSink` records `items` and exposes `Get`/`Count`. Adding `x-routing-error` via `sink.Add` is the right channel; it counts against bytes like any header. **No change this story.**
- **`example/sender/unified_sender.cc`** — demo `Send` template (`:32-36`) = `FillCommon` + `ProjectMeta`, returns `void`, discards `ProjectMeta`'s result. After the pivot it still compiles (return value ignorable). The demo only exercises a **populated** sys3 mask (`:92`), so no missing-required path runs here. **No change this story** (kit-`Send` is Story 1.4).

### What must be preserved (system still works end-to-end, not just the ACs)
- **Wire contract frozen (CR1/AD-9):** the only new header is `x-routing-error`, emitted ONLY on a missing required scalar. Digest, canonical encoding, overflow flag, count/format semantics — byte-identical. Do not touch `process_context_emit.h`.
- **Return value must be ignorable (CR2/AD-10):** do NOT mark `ProjResult` or `ProjectMeta` `[[nodiscard]]`. Existing call sites that discard the return (the demo `Send`, several test cases) must keep compiling.
- **Optional-scalar omit and present-but-empty pctx (`Key=`) behavior** are unrelated rules — leave them exactly as-is.

### Guardrails (do NOT do in this story)
- Do NOT move `ProjectMeta`/`Runtime`/`FillCommon` into `namespace routingmeta` — that is **Story 1.3** (AD-2). `ProjectMeta` stays at its current (global) scope; only its return type changes. `ProjResult`/`Issue` are referenced fully-qualified as `routingmeta::…`.
- Do NOT promote `Send` into the kit or change `unified_sender.cc` — **Story 1.4** (AD-4/FR6).
- Do NOT add overflow→`Issue{Overflow}` plumbing — **Story 1.2** (FR3). Define the enum value only.
- Do NOT self-time / populate `duration` with a real measurement — **Story 1.6** (AD-6). Leave it default-constructed (`{}` = 0 ns).
- Do NOT swap in OpenSSL or any dep (AD-11); reuse `routingmeta::UrlEncode` (`url_encode.h`) already included by the generated `.proj.cc`.

### Exact shape to generate (reference, not prescriptive formatting)
`proj_result.h`:
```cpp
#pragma once
#include <chrono>
#include <string>
#include <vector>
namespace routingmeta {
struct Issue { enum Kind { MissingRequired, Overflow }; Kind kind; std::string key; };
struct ProjResult { bool ok = true; std::vector<Issue> issues; std::chrono::nanoseconds duration{}; };
}  // namespace routingmeta
```
Generated required branch (per projected scalar, `<key>`/`<getter>` substituted by the plugin):
```cpp
if (<getter>.empty()) {
  result.ok = false;
  result.issues.push_back({routingmeta::Issue::MissingRequired, "<key>"});
  sink.Add("x-routing-error", "missing:<key>");
} else {
  sink.Add("<key>", routingmeta::UrlEncode(<getter>));
}
```
`<key>` for sys3 is `x-mask-id` (from the `(routing.project)` annotation), so the emitted value is `missing:x-mask-id`.

### Testing standards
- Harness: the existing `example/tests/test_projection.cc`, plain `assert`, zero test framework — keep that style (NFR: no new deps). Terminal success line `ALL TESTS PASSED` is the contract `build.sh`/CI grep for.
- Build/run: `example/build.sh` (no-cmake fallback) builds plugin → codegen → binaries; CMake path also exists. Verify `test_projection`, and smoke `unified_sender` + `receiver_verify` for no regression.

### Project Structure Notes
- New file lands at `example/src/common/proj_result.h`, alongside `metadata_sink.h`/`process_context_emit.h`, matching the Structural Seed in the Architecture Spine (`src/common/ … proj_result.h <- NEW`).
- No conflicts with existing structure; generated `*.proj.*` continue to emit into the build dir.

### References

- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.1] — user story + 4 ACs (this story).
- [Source: _bmad-output/planning-artifacts/epics.md#Functional Requirements] — FR1 (failure-as-data, no throw), FR2 (missing required → `x-routing-error` + `Issue{MissingRequired}`).
- [Source: ARCHITECTURE-SPINE.md#AD-5] — failure-as-data; exact `ProjResult`/`Issue` shape; missing-required rule.
- [Source: ARCHITECTURE-SPINE.md#AD-9] — wire contract frozen; `x-routing-error` is the only new header (CR1).
- [Source: ARCHITECTURE-SPINE.md#AD-10] — additive API; new return type must be ignorable (CR2).
- [Source: ARCHITECTURE-SPINE.md#AD-2] — namespace move is OUT of scope here (Story 1.3).
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:185,199,208,213-218,249] — generated signature, includes, required-branch throw to replace.
- [Source: grpc-routing-meta/example/tests/test_projection.cc:8,52,107-125,166] — throw-asserting block to pivot; success line.
- [Source: grpc-routing-meta/example/src/common/metadata_sink.h:22-26] — `Add`/bytes accounting (x-routing-error rides this).
- [Source: grpc-routing-meta/example/sender/unified_sender.cc:32-36] — demo `Send` discards the return (must keep compiling).

### Latest tech notes
No external/web research required. Stack is pinned by the Architecture Spine: C++17, libprotobuf/libprotoc 3.20.3 & 3.21.12, hand-rolled `sha256`/`url_encode` (no OpenSSL — AD-11). All APIs used here (`std::chrono::nanoseconds`, `std::vector`, protobuf C++ generator API already in use by the plugin) are stable; nothing version-sensitive is introduced.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer)

### Debug Log References

- Baseline build green before changes: `./build.sh` + `./build/test_projection` → `ALL TESTS PASSED`.
- Red: test rewritten to assert `ProjResult` → compile failure (`ProjectMeta` still `void`, `routingmeta::Issue` absent).
- Green: plugin pivot + `proj_result.h` → build green; `test_projection` `ALL TESTS PASSED`; `unified_sender` + `receiver_verify` run clean, digests unchanged.
- Verification greps: zero `throw` in `build/generated/*.proj.cc`; no `throw/stdexcept/catch` in test; `x-routing-error` wired in all 10 sys3 variants.

### Completion Notes List

- Ultimate context engine analysis completed - comprehensive developer guide created.
- **AC1** — Added `src/common/proj_result.h` (`ProjResult{ok,issues,duration}`, `Issue{Kind(MissingRequired|Overflow),key}`). Generated `ProjectMeta` now returns `routingmeta::ProjResult`; `<stdexcept>` dropped from generated `.proj.cc`; no `throw` on a data condition remains.
- **AC2** — Required-scalar-missing now records `Issue{MissingRequired,"x-mask-id"}`, sets `ok=false`, emits `x-routing-error: missing:x-mask-id`, and does NOT emit the empty `x-mask-id` header (verified in all 10 generated sys3 variants + asserted in test).
- **AC3** — Happy path unchanged: `ok=true`, `issues` empty; `unified_sender` wire output byte-identical to baseline (same digests) → CR1 honored. Return value is discardable (no `[[nodiscard]]`), so the demo `Send` compiles unchanged → CR2/AD-10.
- **AC4** — `test_projection.cc` pivoted from the try/catch `threw` assertion to `ProjResult`/`x-routing-error` assertions, plus positive `ok`/`issues.empty()` checks on sys1 and sys3 happy paths; `ALL TESTS PASSED`.
- Scope held: namespace move (1.3), kit-`Send` (1.4), overflow `Issue` (1.2), `duration` self-timing (1.6) deliberately NOT implemented. `Issue::Overflow` enum value defined (shared type) but unwired.

### File List

- `grpc-routing-meta/example/src/common/proj_result.h` (NEW)
- `grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc` (MODIFIED — `ProjectMeta` returns `ProjResult`, required branch no-throw, dropped generated `<stdexcept>`)
- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — throw→result assertions, removed `<stdexcept>`)

## Change Log

- 2026-06-28 — Story 1.1 implemented: `ProjectMeta` returns `ProjResult` (failure-as-data, no throw); missing required scalar → `Issue{MissingRequired}` + `x-routing-error`. All ACs satisfied; full suite green, no regression (FR1, FR2, CR1, CR2; AD-5/AD-9/AD-10).
