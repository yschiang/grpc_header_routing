---
baseline_commit: 34c0e96f80a531a195d1bef7e24d52c7ce97e4c2
---

# Story 1.12: Canonical projection + crypto vectors, regression-guarded

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a platform/contract maintainer,
I want byte-exact encoding and digest locked by tests,
so that projection cannot drift run-to-run or release-to-release.

## Acceptance Criteria

1. **AC1 — Golden canonical projection.** Given a known request, when projected, then each context = `(routing.pctx)` fields key-sorted (`ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech`), `&`-joined as `Key=UrlEncode(Value)`, empty field → `Key=`, `/` → `%2F`, asserted byte-for-byte against a golden. (FR8, SPEC §5–6)

2. **AC2 — Determinism.** Given the same input projected twice, when compared, then the output is byte-identical. (NFR4)

3. **AC3 — Crypto known-answer vectors.** Given SHA-256 known-answer vectors (empty, 55- and 56-byte boundary, multi-block) and URL-encode round-trips (reserved, space, high-byte), when asserted, then each matches its expected value. (HR1)

## Tasks / Subtasks

> All work is additive regression assertions in `tests/test_projection.cc`, plus one `build.sh` `[neg ]`-gate hardening. NO projection/plugin/kit logic change — this story LOCKS current behavior; it must not alter any byte the kit emits.

- [x] **Task 1 — Golden canonical-projection vector (AC: 1)** — edit `tests/test_projection.cc`
  - [x] Build a KNOWN sys1 request with one context whose fields force every rule: `recipe_id="R/A"` (→ `%2F`), `part_id=""` (→ `PartID=` empty), and the rest set: `chamber_id="CH-A"`, `lot_id="LOT01"`, `operation_no="OP100"`, `stage_id="ETCH"`, `tech="N5"`.
  - [x] `ProjectMeta` it, pull the single `x-process-context`, and assert it equals the EXACT golden string:
    `ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=&RecipeID=R%2FA&StageID=ETCH&Tech=N5`
    — key-sorted (alpha by emitted key), `&`-joined, `Key=UrlEncode(Value)`, empty `PartID=`, `/`→`%2F`. (Confirm the emitted key is `OperationNO` (upper NO) per `proto/process_context.proto:26`, and `ChamberId` (lower d).)
  - [x] This is the byte-for-byte lock (FR8). If the projection order/encoding ever drifts, this assert fails.

- [x] **Task 2 — Determinism (AC: 2)** — edit `tests/test_projection.cc`
  - [x] Project the SAME request twice into two separate sinks; assert the full `sink.items` sequences are equal (same keys, same values, same order) — or at minimum the concatenated `x-process-context` + `x-process-context-digest` are byte-identical across both runs. Proves no map-ordering / iteration nondeterminism (NFR4).

- [x] **Task 3 — SHA-256 KATs + URL-encode round-trips (AC: 3)** — edit `tests/test_projection.cc`
  - [x] Add SHA-256 known-answer asserts against INDEPENDENT published values (NOT recomputed by the kit). Verify each with `printf '%s' "<input>" | shasum -a 256` (or `openssl dgst -sha256`) on the dev host before pinning:
    - empty: `Sha256Hex("")` == `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
    - 56-byte (two-block boundary, NIST): `Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")` == `248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1`
    - 55-byte (one-block padding boundary): `Sha256Hex(std::string(55,'a'))` == (pin the `shasum -a 256` value).
    - multi-block (1,000,000 × 'a', NIST): `Sha256Hex(std::string(1000000,'a'))` == `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`.
    - (Keep the existing `Sha256Hex("abc")` assert.)
  - [x] URL-encode: the existing asserts already cover reserved (`/`→`%2F`, `?`→`%3F`), space (`%20`), high-byte (`\xC3\xA9`→`%C3%A9`), and a round-trip. Confirm they remain; add an explicit `UrlDecode("R%2FA")=="R/A"` and a high-byte round-trip `UrlDecode(UrlEncode("\xC3\xA9"))=="\xC3\xA9"` if not already implied. Do not weaken existing asserts.

- [x] **Task 4 — Cover all 10 sys3 required-scalar messages (deferred from 1.1) (AC: 1)** — edit `tests/test_projection.cc`
  - [x] `proto/sys3.proto` has Submit01–Submit10, each projecting `x-mask-id` via a DIFFERENT getter path (top-level, renamed, nested-1, nested-2, sub-message — see the proto). Only Submit05 is exercised today. For EACH of the 10, set its mask field to a known value and assert `sink.Get("x-mask-id")` == that value (proves every generated getter path projects correctly).
  - [x] Use the nesting from `proto/sys3.proto:19-28` to set each one's field (e.g. `Submit03` → `mutable_mask()->set_id(...)`, `Submit05` → `mutable_job()->mutable_mask()->set_mask_id(...)`, `Submit10` → `mutable_exposure()->mutable_mask()->set_id(...)`, etc.). Include `sys3.proj.h`/`sys3.pb.h` (already included for Submit05). Also assert one EMPTY-required case on a non-Submit05 message → `!ok`, `x-routing-error: missing:x-mask-id` (the deep-nested missing path).

- [x] **Task 5 — Exact-threshold overflow boundaries (deferred from 1.2) (AC: 2-adjacent regression)** — edit `tests/test_projection.cc`
  - [x] The three overflow triggers are strict `>` (`process_context_emit.h:56`): `ctxs.size() > 25`, `maxline > 512`, `sink.bytes()+projected > 7168`. Pin each boundary so a future `>`→`>=` slip fails:
    - **count:** 25 contexts (small fields, total < 7 KB, line < 512) → `!overflow` (emits 25, has digest); 26 → `overflow` (`x-process-context-overflow: true`, no lines).
    - **line:** ONE context padded so its ENCODED string is exactly 512 bytes → `!overflow`; 513 → `overflow`. (Base encoded length with all fields empty except `PartID` ≈ 63, so `part_id = std::string(449,'x')` → 512, `450` → 513 — but CALIBRATE empirically: measure the encoded `x-process-context` length and adjust the pad to hit 512/513 exactly.)
    - **total:** many small contexts (count ≤ 25, each line ≤ 512) padded so the projected metadata total is exactly 7168 → `!overflow`; 7169 → `overflow`. CALIBRATE by measuring `sink.bytes()` + the projected delta; pad one field to land on the boundary.
  - [x] For each, assert the boundary value does NOT overflow and boundary+1 DOES — that is what catches a `>`→`>=` regression. (Keep the existing 30 / 600 / ~9 KB over-the-line tests.)

- [x] **Task 6 — Negative gate asserts the REASON, not just exit code (deferred from 1.10) (regression)** — edit `example/build.sh`
  - [x] Today the `[neg ]` loop only checks codegen exits non-zero (`>/dev/null 2>&1`), so a fixture rejected for a broken import/syntax would still "pass." Harden it: for each `tests/negative/*.proto`, capture stderr and assert it contains that fixture's EXPECTED error substring; fail the gate if a fixture is rejected for the wrong reason (or accepted).
  - [x] Map fixture → expected substring (from the plugin's `Validate` messages): `bad_duplicate_key` → `duplicate (routing.project) key`; `bad_message_project` → (its `ProjectOnlyOnScalarLeaf` message); `bad_repeated_scalar` → (its message); `bad_project_under_repeated` → (its `NoProjectUnderRepeated` message). Read `src/plugin/protoc-gen-meta.cc` for the exact strings. Implement with an associative lookup or a per-file expected-substring convention; keep it list-driven and minimal. The gate must still `exit 1` if ANY fixture is accepted OR rejected for the wrong reason.
  - [x] Verify all four fixtures still pass the HARDENED gate (rejected for the right reason), and that breaking one fixture's reason (scratch test, reverted) would now fail the gate.

- [x] **Task 7 — Build & verify (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; the hardened `[neg ]` gate prints `ok (rejected)` for all four with the reason matched; `OK -> binaries`.
  - [x] `./build/test_projection` → `ALL TESTS PASSED` (golden, determinism, 4 SHA KATs, 10 sys3 messages, 3 exact boundaries).
  - [x] Regression: `./build/receiver_verify` → digest OK (UNCHANGED digest — wire byte-identical); `./build/bench_projection` → BENCH PASSED; `./build/unified_sender` runs. Confirm NO emitted byte changed (this is a lock-in story).

## Dev Notes

### Method (Amelia)
Every task is a regression LOCK, not a behavior change. The discipline: write each assert against the CURRENT correct output (golden string, published KATs, the existing overflow semantics), confirm it passes, and confirm it would FAIL if the locked behavior drifted (spot-check by a scratch flip on the golden and one boundary). The "red" is latent-drift red — the value is catching a future change, so each assert MUST be non-vacuous (assert exact strings / exact booleans, not `!empty()`).

### The golden string (AC1) — derivation
- Emitted keys (`proto/process_context.proto:21-27`): `lot_id→LotID`, `recipe_id→RecipeID`, `tech→Tech`, `part_id→PartID`, `stage_id→StageID`, `operation_no→OperationNO`, `chamber_id→ChamberId`. Key-sorted (alpha): `ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech` — matches AC1.
- For the known input (`recipe_id="R/A"`, `part_id=""`, others set), the single context is exactly:
  `ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=&RecipeID=R%2FA&StageID=ETCH&Tech=N5`
  (the `&` are field separators between `Key=Value` pairs; empty `PartID` shows as `PartID=`; `/`→`%2F`). Assert byte-for-byte.

### SHA-256 KATs (AC3) — independence is the point
- The asserts must compare the kit's `Sha256Hex` to INDEPENDENT reference values, never to itself (that would be circular). Use published NIST vectors (empty, the 56-byte "abcdbcde…" two-block vector, the 1M-'a' multi-block vector) and verify the 55-byte one with `shasum -a 256` before pinning. The 55/56-byte pair straddles SHA-256's one-block→two-block padding boundary (a message of L bytes needs a second block iff L ≥ 56, since padding adds ≥ 9 bytes into the 64-byte block) — the classic boundary KAT.

### Exact-threshold boundaries (Task 5) — why exact, and calibration
- The triggers use strict `>` (`process_context_emit.h:56`). A regression `>`→`>=` would overflow one item too early. Only a test at EXACTLY the limit catches it: at count 25, `>25` is false (no overflow) but `>=25` would be true (overflow) — so asserting "25 → no overflow" fails under the slip. Hence pin the exact boundary value as no-overflow and boundary+1 as overflow.
- Line/total need empirical calibration (encoded lengths depend on key names + separators). Build the request, print the encoded `x-process-context` length / `sink.bytes()`, then size the pad to hit 512/7168 exactly. Do NOT hardcode a guessed pad without measuring.

### All 10 sys3 messages (Task 4) — getter-path coverage
- Submit01–10 (`proto/sys3.proto:19-28`) all project `x-mask-id` but through different generated getters (top-level scalar, renamed, nested `.mask().id()`, double-nested `.job().mask().mask_id()`, sub-message `.request_header()…`, etc.). Submit05 alone is tested today; the other nine share template-identical codegen with different paths. Exercising all 10 proves the plugin's `walkProj` getter generation is correct for every nesting shape and guards against a codegen regression on any path.

### Negative-gate reason (Task 6) — the only non-test edit
- `build.sh`'s `[neg ]` loop currently asserts only non-zero exit. Capture stderr and require each fixture's expected `Validate` error substring, so a fixture silently failing for a parse/import reason (instead of its semantic branch) FAILS the gate. This resolves the 1.10 review deferral and makes all four (now) negative fixtures reason-checked. Keep the loop list-driven; the four expected substrings come from `protoc-gen-meta.cc`'s `Validate`/helper error messages — read them, don't guess.

### What must be preserved (system still works end-to-end)
- **No emitted byte changes (CR1/AD-9 / this is a LOCK story):** projection, plugin, generated code, `EmitProcessContexts`, `VerifyDigest`, and every header/digest are byte-identical. `receiver_verify`'s digest stays `efafba16…`; `unified_sender` byte counts unchanged. If any new assert forces a code change to pass, STOP — that means a real drift was found; raise it, don't "fix" the test.
- **`assert`-based harness:** plain `assert`, `-O2 -Wall`, no `-DNDEBUG`. `ALL TESTS PASSED`.
- **The negative gate stays a hard gate:** Task 6 makes it STRICTER (reason-checked), never weaker; all four fixtures must still be rejected.

### Guardrails (do NOT do in this story)
- Do NOT change `ProjectMeta`, the plugin, `EmitProcessContexts`, `VerifyDigest`, `ParseContext`, `Sha256Hex`, `UrlEncode`, the limits, or any wire output. Tests + the `build.sh` gate-reason check ONLY.
- Do NOT compute KAT expected values with the kit's own `Sha256Hex` (circular) — use published/`shasum` references.
- Do NOT hardcode an un-measured pad for the line/total boundaries — calibrate.
- Do NOT weaken or delete the existing over-the-line overflow tests (30 / 600 / ~9 KB) — add the boundary cases alongside.

### Current state (read before editing)
- **`tests/test_projection.cc`** — has the url+sha256 block (`:40-46`), the sys1 projection/digest block, overflow tests (count 30, bytes via 300-pad, single 600-byte line), the sys3 Submit05 scalar + missing-required block, co-occurrence, common-headers, and `Send` blocks. Add the new golden/determinism/KAT/sys3-all/boundary blocks here.
- **`proto/process_context.proto:20-27`** — the 7 pctx fields + emitted keys. **`proto/sys3.proto:19-28`** — Submit01–10 + their getter shapes. **`src/common/process_context_emit.h:29-31,56`** — limits + the strict-`>` overflow condition. **`src/plugin/protoc-gen-meta.cc`** — `Validate` error strings (for Task 6). **`example/build.sh:58-72`** — the `[neg ]` loop (Task 6).

### Testing standards
- Plain `assert`, `ALL TESTS PASSED` via `build.sh`. Golden byte-exact, KATs vs independent references, boundaries exact, all 10 sys3 getters, hardened negative gate. No new files, no new deps.

### Project Structure Notes
- Edits: `tests/test_projection.cc` (golden, determinism, KATs, sys3×10, boundaries) and `example/build.sh` (negative-gate reason check). No kit/plugin/proto/wire change. Resolves three `deferred-work.md` items (1.1 sys3 coverage, 1.2 boundaries, 1.10 gate reason) — mark them RESOLVED.

### Previous story intelligence (Stories 1.1–1.11)
- 1.2 set the overflow limits + emitted the `Issue{Overflow}`; its review deferred exact boundaries here. 1.1 added the sys3 scalar path; its review deferred all-10-message coverage here. 1.10 added the duplicate-key fixture; its review deferred the negative-gate reason-check here. 1.11 hardened the receiver digest gate (run-time integrity); this hardens projection determinism + crypto (FR8/HR1) and the codegen gate's reason-checking. The wire is frozen across all of these (CR1/AD-9) — 1.12 is pure lock-in.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.12] — user story + 3 ACs.
- [Source: refs/BRIEF.md#D, #G] — exact projection / digest round-trips; testable invariants.
- [Source: ARCHITECTURE-SPINE.md#FR8, #NFR4, #HR1] — byte-exact canonical projection; determinism; SHA-256 + url-encode KATs.
- [Source: grpc-routing-meta/example/proto/process_context.proto:20-27] — pctx keys (golden).
- [Source: grpc-routing-meta/example/proto/sys3.proto:19-28] — Submit01–10 getter shapes.
- [Source: grpc-routing-meta/example/src/common/process_context_emit.h:29-31,56] — limits + strict-`>` overflow (boundaries).
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc] — `Validate` error strings (negative-gate reasons).
- [Source: grpc-routing-meta/example/build.sh:58-72] — `[neg ]` loop (Task 6).
- [Source: deferred-work.md] — the three items 1.12 resolves (sys3 coverage / exact boundaries / gate reason).

### Latest tech notes
No external research. SHA-256 NIST KATs (empty, 56-byte two-block, 1M-'a' multi-block) are public constants; the 55/56-byte pair pins the padding boundary. C++17, plain `assert`, no new deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (engineering subagent under Amelia/dev-story; main-loop independent verification)

### Debug Log References

- Build: `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; hardened `[neg ]` gate prints `ok (rejected: <reason>)` for all four fixtures; `OK -> binaries`.
- `./build/test_projection` → `ALL TESTS PASSED` (golden, determinism, 4 SHA KATs, 10 sys3 messages, 3 exact boundaries).
- `./build/receiver_verify` → digest UNCHANGED `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd` (wire byte-identical); tamper self-check rejects.
- `./build/bench_projection` → `BENCH PASSED (guard=88000)`; `./build/unified_sender` → exit 0.

### Completion Notes List

- **AC1 (golden)**: one context, all 7 fields, pinned byte-for-byte to `ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=&RecipeID=R%2FA&StageID=ETCH&Tech=N5` (key-sorted, `&`-joined, empty `PartID=`, `/`→`%2F`).
- **AC2 (determinism)**: same request projected into two sinks; full `items` sequences asserted equal.
- **AC3 (KATs)**: SHA-256 vs INDEPENDENT references — empty `e3b0c442…`, 56-byte NIST `248d6a61…`, 1M-'a' NIST `cdc76e5c…`, 55-'a' `9f4390f8…` (pinned via `python3 hashlib` + `shasum -a 256`, both agreeing — NOT recomputed by the kit). Existing `Sha256Hex("abc")` + url-encode asserts kept; added `UrlDecode("R%2FA")=="R/A"` + high-byte round-trip.
- **Task 4 (deferred 1.1)**: all 10 sys3 Submit01–10 mask getter paths → `x-mask-id` asserted; plus empty-required on Submit10 (deep-nested) → `!ok` + `x-routing-error: missing:x-mask-id`.
- **Task 5 (deferred 1.2)**: exact boundaries pinned by empirical calibration — count 25→ok / 26→overflow; line `PartID(449)`→512 ok / `PartID(450)`→513 overflow (skeleton line = 63 B measured); total 21×`PartID(200)`+1×`PartID(238)`→`sink.bytes()`=7168 ok / 7169 overflow. Existing 30/600/~9 KB over-the-line tests kept.
- **Task 6 (deferred 1.10)**: `build.sh` `[neg ]` gate now captures codegen stderr and requires each fixture's expected `Validate` substring (bash-3.2-safe `case` map): `bad_duplicate_key`→`duplicate (routing.project) key`; `bad_message_project`→`a message field cannot project to a single-valued header`; `bad_repeated_scalar`→`a repeated field cannot project to a single-valued header`; `bad_project_under_repeated`→`is set under repeated field`. Gate `exit 1` on accept OR wrong-reason reject.
- **No drift**: every lock passes against current behavior; no kit/plugin/proto/wire byte changed (digest identical to pre-edit baseline). Pure regression lock-in (CR1/AD-9).

### File List

- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — additive: golden, determinism, KATs, sys3×10, boundary blocks)
- `grpc-routing-meta/example/build.sh` (MODIFIED — `[neg ]` gate hardened to assert rejection reason)

## Change Log

- 2026-06-28 — Story 1.12 drafted (create-story): lock canonical projection + crypto by tests. Adds a byte-exact golden context, a determinism check, SHA-256 KATs (empty/55/56/1M-'a') + url-encode round-trips, all-10 sys3 getter-path asserts, and exact-threshold overflow boundaries (25/26, 512/513, 7168/7169); hardens `build.sh`'s `[neg ]` gate to assert the rejection REASON. Resolves three deferred items (1.1 sys3 coverage, 1.2 boundaries, 1.10 gate reason). No wire/projection change — pure regression lock (CR1/AD-9).
- 2026-06-28 — Story 1.12 implemented (dev-story): all 7 tasks done. `tests/test_projection.cc` + `build.sh` only. Golden literal pinned; determinism via dual-sink equality; 4 SHA-256 KATs vs independent `python3`/`shasum` references; all 10 sys3 getter paths + deep-nested missing-required; exact boundaries calibrated empirically (line skeleton=63 B → `PartID(449/450)`=512/513; total 21×200+1×238=7168/7169); `[neg ]` gate reason-checks all four fixtures (bash-3.2-safe). Build links, `ALL TESTS PASSED`, `receiver_verify` digest UNCHANGED (`efafba16…`), bench PASSED. No drift — zero wire bytes changed. Status → review.
- 2026-06-28 — Story 1.12 code review (3 adversarial reviewers, no shared context): Acceptance Auditor + Edge Case Hunter PASS (all 3 ACs MET, all 3 deferred items RESOLVED-in-code, wire frozen verified, build/test/digest re-run green). One Blind-Hunter MED accepted: the canonical **digest value** was only `!empty()`-checked, so a preimage-construction drift (join/order) could slip past. Resolved: pinned the golden single-context digest to an INDEPENDENT reference `sha256:3c8087d9…bf31b` (= `shasum -a 256` of the golden line) in the Task-1 block — this both proves and locks that the kit's digest preimage IS the canonical line. Other Blind findings dismissed with cause: high-byte encode-leak already locked by the pre-existing `UrlEncode("\xC3\xA9")=="%C3%A9"` (test:43); 7168 total + 55-byte KAT independently reconstructed/recomputed correct by both repo-access reviewers. Rebuilt: `ALL TESTS PASSED`, digest still `efafba16…`. Test-only addition; no wire change.

## Senior Developer Review (AI)

**Date:** 2026-06-28 · **Outcome:** Approve (1 Med fixed, no High). Three independent adversarial reviewers, no shared context.

- **Acceptance Auditor (spec/arch):** AC1/AC2/AC3 MET; deferred 1.1/1.2/1.10 RESOLVED in code (not just ledger); AD-9/CR1 wire-frozen verified via `--stat` (only `test_projection.cc` + `build.sh`); bookkeeping correct. No High/Med.
- **Edge Case Hunter (repo + build):** Ran build + binaries; `ALL TESTS PASSED`, digest `efafba16…` unchanged. Independently reconstructed the 63 B skeleton, the 449/450 line boundary, and the 7168 total (incl. HPACK +32 accounting) — all exact and would catch a `>`→`>=` slip. All 10 sys3 getter paths match the proto; the 4 `[neg ]` `want` substrings are verbatim from `protoc-gen-meta.cc` and a wrong-reason reject now fails the gate. No High/Med.
- **Blind Hunter (diff-only):** Med — digest value never pinned (only `!empty()`). **Action taken (fixed):** pinned golden single-context digest to independent `sha256:3c8087d9…bf31b`. Its other findings were artifacts of no-repo-access and dismissed with cause: high-byte encode already locked at `test:43`; 7168 and the 55-byte KAT independently verified correct by the two repo reviewers.

### Action Items
- [x] [AI-Review][Med] Pin the canonical digest value for the golden case to an independent reference (closes preimage-drift gap) — done in `tests/test_projection.cc` Task-1 block.
