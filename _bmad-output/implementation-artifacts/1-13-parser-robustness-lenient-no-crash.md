---
baseline_commit: 57309edb76795a5473e08c2521a1d080759b5633
---

# Story 1.13: Parser robustness (lenient, no-crash)

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a receiver,
I want lenient parsing that never crashes on malformed input,
so that a bad header degrades gracefully per spec.

## Acceptance Criteria

1. **AC1 — Malformed `%`-escape passes through literally (HR2).** Given a context value with a malformed `%`-escape (a trailing `%`, a truncated `%X`, or a non-hex digit `%XZ`), when the receiver `UrlDecode`/`ParseContext` parses it, then the malformed escape is passed through **literally** (SPEC §6) and parsing does not crash. A WELL-formed escape still decodes (`%2F`→`/`).

2. **AC2 — Duplicate keys: last-wins, and both rules documented.** Given duplicate keys within one context (`K=a&K=b`), when parsed, then **last wins** (`K`→`b`). This rule AND the lenient-parse rule (AC1) are **documented** in the parser header.

3. **AC3 — No crash on a garbled corpus.** Given a set of negative / garbled inputs (lone `%`, `%%%`, `&&&`, `=`, `=v`, `k=`, `k=v=w`, no-`=`, empty, high-byte, long), when parsed, then no crash / no UB occurs on any of them, and each returns a sane (possibly empty) map.

## Tasks / Subtasks

> Receiver-side robustness LOCK + DOC. The parser (`src/common/process_context_parser.h`) is ALREADY lenient (bounded `%` read, hex-validity guard, `=`-less pairs skipped, `std::map` last-wins). This story PROVES it with negative tests and DOCUMENTS the two rules. Tests + comments only — **no sender/wire/projection byte changes** (UrlDecode/ParseContext are receiver-side; they never touch emitted bytes). If a garbled input genuinely crashes the parser, fixing that receiver-side robustness bug is IN SCOPE for HR2 (it cannot change emitted bytes); but per analysis none should.

- [x] **Task 1 — Malformed `%`-escape pass-through (AC: 1, HR2)** — edit `tests/test_projection.cc`
  - [x] Assert `UrlDecode` passes malformed escapes through LITERALLY and does not crash:
    - trailing `%`: `UrlDecode("R%") == "R%"`, `UrlDecode("%") == "%"`
    - truncated: `UrlDecode("%2") == "%2"`, `UrlDecode("a%2") == "a%2"`
    - non-hex digit: `UrlDecode("%2G") == "%2G"`, `UrlDecode("%G2") == "%G2"`, `UrlDecode("%ZZ") == "%ZZ"`
    - well-formed still decodes (lower+upper hex both accepted): `UrlDecode("%2F") == "/"`, `UrlDecode("%2f") == "/"`
    - mixed: `UrlDecode("a%2Fb%ZZc%") == "a/b%ZZc%"` (one valid, one bad, one trailing)
  - [x] Assert the same leniency through `ParseContext`: `ParseContext("RecipeID=R%2FA")["RecipeID"] == "R/A"`; `ParseContext("RecipeID=R%ZZ")["RecipeID"] == "R%ZZ"` (malformed value survives literally, no crash).

- [x] **Task 2 — Duplicate keys last-wins + documentation (AC: 2)** — edit `tests/test_projection.cc` AND `src/common/process_context_parser.h`
  - [x] Test: `ParseContext("RecipeID=A&RecipeID=B")["RecipeID"] == "B"` (last wins); a 3-way `K=a&K=b&K=c` → `c`; and that an unrelated key in the same string is unaffected.
  - [x] **Document** both rules in `process_context_parser.h` (comment-only, NO logic change): above `UrlDecode`, state malformed `%`-escapes (trailing `%` / non-hex) are passed through literally per SPEC §6 and never crash; above `ParseContext`, state pairs without `=` are skipped and **duplicate keys → last-wins** (`std::map` assignment). This satisfies AC2's "documented" requirement (the kit has no separate docs file; the header is the doc surface — keep it in sync, anticipating Story 1.15).

- [x] **Task 3 — No-crash garbled corpus (AC: 3)** — edit `tests/test_projection.cc`
  - [x] Drive a table of garbled inputs through `ParseContext` (and a few through `VerifyDigest`) and assert each RETURNS without crashing/UB; assert exact maps where meaningful:
    - `""` → empty map; `"%"`, `"%%%"`, `"&&&"`, `"&=&=&"` → no crash (no `=`-pair or empty pairs skipped)
    - `"="` → key `""`→`""`; `"=v"` → `""`→`"v"`; `"k="` → `"k"`→`""`; `"k"` (no `=`) → skipped (empty map)
    - `"k=v=w"` → `"k"`→`"v=w"` (first `=` splits; rest literal incl. `=`)
    - high-byte raw value `"k=\xC3\xA9"` → returns (value is the raw bytes, decode leaves non-`%` bytes as-is)
    - a long value (e.g. `"k=" + std::string(5000,'x')`) → returns, value length preserved
  - [x] Include at least one `VerifyDigest` call on garbled/empty contexts to confirm the verify path also never crashes (e.g. `VerifyDigest({}, "")` and `VerifyDigest({"garb=%"}, "sha256:0…")` just return a `VerifyResult`).

- [x] **Task 4 — Build & verify (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; `[neg ]` gate still green (unchanged).
  - [x] `./build/test_projection` → `ALL TESTS PASSED` (new malformed-escape, last-wins, garbled-corpus blocks included).
  - [x] Regression: `./build/receiver_verify` → digest OK and UNCHANGED `sha256:efafba16…` (parser-comment + tests don't touch the wire); `./build/bench_projection` → BENCH PASSED; `./build/unified_sender` runs.

## Dev Notes

### Method (Amelia)
Robustness LOCK + DOC, mirroring the 1.10–1.12 cadence. The parser is already correct; each test must be NON-VACUOUS (assert the exact literal pass-through / exact last-wins value, not just "didn't crash"). The "no crash" ACs are proven by the test binary exiting 0 with `ALL TESTS PASSED` after driving every garbled input — a crash/UB would abort the run. Document the two behavioral rules where the behavior lives (the header), since the kit has no separate doc file.

### Current parser behavior (read before editing) — already lenient
- **`UrlDecode` (`process_context_parser.h:21-42`):** `if (in[i]=='%' && i+2 < in.size())` bounds the 2-char read (no OOB on a trailing/truncated `%`); the `hi>=0 && lo>=0` hex-validity guard (`:33`) means a non-hex digit falls through to `out.push_back(in[i])` — the `%` is emitted literally and the following chars are processed normally. Accepts upper AND lower hex (`:24-28`). This is exactly SPEC §6's "malformed escape passed through literally, MUST NOT crash."
- **`ParseContext` (`:45-59`):** splits on `&`, then first `=`; a pair with no `=` is **skipped** (`if (eq != npos)`); key and value are each `UrlDecode`d; `kv[key] = value` on a `std::map` ⇒ **duplicate keys last-wins**. No unbounded recursion, no unchecked index — cannot crash on any byte string.
- **`VerifyDigest` (`:71-89`):** pure string concat + hash + compare; empty-digest and mismatch are returned as data (`error` string), never thrown.

### Why test-and-document, not change (HR2 + AD-12)
- HR2 (spine `:162`) requires the receiver parse be lenient: malformed `%`-escape → literal (SPEC §6), duplicate keys → last-wins, negative inputs must not crash. The architecture (spine `:203`, invariant G) explicitly lists **"receiver parser negative tests (HR2)"** as the deliverable — i.e. the gap is the *tests*, not the parser. AD-12 (pure/re-entrant) is already satisfied by the header-only, allocation-only implementation.
- The parser is **receiver-side**: `UrlDecode`/`ParseContext`/`VerifyDigest` are never on the sender/projection path, so nothing here can change an emitted byte. The wire stays frozen (CR1/AD-9); `receiver_verify`'s `efafba16…` digest is unaffected by comments + new tests.

### Documentation surface (AC2 "documented")
- The kit ships no `README`/`docs/*.md` (verified: no markdown under `grpc-routing-meta/example`). The parser header IS the doc surface — document the lenient-parse + last-wins rules there as block comments above the two functions. Story 1.15 ("live docs match the code") will treat these header comments as the authoritative doc; keeping them exact now feeds that.

### What must be preserved (system still works end-to-end)
- **No wire/projection byte changes:** sender projection, plugin, generated code, `EmitProcessContexts`, emitted headers, and the digest are untouched. `receiver_verify` digest stays `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd`.
- **No logic change to `UrlDecode`/`ParseContext`/`VerifyDigest`** unless a test proves a real crash/UB (none expected). If one is found, the fix is receiver-side only and still cannot change emitted bytes — surface it explicitly as a robustness fix.
- **Existing tests keep passing:** the round-trip `UrlDecode(UrlEncode(...))` asserts (`test_projection.cc:44,307`), the digest-gate / tamper block, the 1.12 locks — all stay green.
- **`assert`-based harness:** plain `assert`, `-O2 -Wall`, no `-DNDEBUG`. `ALL TESTS PASSED`.

### Guardrails (do NOT do in this story)
- Do NOT change emitted/wire bytes, the plugin, the projection, or `UrlEncode` (sender-side). Receiver parser tests + header doc-comments ONLY (+ a receiver-side robustness fix only if a real crash is proven).
- Do NOT weaken or delete existing asserts (round-trips, digest gate, 1.12 locks).
- Do NOT add a new test file or dependency — extend `tests/test_projection.cc` (the kit's single test binary, wired in `build.sh:104-105`).
- Keep assertions non-vacuous: pin exact literal outputs, not just "no crash".

### Project Structure Notes
- Edits: `tests/test_projection.cc` (malformed-escape, last-wins, garbled-corpus blocks) and `src/common/process_context_parser.h` (doc-comments only). No proto/plugin/sender/wire change.

### Previous story intelligence (Stories 1.10–1.12)
- 1.10 added the negative-codegen fixtures (build-time gate); 1.11 hardened the receiver DIGEST gate (run-time integrity — tamper rejected); 1.12 locked canonical projection + crypto vectors (FR8/NFR4/HR1) and reason-checked the codegen gate. 1.13 completes the receiver story: parse-time ROBUSTNESS (HR2) — the lenient/last-wins/no-crash contract. The wire has been frozen across all of these (CR1/AD-9); 1.13 is receiver-side test + doc, no wire impact. The `assert`-based single-binary pattern and "lock current correct behavior, prove it would fail on drift" discipline carry over directly.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.13] — user story + 3 ACs.
- [Source: refs/SPEC.md#6] — URL-encoding frozen; "malformed escape … passed through literally and MUST NOT crash the parser."
- [Source: ARCHITECTURE-SPINE.md:162] — HR2: lenient parse, malformed `%`→literal, duplicate keys last-wins, negative inputs must not crash.
- [Source: ARCHITECTURE-SPINE.md:203] — invariant G lists "receiver parser negative tests (HR2)" as the deliverable.
- [Source: grpc-routing-meta/example/src/common/process_context_parser.h:21-89] — `UrlDecode` / `ParseContext` / `VerifyDigest` (the code under test + doc).
- [Source: grpc-routing-meta/example/tests/test_projection.cc:44,71-104,306-307] — existing parser/verify/round-trip asserts to extend (not weaken).
- [Source: grpc-routing-meta/example/build.sh:104-105] — `test_projection` build wiring.

### Latest tech notes
No external research. RFC 3986 url-decoding + SPEC §6 leniency are fixed; C++17, plain `assert`, no new deps. `std::map` operator[] last-wins is standard.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (engineering subagent under Amelia/dev-story; main-loop independent verification)

### Debug Log References

- Build: `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; `[neg ]` gate unchanged (4 fixtures rejected for the right reason); `OK -> binaries`.
- `./build/test_projection` → `ALL TESTS PASSED` (malformed-escape, last-wins, garbled-corpus blocks).
- `./build/receiver_verify` → digest UNCHANGED `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd`.
- `./build/bench_projection` → `BENCH PASSED (guard=88000)`; `./build/unified_sender` → exit 0.

### Completion Notes List

- **AC1 (malformed escape → literal, HR2)**: 10 `UrlDecode` asserts + 2 `ParseContext` leniency asserts (12 AC1 asserts) pin literal pass-through — trailing `%` (`"R%"`,`"%"`), truncated (`"%2"`,`"a%2"`), non-hex (`"%2G"`,`"%G2"`,`"%ZZ"`), well-formed upper+lower (`"%2F"`/`"%2f"`→`/`), and mixed `"a%2Fb%ZZc%"`→`"a/b%ZZc%"`. Same leniency through `ParseContext` (`"R%2FA"`→`R/A`, `"R%ZZ"`→`R%ZZ`). Subagent traced `"a%2Fb%ZZc%"` against the real `UrlDecode` — output matches the derived literal exactly (no code/story divergence).
- **AC2 (last-wins + documented)**: `ParseContext("RecipeID=A&RecipeID=B")["RecipeID"]=="B"`, 3-way `K=a&K=b&K=c`→`c`, unrelated key unaffected. Documented both rules in `process_context_parser.h` as comment-only block comments (verified: `git diff` shows zero non-comment added lines) — lenient `%`-pass-through above `UrlDecode`, `=`-less-skip + duplicate-key-last-wins above `ParseContext`.
- **AC3 (no-crash garbled corpus)**: drove `""`,`"%"`,`"%%%"`,`"&&&"`,`"&=&=&"`,`"="`,`"=v"`,`"k="`,`"k"`,`"k=v=w"`, high-byte `"k=\xC3\xA9"`, and a 5000-char value through `ParseContext`; plus `VerifyDigest({}, "")` and `VerifyDigest({"garb=%","x"}, sha256:0…)` — each returns a sane `map`/`VerifyResult`, no crash (binary reaches `ALL TESTS PASSED`). Non-vacuous: exact map sizes/contents pinned (`"&=&=&"`→size 1 `""→""`; `"k=v=w"`→`"v=w"`).
- **No drift**: parser already lenient; only `tests/test_projection.cc` + `process_context_parser.h` comments changed. Receiver-side only → zero wire/projection bytes changed; digest `efafba16…` identical; `[neg ]` gate untouched.

### File List

- `grpc-routing-meta/example/tests/test_projection.cc` (MODIFIED — additive: malformed-escape, last-wins, garbled-corpus blocks)
- `grpc-routing-meta/example/src/common/process_context_parser.h` (MODIFIED — doc-comments only above `UrlDecode` and `ParseContext`; no logic change)

## Change Log

- 2026-06-28 — Story 1.13 drafted (create-story): lock receiver parser robustness (HR2) by negative tests + document the two rules. Adds malformed `%`-escape literal-pass-through asserts (trailing/truncated/non-hex), duplicate-key last-wins, and a no-crash garbled corpus through `ParseContext`/`VerifyDigest`; documents lenient-parse + last-wins in `process_context_parser.h`. Parser already lenient — test + doc only, no wire/projection change (receiver-side; CR1/AD-9 preserved).
- 2026-06-28 — Story 1.13 implemented (dev-story): all 4 tasks done. `tests/test_projection.cc` (malformed-escape + last-wins + garbled-corpus asserts, all non-vacuous) + `process_context_parser.h` (comment-only docs for both rules — verified zero non-comment added lines). Subagent traced `UrlDecode("a%2Fb%ZZc%")`→`"a/b%ZZc%"` against the real code (matches). Build links, `ALL TESTS PASSED`, `receiver_verify` digest UNCHANGED (`efafba16…`), bench PASSED, `[neg ]` gate untouched. No drift — parser already lenient, receiver-side only, zero wire bytes changed. Status → review.
- 2026-06-28 — Story 1.13 code review (3 adversarial reviewers, no shared context): Acceptance Auditor + Edge Case Hunter PASS — all 3 ACs MET, wire frozen verified, build/test/digest re-run green; Edge Case Hunter additionally ran an ASan/UBSan robustness probe (embedded NUL, `%00`, `%3D` key, `%26` value, trailing `%` at many lengths, 100k key) — all sanitizer-clean. Two Low fixes applied: (1) Blind Hunter — `ParseContext` doc misattributed last-wins to `std::map`; reworded to credit the `kv[key]=` assignment (doc-accuracy matters in a doc story). (2) Acceptance Auditor — added one `VerifyDigest(..., "sha256:zz")` malformed-digest-string no-crash assert, and corrected a Completion-Notes count (10 `UrlDecode` + 2 `ParseContext`, not 11). Dismissed with cause: "SPEC §6 unverifiable" (confirmed §6 = "URL-encoding (frozen)" with the leniency sentence); re-entrancy/AD-12 correctly deferred to Story 1.14. Rebuilt: `ALL TESTS PASSED`, digest still `efafba16…`, parser.h still comment-only. Status → done.

## Senior Developer Review (AI)

**Date:** 2026-06-28 · **Outcome:** Approve (2 Low fixed, no High/Med). Three independent adversarial reviewers, no shared context.

- **Acceptance Auditor (spec/arch):** AC1/AC2/AC3 MET; HR2/invariant-G deliverable ("receiver parser negative tests") discharged for the parse-leniency contract; AD-9/CR1 wire-frozen verified (`--stat` = comment-only header + additive test); bookkeeping accurate. Lows: VerifyDigest malformed-digest-string corner (fixed); Completion-Notes miscount (fixed); re-entrancy → 1.14 (correct deferral).
- **Edge Case Hunter (repo + build + sanitizers):** Ran build + binaries; `ALL TESTS PASSED`, digest `efafba16…` unchanged. Hand-traced every assert against the parser; all exact. ASan/UBSan probe of uncovered inputs (NUL, `%00`, decode-to-`=`/`&` keys/values, trailing `%`, 100k key) — no crash, sanitizer-clean. `operator[]` empty-expected asserts are size-guarded (non-vacuous). No defects.
- **Blind Hunter (diff-only):** Traced all literals (incl. `a%2Fb%ZZc%`→`a/b%ZZc%`) and corpus maps (incl. `&=&=&`→size 1) — all correct. Low: `ParseContext` doc misattributed last-wins to `std::map` rather than the `kv[key]=` assignment (**fixed**). "SPEC §6 unverifiable" dismissed (reference is correct).

### Action Items
- [x] [AI-Review][Low] Reword `ParseContext` last-wins doc to credit `kv[key]=` assignment, not `std::map` — done in `process_context_parser.h`.
- [x] [AI-Review][Low] Add `VerifyDigest(…, "sha256:zz")` malformed-digest-string no-crash assert — done in `tests/test_projection.cc`.
- [x] [AI-Review][Low] Correct Completion-Notes assert count (10 `UrlDecode` + 2 `ParseContext`) — done.
