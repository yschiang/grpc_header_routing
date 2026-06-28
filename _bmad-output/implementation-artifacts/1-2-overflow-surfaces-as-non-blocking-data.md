---
baseline_commit: db5fc592ee7920b61ef79d46dd9acb5b1f460a9b
---

# Story 1.2: Overflow surfaces as non-blocking data

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a gateway operator,
I want an oversized projection flagged explicitly and non-blocking,
so that routing still proceeds but the overflow is observable, never silently dropped.

## Acceptance Criteria

1. **AC1 — Overflow emits the flag + records a non-blocking issue.** Given a request whose projection would exceed any of total kit metadata > 7168 B, count > 25, or one context value > 512 B, when `ProjectMeta` runs, then it emits `x-process-context-overflow: true`, suppresses the context lines and the digest, still emits `count` and `format`, **and** records a non-blocking `Issue{Overflow}` while leaving `ok=true`. (FR3, SPEC §5.4, AD-5)

2. **AC2 — Caller sees exactly one Overflow issue, ok true.** Given the same request, when the caller inspects the returned `ProjResult`, then `issues` contains exactly one `Overflow` issue and `ok` is true.

3. **AC3 — Test proves it.** Given `test_projection`, when it runs, then an overflow case asserts the above (all three triggers) and the binary prints `ALL TESTS PASSED`.

## Tasks / Subtasks

- [x] **Task 1 — Make the overflow decision reportable (AC: 1, 2)** — edit `example/src/common/process_context_emit.h`
  - [x] Change `EmitProcessContexts` from `void` to `bool` (signature at `process_context_emit.h:38`): return `true` iff the overflow flag was emitted, `false` otherwise.
  - [x] The empty-contexts early return (`:43`) returns `false` (count=0 is not overflow).
  - [x] The overflow branch (`:58-60`) emits `x-process-context-overflow: true` as today, then returns `true`.
  - [x] The within-budget path (after the digest + per-context emit) returns `false`.
  - [x] Do NOT change any limit, the overflow condition, the header names/values, or the suppress-lines-and-digest behavior — only the return type and the three `return` points. The policy (limits 7168/25/512, the decision) stays here (AD-7/NFR6).

- [x] **Task 2 — Map the overflow signal into `ProjResult` (AC: 1, 2)** — edit `example/src/plugin/protoc-gen-meta.cc`
  - [x] In the generated process-context block, change the call (currently `protoc-gen-meta.cc:254`, `routingmeta::EmitProcessContexts(sink, ctxs);`) to capture the bool and push the issue: `if (routingmeta::EmitProcessContexts(sink, ctxs)) result.issues.push_back({routingmeta::Issue::Overflow, ""});`
  - [x] `result` is in function scope and visible inside the context block — keep the existing `{ … }` braces. Do NOT set `result.ok = false` (overflow is non-blocking; `ok` stays `true`).
  - [x] `Issue{Overflow}` carries no key — use empty string `""` for `Issue::key`.
  - [x] Leave the required/optional scalar branches and `return result;` (`:256`) exactly as Story 1.1 left them.

- [x] **Task 3 — Assert the overflow result on all three triggers (AC: 2, 3)** — edit `example/tests/test_projection.cc`
  - [x] Overflow-by-COUNT case (`test_projection.cc:85-94`): capture `auto r = ProjectMeta(req, sink);` and assert `r.ok == true`, `r.issues.size() == 1`, `r.issues[0].kind == routingmeta::Issue::Overflow`. Keep the existing header assertions.
  - [x] Overflow-by-BYTES case (`:97-105`): same capture + assertions.
  - [x] Overflow-by-single-oversized-LINE case (`:145-152`): same capture + assertions.
  - [x] Confirm the existing happy-path `r.issues.empty()` assertions (sys1 `:51`, sys3 `:113`) still hold (non-overflow → `EmitProcessContexts` returns `false` → no issue pushed).

- [x] **Task 4 — Build & verify green (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` (rebuilds plugin, regenerates `*.proj.*`, links binaries).
  - [x] `./build/test_projection` → `ALL TESTS PASSED`.
  - [x] `./build/unified_sender` + `./build/receiver_verify` → run clean; the 60-context overflow demo block still prints `x-process-context-overflow: true` with no other wire change (CR1).

## Dev Notes

### Method (Amelia)
Red → green → refactor. Write the Task 3 overflow-issue assertions first; build → red (the issue isn't pushed yet / signature still `void`). Then Tasks 1–2 → green. Done only when `test_projection` prints `ALL TESTS PASSED` and the demo binaries still run.

### Design decision: where the overflow→issue mapping lives
- The overflow **decision** (limits + condition) must stay in `process_context_emit.h` alone (AD-7/NFR6, criterion F). So `EmitProcessContexts` returns a `bool` rather than the generated code re-deriving overflow.
- The **result assembly** (pushing `Issue{Overflow}` into `result.issues`) lives in the generated `ProjectMeta`, mirroring how Story 1.1 assembles `result` for the missing-required `Issue` in the generated code. The policy header stays decoupled from `ProjResult` (it does not include `proj_result.h`; it only returns a bool).
- Returning `bool` is source-compatible: the only caller is the generated `.proj.cc`, and a statement that ignored the old `void` return still compiles.

### Current state of the files this story changes (read before editing)
- **`example/src/common/process_context_emit.h`** — `EmitProcessContexts(sink, ctxs)` is `void` today. It always emits `count` + `format`; returns early on empty (`:43`); computes `overflow = ctxs.size() > kMaxContexts || maxline > kMaxLineBytes || sink.bytes()+projected > kMaxTotalMetaBytes` (`:55-57`); on overflow emits `x-process-context-overflow: true` and returns (`:58-61`); otherwise emits the digest + one header per context. **This story only adds a `bool` return — no behavior change.**
- **`example/src/plugin/protoc-gen-meta.cc`** — generated `ProjectMeta` (post-1.1) opens with `routingmeta::ProjResult result;`, handles scalars, then the context block calls `EmitProcessContexts(sink, ctxs)` (`:254`) and ends `return result;` (`:256`). `result` is visible inside the context block's braces.
- **`example/tests/test_projection.cc`** — has three overflow cases (count `:85`, bytes `:97`, single line `:145`) that currently call `ProjectMeta(req, sink)` discarding the return and assert only the sink headers. Post-1.1 the sys1/sys3 happy paths already assert `r.ok` + `r.issues.empty()`.

### What must be preserved (system still works end-to-end)
- **Wire frozen (CR1/AD-9):** `x-process-context-overflow`, the suppression of context lines + digest, and the still-emitted `count`/`format` are all unchanged. No new header, no value change. The only observable change is in the returned `ProjResult.issues` (in-process, not on the wire).
- **`ok` stays `true` on overflow (FR3, AD-5):** overflow is explicitly non-blocking. Only a missing **required** scalar sets `ok=false` (Story 1.1). If a request hits BOTH (missing-required AND overflow), `issues` will correctly contain both and `ok=false` — that is the right semantics and needs no special-casing.
- **Return value discardable (CR2/AD-10):** do not add `[[nodiscard]]`. The demo `Send` and receiver still discard the return.

### Guardrails (do NOT do in this story)
- Do NOT move anything into `namespace routingmeta` (Story 1.3), promote `Send` (Story 1.4), populate `duration` (Story 1.6), or add the bench (Story 1.7).
- Do NOT change the limits, the overflow condition, or any header name/value (AD-7 keeps the policy single-sourced; CR1 freezes the wire).
- Do NOT have `EmitProcessContexts` push the issue itself or include `proj_result.h` — keep the policy header decoupled; the generated code owns result-assembly.

### Exact shape to produce (reference)
`process_context_emit.h` (return points only):
```cpp
inline bool EmitProcessContexts(MetadataSink& sink, const std::vector<std::string>& ctxs) {
  sink.Add("x-process-context-count", std::to_string(ctxs.size()));
  sink.Add("x-process-context-format", "urlencoded-query-string-v1");
  if (ctxs.empty()) return false;
  // … compute overflow exactly as today …
  if (overflow) { sink.Add("x-process-context-overflow", "true"); return true; }
  // … digest + per-context emit …
  return false;
}
```
Generated call site (plugin template):
```cpp
if (routingmeta::EmitProcessContexts(sink, ctxs))
  result.issues.push_back({routingmeta::Issue::Overflow, ""});
```

### Testing standards
- Same harness: `example/tests/test_projection.cc`, plain `assert`, zero deps; success line `ALL TESTS PASSED`. Build via `example/build.sh`.

### Project Structure Notes
- No new files. Edits land in `src/common/process_context_emit.h`, `src/plugin/protoc-gen-meta.cc`, `tests/test_projection.cc`.

### Previous story intelligence (Story 1.1)
- 1.1 added `src/common/proj_result.h` (`ProjResult{ok,issues,duration}`, `Issue{Kind(MissingRequired|Overflow),key}`) and pivoted generated `ProjectMeta` to return it; the generated code assembles `result` directly (pattern reused here). Issue aggregate-init form: `{routingmeta::Issue::Kind, "key"}`.
- 1.1's code review **explicitly deferred this exact work to Story 1.2**: "`Issue::Overflow` declared but never produced — overflow reported only via the sink header, not in `result.issues`." This story wires it. (See `_bmad-output/implementation-artifacts/deferred-work.md`.)
- 1.1 review also noted the missing+overflow co-occurrence; this story's design records both issues with `ok=false` in that case — no extra work needed.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.2] — user story + 3 ACs.
- [Source: _bmad-output/planning-artifacts/epics.md#Functional Requirements] — FR3 (overflow as non-blocking data).
- [Source: ARCHITECTURE-SPINE.md#AD-5] — overflow → `Issue{Overflow}`, `ok` stays true.
- [Source: ARCHITECTURE-SPINE.md#AD-7] — limits + policy single-sourced in `process_context_emit.h`.
- [Source: ARCHITECTURE-SPINE.md#AD-9] — wire frozen (CR1).
- [Source: grpc-routing-meta/example/src/common/process_context_emit.h:38,43,55-61] — `EmitProcessContexts` current void shape + overflow branch.
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:254,256] — generated call site + `return result;`.
- [Source: grpc-routing-meta/example/tests/test_projection.cc:85,97,145] — three overflow cases to assert against.
- [Source: _bmad-output/implementation-artifacts/1-1-projectmeta-returns-projresult-no-throw.md] — prior story + ProjResult pattern.

### Latest tech notes
No external research required. C++17 + protobuf 3.20/3.21 (pinned by the architecture). `bool` return on an inline header function is trivially stable; no new APIs.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer)

### Debug Log References

- Red: added Overflow-issue assertions to the three overflow test cases → build OK, `test_projection` aborts at `r.issues.size()==1` (issue not yet produced).
- Green: `EmitProcessContexts` void→bool + generated mapping → `ALL TESTS PASSED`; `unified_sender` + `receiver_verify` clean; 60-context overflow demo unchanged (513 bytes, `x-process-context-overflow: true`).
- Verified `Issue::Overflow` wired in every generated message variant (sys1×1, sys2×5, sys3×10).

### Completion Notes List

- Ultimate context engine analysis completed - comprehensive developer guide created.
- **AC1/AC2** — `EmitProcessContexts` now returns `bool` (true iff the overflow flag was emitted); the generated `ProjectMeta` records `result.issues.push_back({routingmeta::Issue::Overflow, ""})` on that signal and leaves `result.ok` unchanged (`true`). Policy (limits 7168/25/512 + the overflow condition) stays single-sourced in `process_context_emit.h` (AD-7/NFR6).
- **AC3** — All three overflow triggers (count>25, total>7168 B, single value>512 B) assert `r.ok==true`, `r.issues.size()==1`, `kind==Overflow`. `ALL TESTS PASSED`.
- Wire frozen (CR1/AD-9): `x-process-context-overflow`, the suppression of context lines + digest, and the still-emitted `count`/`format` are byte-identical; only in-process `ProjResult.issues` changed. Demo overflow block unchanged.
- Co-occurrence: a request with both a missing required scalar and overflow now reports both issues with `ok=false` (the blocking scalar dominates) — closes the 1.1-review note. Verified by construction (independent branches both push to `result.issues`).
- Scope held: namespace (1.3), `Send` (1.4), `duration` (1.6), bench (1.7) untouched; no `[[nodiscard]]` added (CR2/AD-10).

### File List

- `grpc-routing-meta/example/src/common/process_context_emit.h` (MODIFIED — `EmitProcessContexts` void→bool; three return points)
- `grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc` (MODIFIED — generated code maps the bool → `Issue{Overflow}`)
- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — assert the Overflow issue on all three triggers)

## Change Log

- 2026-06-28 — Story 1.2 implemented: overflow surfaces as a non-blocking `Issue{Overflow}` in `ProjResult` (`ok` stays `true`); `EmitProcessContexts` returns the overflow signal, generated `ProjectMeta` records it. Wire unchanged (CR1). All ACs met; full suite green, no regression (FR3; AD-5/AD-7/AD-9).
