---
baseline_commit: 3285bbd5bef53375056da3f49bea6f42cf71eec1
---

# Story 1.10: Build-time negative-codegen gate, proven by fixtures

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a system provider,
I want a malformed annotation to fail the build,
so that a bad `(routing.project)` never reaches the Sender.

## Acceptance Criteria

1. **AC1 — Fixtures cover all four malformed shapes.** Given `tests/negative/*.proto` fixtures, when present, then they cover `(routing.project)` on a repeated field, on a message-typed field, on a field under a repeated, and a **duplicate projected key**. (AD-8, SPEC §9)

2. **AC2 — Each fixture fails codegen and the build asserts it.** Given each negative fixture, when `protoc --meta_out` runs in `build.sh` and CI, then codegen fails with non-zero exit and the build asserts the rejection. (FR4)

3. **AC3 — No false positive on a valid proto.** Given a valid proto, when codegen runs, then it succeeds.

## Tasks / Subtasks

- [x] **Task 1 — Add the missing duplicate-projected-key fixture (AC: 1, 2)** — NEW file `example/tests/negative/bad_duplicate_key.proto`
  - [x] Three of the four AC1 shapes already exist: `bad_repeated_scalar.proto` (repeated field), `bad_message_project.proto` (message-typed field), `bad_project_under_repeated.proto` (field under a repeated). The ONLY missing shape is **duplicate projected key**.
  - [x] Author a fixture with TWO non-repeated scalar leaves in ONE message, both projecting the SAME header key — so the only reason for rejection is the duplicate key (the two fields are otherwise valid scalar leaves, isolating the duplicate-key branch from `ProjectOnlyOnScalarLeaf`/`NoProjectUnderRepeated`):
    ```proto
    // NEGATIVE fixture — codegen MUST reject this (see build.sh "[neg ]").
    // TWO scalar leaves projecting the SAME key: a single-valued header would be
    // emitted twice. Validate() must fail loud (duplicate-key branch).
    syntax = "proto3";
    package neg.v1;
    import "metadata_options.proto";

    message BadDuplicateKey {
      string mask_id   = 1 [(routing.project) = {key: "x-mask-id"}];
      string mask_id_2 = 2 [(routing.project) = {key: "x-mask-id"}];
    }
    ```
  - [x] Match the banner-comment style of the three existing fixtures (one-line "codegen MUST reject this" + why).
  - [x] No plugin change: `Validate()` already rejects this (`src/plugin/protoc-gen-meta.cc:150-156`, the `std::set<std::string> keys` duplicate-insert check). This story PROVES that existing capability with the fixture the gate was missing.
  - [x] No `build.sh` change: the `[neg ]` gate globs `tests/negative/*.proto` (`build.sh:59`), so the new fixture is picked up automatically.

- [x] **Task 2 — Verify rejection (right reason) + no false positive (AC: 2, 3)**
  - [x] `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → the `[neg ]` block now prints `ok (rejected): bad_duplicate_key.proto` alongside the other three, and the build does NOT abort (gate stays green). The four binaries still link; `OK -> binaries`.
  - [x] Confirm the rejection reason is the duplicate-key check (not a spurious one): run the plugin directly on the fixture and check stderr — `"$PROTOC" -I proto -I "$(pkg-config --variable=includedir protobuf)" -I tests/negative --plugin=protoc-gen-meta=build/protoc-gen-meta --meta_out=build/generated tests/negative/bad_duplicate_key.proto` → non-zero exit, stderr contains `duplicate (routing.project) key "x-mask-id" in message BadDuplicateKey`.
  - [x] **AC3 (no false positive):** the valid `sys1`/`sys2`/`sys3` protos still codegen successfully in the same `build.sh` run (they always have — the build aborts if any valid proto is rejected). The duplicate-key check is **per-message** (`keys` is local to each `Validate(message)` call, `protoc-gen-meta.cc:150`), so the SAME key in two DIFFERENT messages is intentionally allowed and does not false-positive — note this; it is why the sys protos (each projecting its own keys) pass.
  - [x] Regression: `./build/test_projection` → `ALL TESTS PASSED`; `./build/bench_projection` → `BENCH PASSED`; `./build/receiver_verify` → digest OK. (No code/wire change in this story; these are unchanged.)

## Dev Notes

### Method (Amelia)
No production code changes — the plugin already enforces the rule; this story makes it **tested**. The discipline: (1) add the fixture; (2) prove it is REJECTED for the RIGHT reason (capture the `duplicate (routing.project) key` error), which is the "red" that the gate now catches; (3) prove no false positive (the valid sys protos still codegen). "Green" = `build.sh`'s `[neg ]` gate rejects all four fixtures and the build completes with all binaries.

### The capability already exists (this is a coverage story)
- `Validate()` (`src/plugin/protoc-gen-meta.cc:146-159`) runs three checks before any output is emitted (fail-loud, "never silent"): `ProjectOnlyOnScalarLeaf` (no project on a message-typed field), the **duplicate-key check** (`:150-156`: collect `walkProj` keys into a `std::set`, reject on duplicate insert with `duplicate (routing.project) key "<k>" in message <M>`), and `NoProjectUnderRepeated` (no project on a scalar under a repeated, and `walkProj` itself skips repeated scalars). The four AC1 shapes map 1:1 onto these guards — three already have fixtures, the duplicate-key guard did not. AC1/SPEC §9 lists all four, so the gate was under-covering.
- Because the check is real and the gate already globs the directory, the ENTIRE story is one fixture file. Resist adding a plugin "fix" — there is no bug to fix; adding logic would be scope creep against a passing guard.

### Why the fixture isolates the duplicate-key branch
- Two NON-repeated scalar leaves with the same key: `ProjectOnlyOnScalarLeaf` passes (both are scalar leaves), `NoProjectUnderRepeated` passes (no repeated), so the ONLY trigger is the duplicate-key check. If the fixture instead used a repeated/message field, a DIFFERENT guard would reject it first and the duplicate-key branch would go untested. Keep both fields plain `string` scalars in one message.
- `walkProj` also descends non-repeated sub-messages, so a duplicate key split across two nested non-repeated messages is caught too — but the flat two-scalar form is the clearest fixture and the one to commit.

### What must be preserved (system still works end-to-end)
- **No code/wire change (CR1/AD-9):** this story adds a test fixture only. The plugin, generated code, kit, sender, receiver, and all four binaries are byte-identical; their self-checks are unchanged.
- **The gate stays the build-time hard-gate (criterion G/FR4):** `build.sh`'s `[neg ]` loop must still reject every `tests/negative/*.proto` and `exit 1` if any is accepted. The new fixture joins that set; do not weaken the loop.
- **No false positive (AC3):** the duplicate-key check is per-message-scoped — do not change it to a file-global set (that would wrongly reject two different request types projecting the same header). The valid sys protos are the standing AC3 evidence.

### Guardrails (do NOT do in this story)
- Do NOT edit the plugin, `build.sh`, generated code, kit, or any binary — fixture file only.
- Do NOT make the duplicate-key check file-global or otherwise alter `Validate()` — it is correct and already passing the other fixtures.
- Do NOT add a positive-fixture harness; AC3 is covered by the existing valid sys1/2/3 codegen in `build.sh`.
- Keep the fixture minimal and in `neg.v1` like its siblings; `import "metadata_options.proto"` for the `(routing.project)` extension.

### Current state of the things this story touches (read before editing)
- **`example/tests/negative/`** — three fixtures today (`bad_message_project.proto`, `bad_project_under_repeated.proto`, `bad_repeated_scalar.proto`), all `package neg.v1; import "metadata_options.proto";`, each a single message with one mis-annotated field and a banner comment. Add the fourth alongside them.
- **`example/build.sh:58-72`** — the `[neg ]` gate: `for f in tests/negative/*.proto; do … --meta_out … && FAIL else ok(rejected); done; [ neg_ok ] || exit 1`. Globs the dir, so no edit needed.
- **`example/src/plugin/protoc-gen-meta.cc:146-159`** — `Validate()` with the three guards incl. the duplicate-key check at `:150-156`. Read-only here.

### Testing standards
- The "test" is the negative-codegen gate itself: `build.sh`'s `[neg ]` block must print `ok (rejected): bad_duplicate_key.proto` and keep the build green; a direct plugin run on the fixture must emit the `duplicate (routing.project) key` error with non-zero exit. AC3 is the existing valid-proto codegen (sys1/2/3) succeeding in the same run. No unit-test or binary changes.

### Project Structure Notes
- NEW: `example/tests/negative/bad_duplicate_key.proto`. No other file changes. (CI picks it up for free — `.github/workflows/ci.yml` runs `build.sh`, which globs the negative dir.)

### Previous story intelligence (Stories 1.1–1.9)
- The plugin's `Validate()` (with the duplicate-key branch) predates this story — it was authored defensively in the baseline plugin. 1.9's CI runs `build.sh` per matrix cell, so this fixture is automatically gated across the whole {gcc,clang}×{3.20.3,3.21.12} matrix once added — no CI change needed. Story 1.12 (test hardening) owns exact-threshold/coverage items; the boundary cases for the negative gate beyond these four shapes (if any) would live there, not here.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.10] — user story + 3 ACs (the four shapes incl. duplicate projected key).
- [Source: refs/BRIEF.md#G + Verify] — "codegen negative tests run in CI"; "a bad-annotation proto fails codegen (build.sh asserts this)".
- [Source: ARCHITECTURE-SPINE.md#AD-8] — negative-codegen gate; malformed `(routing.project)` fails the build (the four shapes).
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:146-159] — `Validate()`; duplicate-key check at `:150-156` (per-message `std::set` of keys).
- [Source: grpc-routing-meta/example/build.sh:58-72] — the `[neg ]` gate globbing `tests/negative/*.proto`.
- [Source: grpc-routing-meta/example/tests/negative/*.proto] — the three existing fixtures to mirror.

### Latest tech notes
No external research. protobuf custom option `(routing.project)` on `FieldOptions` (defined in `metadata_options.proto`); a duplicate target key is a contract error the plugin already rejects at build time. C++17 plugin; no new deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer) — fixture authored by a general-purpose subagent; main loop did create-story, independent verification (re-ran the gate + the right-reason plugin run + a per-message false-positive probe), and BMad bookkeeping.

### Debug Log References

- Fixture creation delegated to a subagent; main loop independently re-verified before marking review.
- Gate (independently re-run, `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig ./build.sh`): `[neg ]` prints `ok (rejected)` for all FOUR fixtures (`bad_duplicate_key`, `bad_message_project`, `bad_project_under_repeated`, `bad_repeated_scalar`), build does NOT abort, reaches `OK -> binaries`.
- Right-reason (direct plugin run on the fixture): `--meta_out: bad_duplicate_key.proto: duplicate (routing.project) key "x-mask-id" in message BadDuplicateKey — a single-valued header would be emitted twice`, `exit=1`. So the duplicate-key branch (`protoc-gen-meta.cc:152-155`) is the sole trigger, not a spurious guard.
- AC3 false-positive probe (main loop, beyond the subagent's report): a throwaway proto with the SAME key `x-mask-id` in TWO different messages codegens cleanly (`exit=0`) — directly confirming the duplicate-key check is per-message-scoped and does NOT false-positive across messages.
- Regression: `test_projection` → ALL TESTS PASSED; `bench_projection` → BENCH PASSED (0); `receiver_verify` → digest OK.
- Scope: `git status --short` → only the one new fixture in the code dir; plugin/`build.sh`/code/wire untouched.

### Completion Notes List

- **AC1** — `tests/negative/bad_duplicate_key.proto` adds the fourth malformed shape (duplicate projected key) alongside the existing repeated-scalar, message-typed, and under-repeated fixtures. Two non-repeated scalar leaves project the same `x-mask-id`, isolating the duplicate-key branch.
- **AC2** — the `[neg ]` gate (which globs `tests/negative/*.proto`) rejects it with non-zero exit and the build asserts the rejection; verified the exact error is the duplicate-key message. CI (1.9) gates it across the whole matrix for free (it runs `build.sh`).
- **AC3** — no false positive: the valid sys1/2/3 protos still codegen in the same run, and I additionally proved the per-message scoping (same key in two messages → codegens). 
- **Scope held / CR1·AD-9** — fixture file only; no plugin/`build.sh`/code change; wire byte-identical; the duplicate-key guard was pre-existing (`Validate()` `:150-156`) — this story proves it, doesn't add it.

### File List

- `grpc-routing-meta/example/tests/negative/bad_duplicate_key.proto` (NEW — negative fixture: two scalar leaves projecting the same key; exercises the plugin's pre-existing duplicate-key `Validate` branch)

## Change Log

- 2026-06-28 — Story 1.10 drafted (create-story): add the missing `tests/negative/bad_duplicate_key.proto` fixture (two scalar leaves, same projected key) to complete AC1's four malformed shapes. The plugin's `Validate()` already rejects duplicate keys (`protoc-gen-meta.cc:150-156`) and `build.sh`'s `[neg ]` gate globs the dir — so this is a one-fixture coverage story proving an existing guard; no plugin/build/code change. AC3 (no false positive) covered by the existing valid sys1/2/3 codegen + the per-message scoping of the check.
- 2026-06-28 — Story 1.10 implemented (dev-story): NEW `tests/negative/bad_duplicate_key.proto`. Gate rejects all four fixtures (build stays green, reaches `OK -> binaries`); direct plugin run confirms the duplicate-key error is the sole reason (exit 1); per-message false-positive probe (same key in two messages) codegens (exit 0); regression binaries green. Fixture only — no plugin/build/code/wire change. Engineering by a subagent, independently re-verified in the main loop. Status → review.
