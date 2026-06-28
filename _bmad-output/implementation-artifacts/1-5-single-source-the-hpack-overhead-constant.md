---
baseline_commit: 1063ca904f2fa85c2a98fda540454996cc741150
---

# Story 1.5: Single-source the HPACK overhead constant

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a platform/contract maintainer,
I want the HPACK per-entry overhead defined exactly once,
so that the byte-budget math cannot drift between files.

## Acceptance Criteria

1. **AC1 — Defined once, in the leaf, referenced by the policy header.** Given `metadata_sink.h` and `process_context_emit.h`, when the kit builds, then `kHpackEntryOverhead = 32` is defined exactly once — in the leaf `metadata_sink.h` — and referenced by `process_context_emit.h`, and no literal `+32` duplicate remains. (AD-7, respects include direction)

2. **AC2 — The three limits stay only in the policy header.** Given the three limits 7168 / 25 / 512, when searched, then they live only in `process_context_emit.h`. (NFR6, F)

3. **AC3 — Byte math uses the constant; thresholds unchanged.** Given the byte-accounting, when a context is added, then the size math uses the single constant and the existing overflow thresholds are unchanged.

## Tasks / Subtasks

- [x] **Task 1 — Define the constant in the leaf header (AC: 1, 3)** — edit `example/src/common/metadata_sink.h`
  - [x] Inside `namespace routingmeta` and BEFORE `class MetadataSink` (after `metadata_sink.h:13`), add `constexpr std::size_t kHpackEntryOverhead = 32;` with a one-line comment (gRPC/HPACK per-entry, RFC 7541 §4.1).
  - [x] In `MetadataSink::Add` (`metadata_sink.h:23`), replace the literal `+ 32` with `+ kHpackEntryOverhead` so the running byte total uses the named constant.
  - [x] Leave the explanatory comment at `:20` (it documents the gRPC formula, not a second definition).

- [x] **Task 2 — Reference the leaf constant from the policy header (AC: 1, 2)** — edit `example/src/common/process_context_emit.h`
  - [x] Delete the local definition `constexpr size_t kHpackEntryOverhead = 32;` (`process_context_emit.h:33-34`). The references at `:50` and `:53` now resolve to the one in `metadata_sink.h` (already `#include`d at `:21`, same `namespace routingmeta`).
  - [x] Keep the three limits `kMaxTotalMetaBytes` / `kMaxContexts` / `kMaxLineBytes` (`:29-31`) exactly as-is — values unchanged, still defined ONLY here (NFR6).
  - [x] Tidy the stale comments that mention the "+32" duplicate (`:32` "this + name + 32", `:34` "must match the +32 in metadata_sink.h").

- [x] **Task 3 — Build & verify no drift (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` → links; `./build/test_projection` → `ALL TESTS PASSED` (the overflow-by-bytes / line / count cases are unchanged → thresholds intact).
  - [x] `grep -rn "kHpackEntryOverhead" src/common` → exactly one definition (metadata_sink.h) + the two references (process_context_emit.h). `grep -rn "+ 32\|+32" src/common` → no functional `+ 32` literal remains (only the RFC explanatory comment at `metadata_sink.h:20`, if kept).
  - [x] `./build/unified_sender` metadata byte counts (e.g. `875 bytes` for sys1 2-context) are byte-identical to before — proves the accounting didn't drift. `./build/receiver_verify` → digest OK.

## Dev Notes

### Method (Amelia)
Pure refactor; the test is "the byte math is identical (32 == 32) and nothing drifts." No behavior change — the existing overflow-by-bytes test already exercises the accounting, so it is the regression guard. Done when `test_projection` is green, exactly one definition of `kHpackEntryOverhead` exists, and `unified_sender` byte counts are unchanged.

### Why the constant must live in `metadata_sink.h` (the leaf), not the policy header
- Include direction: `process_context_emit.h` `#include`s `metadata_sink.h` (`:21`), **not** the reverse. So a constant defined in `metadata_sink.h` is visible to `process_context_emit.h`, but a constant defined in `process_context_emit.h` is NOT visible to `metadata_sink.h`.
- Today the constant is (wrongly) in `process_context_emit.h:33`, which is exactly why `metadata_sink.h:23` couldn't reference it and had to duplicate the literal `+ 32`. Moving it to the leaf collapses the duplication without creating a circular include. (AD-7 calls this out explicitly.)

### Current state of the files this story changes (read before editing)
- **`example/src/common/metadata_sink.h`** — `MetadataSink::Add` (`:22-25`) does `bytes_ += key.size() + value.size() + 32;` (literal). `MetadataSink`/`VectorSink`/`GrpcSink` are in `namespace routingmeta` (`:13`). Uses `std::size_t`. This is the LEAF — depends on nothing kit-specific.
- **`example/src/common/process_context_emit.h`** — defines `kHpackEntryOverhead = 32` (`:33`) and the three limits `7168/25/512` (`:29-31`), all in `namespace routingmeta` (`:24`); `#include`s `metadata_sink.h` (`:21`). Uses `kHpackEntryOverhead` at `:50` (`projected = 24 + 71 + kHpackEntryOverhead`) and `:53` (`projected += kKey.size() + c.size() + kHpackEntryOverhead`). Returns `bool` overflow (Story 1.2).

### What must be preserved (system still works end-to-end)
- **Byte accounting identical:** `kHpackEntryOverhead` MUST stay `32` — same value as the literal it replaces. The total returned by `sink.bytes()` and every overflow comparison must be byte-identical.
- **Overflow thresholds unchanged (AC3):** do not touch `7168/25/512` or the overflow condition. The existing overflow-by-bytes test (`test_projection`, the padded-20-context case) is the guard — it must still trip at the same point.
- **Wire frozen (CR1/AD-9):** the byte math gates whether context headers are emitted vs. the overflow flag; identical math → identical wire output. `unified_sender` byte counts and digests unchanged.
- **NFR6 (AC2):** the three numeric limits remain single-sourced in `process_context_emit.h`. `kHpackEntryOverhead` is a separate constant single-sourced in the leaf — moving it does NOT move the limits.

### Guardrails (do NOT do in this story)
- Do NOT change any limit value or the overflow logic.
- Do NOT populate `duration` (Story 1.6), add the bench (1.7), or touch CI (1.9).
- Do NOT add a new constants header or move the three limits — only the HPACK overhead relocates, to the leaf `metadata_sink.h`.

### Exact shape to produce (reference)
`metadata_sink.h` (in `namespace routingmeta`, before the class):
```cpp
// gRPC/HPACK per-entry overhead (RFC 7541 §4.1) — single source of truth,
// referenced by the policy header process_context_emit.h.
constexpr std::size_t kHpackEntryOverhead = 32;
…
void Add(const std::string& key, const std::string& value) {
  bytes_ += key.size() + value.size() + kHpackEntryOverhead;
  Write(key, value);
}
```
`process_context_emit.h`: remove the `kHpackEntryOverhead` definition; keep the `kMaxTotalMetaBytes/kMaxContexts/kMaxLineBytes` trio; the existing `kHpackEntryOverhead` uses now bind to the leaf constant.

### Testing standards
- `example/tests/test_projection.cc`, plain `assert`, `ALL TESTS PASSED`, via `example/build.sh`. No new test needed — the overflow-by-bytes/line/count cases are the regression guard for "thresholds unchanged"; a grep for the single definition guards "defined once".

### Project Structure Notes
- No new files. Edits: `src/common/metadata_sink.h`, `src/common/process_context_emit.h`.

### Previous story intelligence (Stories 1.1–1.4)
- 1.2 changed `EmitProcessContexts` to return a `bool` overflow signal — the byte-budget math (which uses `kHpackEntryOverhead`) is unchanged by this story, only the constant's home moves. 1.3 put `MetadataSink` (and now this constant) in `namespace routingmeta`; `process_context_emit.h` is in the same namespace so the unqualified reference resolves. The overflow tests added/asserted in 1.2 are the guard here.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.5] — user story + 3 ACs.
- [Source: ARCHITECTURE-SPINE.md#AD-7] — centralized policy; `kHpackEntryOverhead` (32) must have exactly one definition shared by `metadata_sink.h` and the policy header; home respects include direction — the single constant lives in the leaf `metadata_sink.h`, referenced by `process_context_emit.h`.
- [Source: grpc-routing-meta/example/src/common/metadata_sink.h:13,22-25] — namespace + `Add` literal `+ 32` to replace.
- [Source: grpc-routing-meta/example/src/common/process_context_emit.h:29-34,50,53] — three limits (keep) + `kHpackEntryOverhead` def (move) + its references.

### Latest tech notes
No external research. `constexpr std::size_t` in a header is a standard inline-constant single-source; C++17, protobuf 3.20/3.21 pinned. No new deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer)

### Debug Log References

- Captured baseline `unified_sender` byte counts; after the move `diff` → IDENTICAL (no accounting drift — AC3/CR1).
- `grep kHpackEntryOverhead src/common` → exactly one definition (`metadata_sink.h:18`) + the `Add` use (`:28`) + two references in `process_context_emit.h` (`:49,:52`); no functional `+ 32` literal remains.
- `grep` for `7168/25/512` → only `process_context_emit.h` (AC2). `test_projection` → `ALL TESTS PASSED`; `receiver_verify` digest OK.

### Completion Notes List

- Ultimate context engine analysis completed - comprehensive developer guide created.
- **AC1** — `constexpr std::size_t kHpackEntryOverhead = 32;` now lives ONCE, in the leaf `metadata_sink.h`, used by `MetadataSink::Add`; `process_context_emit.h`'s definition was deleted and its two uses bind to the leaf constant via the existing `#include`. The duplicate literal `+ 32` in `Add` is gone.
- **AC2** — The three limits `7168 / 25 / 512` remain single-sourced in `process_context_emit.h` (verified by grep — only place).
- **AC3** — Byte math uses the named constant (= 32, same value); overflow thresholds unchanged. `unified_sender` metadata byte counts byte-identical to baseline (875/700/449/499/513/514), so the accounting did not drift; the existing overflow-by-bytes/line/count tests still pass.
- Why the leaf: `process_context_emit.h` `#include`s `metadata_sink.h` (not vice-versa), so the shared constant must live in the leaf to avoid a circular include (AD-7). Wire frozen (CR1). Scope held: limits/overflow logic untouched; `duration` (1.6), bench (1.7), CI (1.9) not touched.

### File List

- `grpc-routing-meta/example/src/common/metadata_sink.h` (MODIFIED — define `kHpackEntryOverhead`, use it in `Add`)
- `grpc-routing-meta/example/src/common/process_context_emit.h` (MODIFIED — remove duplicate def; references bind to the leaf constant; tidy comments)

## Change Log

- 2026-06-28 — Story 1.5 implemented: HPACK per-entry overhead single-sourced as `kHpackEntryOverhead` in the leaf `metadata_sink.h`, referenced by `process_context_emit.h`; duplicate literal `+ 32` removed. Three limits stay in the policy header (NFR6). Byte accounting byte-identical (CR1); all ACs met; full suite green (AD-7).
