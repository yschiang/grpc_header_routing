---
baseline_commit: 32e0611b46c453126c257c40e4e19adbf4716dab
---

# Story 1.11: Receiver digest gate regression

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a receiver,
I want to recompute and compare the digest,
so that any header↔body drift is rejected.

## Acceptance Criteria

1. **AC1 — Accept path: matching digest is accepted.** Given a correctly projected context set, when `receiver_verify` recomputes `x-process-context-digest` over the `\n`-joined contexts, then it matches and accepts. (FR5, BRIEF Verify line 3)

2. **AC2 — Reject path: tampered/mismatched is rejected.** Given a tampered or mismatched context set, when the receiver verifies, then it rejects on digest mismatch. (AD-9)

3. **AC3 — Both paths asserted.** Given `test_projection` / `receiver_verify`, when run, then both the accept and reject paths are asserted.

## Tasks / Subtasks

- [x] **Task 1 — `receiver_verify` proves the reject path, not just accept (AC: 2, 3)** — edit `example/receiver/receiver_verify.cc`
  - [x] Today `receiver_verify` only exercises the ACCEPT path (projects a valid sys1 request → `VerifyDigest` → `OK` → `return 0`). The receiver binary the story is named for never demonstrates rejection. Add an explicit reject-path self-check AFTER the genuine accept: tamper a COPY of the received contexts (introduce header↔body drift, e.g. `tampered[0] += "&INJECTED=1";`), recompute via `VerifyDigest(tampered, digest)`, print the outcome, and `assert(!bad.ok)` — a regression guard that fires (aborts, non-zero) if a future change makes the gate accept drift.
  - [x] Keep `main`'s genuine return on the real (untampered) path = `vr.ok ? 0 : 1` (→ 0), so `build.sh`'s self-check and CI stay green. The reject self-check is an ADDED internal assertion + a printed line (e.g. `tamper check: rejected (expected) — digest mismatch: header/body projection drift`), NOT the binary's exit code.
  - [x] Add `#include <cassert>`. Guard `tampered[0]` with `if (!contexts.empty())` (the binary projects 2 contexts, but be defensive). Do NOT change the projection, the parser, or `VerifyDigest`.

- [x] **Task 2 — First-class digest-gate regression block in `test_projection` (AC: 1, 2, 3)** — edit `example/tests/test_projection.cc`
  - [x] The existing sys1 block asserts accept (`:71`) and a single tamper-reject (`:74`); AC2 names "tampered OR mismatched" but only tampered-context is covered, and it is incidental. Add a dedicated, labeled block (e.g. `// --- digest gate: accept + every reject form (FR5/AD-9, story 1.11) ---`) over a fresh 2-context sys1 projection, asserting:
    - **accept:** `VerifyDigest(cs, dg).ok` AND `actual_digest == expected_digest`.
    - **reject — tampered context:** mutate a value (`cs[0] += "&INJECTED=1"`) → `!ok` AND `error` contains `mismatch`.
    - **reject — corrupted digest header (intact body):** `VerifyDigest(cs, "sha256:" + std::string(64,'0'))` → `!ok` AND `actual_digest != expected_digest` AND `error` contains `mismatch`.
    - **reject — dropped context (count drift):** `cs.pop_back()` → `VerifyDigest(shorter, dg)` → `!ok` (the `\n`-joined canon changed).
    - **reject — no digest provided (overflow/omitted):** `VerifyDigest(cs, "")` → `!ok` AND `error` contains `no digest`.
  - [x] Use the existing `sys1Req` helper and the `sink.items` extraction pattern already in the file. Assert on `VerifyResult` fields (`ok`/`error`/`actual_digest`/`expected_digest`) so the regression pins the gate's REPORTED reason, not just the boolean. Leave the existing round-trip/tamper block as-is (it also tests `ParseContext`).

- [x] **Task 3 — Build & verify both paths green (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; binaries unchanged elsewhere.
  - [x] `./build/test_projection` → `ALL TESTS PASSED` (the new digest-gate block's accept + 4 reject asserts pass).
  - [x] `./build/receiver_verify` → prints `digest check: OK …` (accept) AND the new `tamper check: rejected (expected) — …` line, and exits `0` (genuine path accepts; the tamper self-check asserts rejection internally). `echo $?` → 0.
  - [x] Regression: `./build/bench_projection` → `BENCH PASSED`; the `[neg ]` gate still green; wire output byte-identical (this story only adds receiver-side assertions — no projection/plugin/wire change; CR1/AD-9).

## Dev Notes

### Method (Amelia)
Red → green per path. (1) In `test_projection`, add the reject asserts first; they pass immediately because `VerifyDigest` already rejects drift — so the "red" here is *coverage* red (the gate's reject behavior was under-asserted), turned green by pinning every reject form. (2) In `receiver_verify`, add the tamper self-check; confirm it prints "rejected (expected)" and the binary still exits 0 on the genuine path. The regression value: if anyone later weakens `VerifyDigest` (e.g. compares lengths only, or skips the recompute), `test_projection` fails AND `receiver_verify` aborts.

### How the gate works (read before editing)
- `VerifyDigest(contexts, received_digest)` (`src/common/process_context_parser.h:71-89`): recomputes `"sha256:" + Sha256Hex(canon)` where `canon` is the contexts joined by `'\n'` (same canonical rule the sender's `EmitProcessContexts` digests), then:
  - `received_digest.empty()` → `ok=false`, `error="no digest provided (overflow or sender omitted)"`.
  - else `ok = (actual_digest == received_digest)`; on mismatch `error="digest mismatch: header/body projection drift"`.
  - It always sets `expected_digest` (= received) and `actual_digest` (= recomputed) — assert on these to pin the gate's report.
- The receiver canonicalization MUST match the sender's: contexts are already key-sorted internally by the projection, and the digest is over the `'\n'`-join in body order. Do not re-sort or re-encode on the receiver side — `VerifyDigest` takes the contexts as received.

### Why `receiver_verify` keeps exit 0 on the happy path
- `build.sh`'s self-check and CI (Story 1.9) run `./build/receiver_verify` and expect success (exit 0) — it is the BRIEF "Verify line 3" (`receiver_verify  # digest OK`). So the genuine projection must still accept and `main` must still `return vr.ok ? 0 : 1`. The reject path is proven by an INTERNAL `assert(!bad.ok)` on a tampered copy (aborts → non-zero only if the gate REGRESSES to accept drift). This gives the binary both-path coverage (AC3) without flipping its success contract.

### What must be preserved (system still works end-to-end)
- **No projection/plugin/wire change (CR1/AD-9):** this story adds receiver-side and test-side assertions only. `ProjectMeta`, the plugin, generated code, `EmitProcessContexts`, and all emitted headers/digests are byte-identical. AD-9 is precisely the integrity property this story regression-guards.
- **Do NOT modify `VerifyDigest`/`ParseContext`** — they are correct; the story exercises them, it does not change them. (If a test reveals a real bug, raise it — but none is expected.)
- **`assert`-based harness convention:** `build.sh` compiles `-O2 -Wall` with no `-DNDEBUG`, so `assert` is live (same convention as `test_projection`). The reject self-check in `receiver_verify` relies on this.
- **The accept-path self-check binaries stay green:** `unified_sender`/`bench_projection`/`test_projection` unchanged in behavior; the `[neg ]` gate unaffected.

### Guardrails (do NOT do in this story)
- Do NOT change `ProjectMeta`, the plugin, `EmitProcessContexts`, `VerifyDigest`, `ParseContext`, or any wire output. Receiver/test assertions ONLY.
- Do NOT make `receiver_verify` return non-zero on its genuine happy path (would break `build.sh`/CI). The reject proof is an internal assert + printed line.
- Do NOT add a new dependency or a test framework — plain `assert`, same as `test_projection`.
- Do NOT re-sort/re-encode contexts on the receiver — verify them as received (that is the whole point of the digest catching drift).

### Current state of the things this story touches (read before editing)
- **`example/receiver/receiver_verify.cc`** — projects a 2-context sys1 request, extracts `x-process-context` + `x-process-context-digest` from the sink, parses each context, calls `VerifyDigest`, prints `digest check: OK/FAILED` with expected/actual, `return vr.ok ? 0 : 1`. No reject-path demonstration today; no `<cassert>` include.
- **`example/tests/test_projection.cc`** — the sys1 block (`:48-75`) has `assert(VerifyDigest(cs,dg).ok)` (`:71`) and a tamper `assert(!VerifyDigest(cs,dg).ok)` (`:74`); includes `<cassert>`, `common/process_context_parser.h`, the `sys1Req` helper. Add the new dedicated digest-gate block here.
- **`example/src/common/process_context_parser.h:61-89`** — `VerifyResult` + `VerifyDigest` (read-only; the contract above).

### Testing standards
- Plain `assert`, zero test deps, `ALL TESTS PASSED` via `build.sh`. `test_projection` asserts accept + every reject form (tampered, corrupted digest, dropped context, no digest), pinning `VerifyResult.error`/`actual_digest`. `receiver_verify` prints the accept line + a tamper-rejected line and asserts the reject, exiting 0 on the genuine path. Both are run by `build.sh`/CI.

### Project Structure Notes
- Edits: `receiver/receiver_verify.cc` (+`<cassert>`, reject self-check), `tests/test_projection.cc` (digest-gate regression block). No new files. No kit/plugin/build.sh/wire change.

### Previous story intelligence (Stories 1.1–1.10)
- The digest is computed in `EmitProcessContexts` (Story 1.2/1.5 area) over the canonical `'\n'`-join and emitted as `x-process-context-digest`; `VerifyDigest` mirrors it. 1.10 hardened the build-time negative-codegen gate; this story hardens the RUN-time integrity gate (the receiver's digest compare). Both are "fail loud" guards — 1.10 at codegen, 1.11 at receive. The 1.10 review deferred "negative gate asserts exit-code only, not reason" to 1.12 — unrelated to this story (that is the codegen gate; this is the receiver gate). Wire is frozen (CR1/AD-9): no projection change here.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.11] — user story + 3 ACs (accept, reject, both asserted).
- [Source: refs/BRIEF.md#receiver + Verify line 3] — "receiver: x-process-context-digest recompute + compare → reject on mismatch"; `./build/receiver_verify  # digest OK`.
- [Source: ARCHITECTURE-SPINE.md#AD-9] — wire frozen + the digest integrity gate; header↔body drift is rejected.
- [Source: ARCHITECTURE-SPINE.md#FR5] — receiver recomputes and compares the digest.
- [Source: grpc-routing-meta/example/src/common/process_context_parser.h:71-89] — `VerifyDigest` (canonical `\n`-join, two reject reasons).
- [Source: grpc-routing-meta/example/receiver/receiver_verify.cc] — current accept-only flow.
- [Source: grpc-routing-meta/example/tests/test_projection.cc:71,74] — existing accept + incidental tamper asserts.

### Latest tech notes
No external research. sha256 over the canonical `'\n'`-joined, key-sorted, url-encoded contexts; receiver recompute + compare is the standard integrity check. C++17, plain `assert`. No new deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer) — engineering by a general-purpose subagent; main loop did create-story, independent verification (re-ran build + both binaries + scope), and BMad bookkeeping.

### Debug Log References

- Engineering delegated to a subagent; main loop independently re-ran before marking review.
- Green (independently re-run, `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig ./build.sh`): links, `[neg ]` gate green, `OK -> binaries`. `test_projection` → `ALL TESTS PASSED` (new digest-gate block: accept + 4 reject forms). `receiver_verify` → prints BOTH `digest check: OK (header matches body)` AND `tamper check: rejected (expected reject) — digest mismatch: header/body projection drift`; `exit=0`.
- Vacuity proof (subagent, reverted): flipping the tampered-context assert to `assert(r.ok)` made `test_projection` SIGABRT (`Assertion failed: (r.ok) … line 94`, exit 134) — the reject asserts genuinely bite; restored and reconfirmed `ALL TESTS PASSED`.
- Scope: `git diff --stat` → only `receiver/receiver_verify.cc` (+13) and `tests/test_projection.cc` (+31), additions only. `VerifyDigest`/`ParseContext`/plugin/generated/`build.sh` untouched; receiver digest still `efafba16…` (wire byte-identical).

### Completion Notes List

- **AC1** — accept path: `receiver_verify` recomputes over the `\n`-joined contexts → `digest check: OK`; `test_projection` asserts `VerifyDigest(cs,dg).ok` AND `actual_digest == expected_digest`.
- **AC2** — reject path: `receiver_verify`'s tamper self-check rejects header↔body drift (asserted); `test_projection` asserts FOUR reject forms — tampered context, corrupted digest header, dropped context (count drift), and empty digest — each pinning `VerifyResult.error`/`actual_digest`, covering AC2's "tampered OR mismatched" fully (was only tampered before).
- **AC3** — both paths asserted across both named binaries: `test_projection` (accept + 4 rejects) and `receiver_verify` (accept printed + reject asserted). `receiver_verify` keeps exit 0 on its genuine path (build.sh/CI contract) — the reject is an internal `assert(!bad.ok)` that fires only on a gate regression.
- **Scope held / CR1·AD-9** — receiver/test assertions only; no projection/plugin/`VerifyDigest`/wire change; the integrity gate (AD-9) is now regression-guarded at both the unit (`test_projection`) and binary (`receiver_verify`) level.

### File List

- `grpc-routing-meta/example/receiver/receiver_verify.cc` (MODIFIED — `+#include <cassert>`; reject-path self-check: tamper a copy of the received contexts, `VerifyDigest` → print + `assert(!bad.ok)`; genuine-path `return vr.ok ? 0 : 1` unchanged)
- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — dedicated digest-gate regression block: accept + 4 reject forms, asserting `VerifyResult` fields)

## Change Log

- 2026-06-28 — Story 1.11 drafted (create-story): make the receiver digest gate's REJECT path first-class and regression-guarded. `receiver_verify` gains a tamper self-check (proves it rejects header↔body drift, still exits 0 on the genuine path); `test_projection` gains a dedicated digest-gate block asserting accept + four reject forms (tampered context, corrupted digest, dropped context, no digest) pinning `VerifyResult`'s reported reason. No projection/plugin/`VerifyDigest`/wire change (CR1/AD-9) — receiver/test assertions only.
- 2026-06-28 — Story 1.11 implemented (dev-story): `receiver_verify` reject self-check (+13) + `test_projection` digest-gate block (+31). `ALL TESTS PASSED`; `receiver_verify` prints both accept + reject lines, exit 0; vacuity-proven (flipped assert → SIGABRT, reverted). Additions only; wire byte-identical (digest `efafba16…`). Engineering by a subagent, independently re-verified in the main loop. Status → review.
