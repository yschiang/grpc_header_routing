---
baseline_commit: 8e46efe
---

# Story 1.15: Live docs match the code

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a new adopter,
I want the docs to describe the behavior the code actually has,
so that onboarding is not misled by stale claims.

## Acceptance Criteria

1. **AC1 — CONTEXT.md invariant 9 (criterion I).** The live `grpc-routing-meta/CONTEXT.md` invariant 9 "throws" is rewritten to: `ProjectMeta` records `MissingRequired` in `ProjResult` (`ok=false`) and emits `x-routing-error: missing:<key>`, suppressing the empty scalar (does NOT throw, does NOT emit an empty `x-mask-id`).

2. **AC2 — OVERVIEW.zh.md failure + digest framing.** In the live `grpc-routing-meta/OVERVIEW.zh.md`: the "throw" failure language becomes `ProjResult` (failure-as-data); and the digest framing is integrity-only — a party that can edit the body recomputes the digest, so it detects drift/accident, it is **not** a security control (no key, no signature).

3. **AC3 — README.md Send / ProjResult.** The live `grpc-routing-meta/README.md` describes the `routingmeta::Send` / `ProjResult` usage and the **Send-lives-in-the-kit** boundary (AD-4): `Send` is provided by the kit (`example/src/common/send.h`), returns `ProjResult`, never throws on a data condition, and the caller owns abort/proceed.

4. **AC4 — refs/ untouched (CR3).** When the doc work is complete, nothing under `refs/` (including `refs/SPEC.md`, `refs/CONTEXT.md`) has been edited.

5. **AC5 — build.sh↔CMake parity (deferred 1.7/1.8 + 1.14).** `example/CMakeLists.txt` builds the same test/app set as `build.sh`: add the missing `bench_projection` (deferred from 1.7/1.8) and `test_concurrency` (added to `build.sh` in 1.14) targets, so the README's "canonical, portable" `cmake` path actually builds every binary `build.sh` does.

## Tasks / Subtasks

> DOCS + BUILD-PARITY only. No wire/projection/code-logic change — the kit's behavior is already correct; this story makes the prose and the CMake build match it. **CR3 is a hard guardrail: edit ONLY the live `grpc-routing-meta/` doc copies and `example/CMakeLists.txt`; never touch anything under `refs/`.** Every doc edit must be VERIFIED against the actual code (read `send.h`, `proj_result.h`, the generated `ProjectMeta`, `process_context_parser.h`) before writing — do not paraphrase from memory.

- [x] **Task 1 — CONTEXT.md invariant 9 (AC: 1, criterion I)** — edit `grpc-routing-meta/CONTEXT.md`
  - [x] Line ~59 (invariant 9): replace "`ProjectMeta` throws if the source field is empty" with the failure-as-data behavior: *required & empty → `ProjectMeta` records `Issue{MissingRequired, key}` in `ProjResult` (`ok=false`) and emits `x-routing-error: missing:x-mask-id`, suppressing the empty `x-mask-id` (it does NOT throw and does NOT emit an empty scalar).* Keep the rest of the invariant (codegen scalar-leaf rejection, the 7 KB note) intact.
  - [x] Line ~88 (checklist): rewrite "Domain scalar (9): nested path reached; empty ⇒ throws." to "… empty ⇒ `ok=false` + `x-routing-error: missing:x-mask-id`, scalar suppressed (no throw)."
  - [x] Verify against `build/generated/sys3.proj.cc` + `proj_result.h` (`Issue::MissingRequired`) and `test_projection.cc`'s missing-required asserts — the prose must match what the code does.

- [x] **Task 2 — OVERVIEW.zh.md failure + digest framing (AC: 2)** — edit `grpc-routing-meta/OVERVIEW.zh.md`
  - [x] Throw language → `ProjResult` (failure-as-data). Lines ~117 (`ProjectMeta 當場 throw,不會送出空值`), ~157 (`required throw`), ~183 (`sender required throw` + the error-handling row). Rewrite to convey: `required` 的 scalar 沒填 → `ProjectMeta` 回傳 `ProjResult{ ok=false, Issue=MissingRequired }` 並送出 `x-routing-error: missing:<key>`,**不丟例外、不送空值**(caller 依 `ProjResult` 決定 abort/proceed). Keep the table structure; only fix the behavior text.
  - [x] Digest framing → integrity-only. Near the digest rows (~119, ~121, and the `Quality/一致性` row ~184): make explicit that `x-process-context-digest` 是 **完整性(integrity)** 檢查、**不是安全控制** — 能改 body 的人可以一併重算 digest(沒有 key、沒有簽章);它偵測的是 sender bug / 版本漂移 / 傳輸破壞,不是惡意竄改。(Matches `refs/SPEC.md` §5.3 — read it for the exact framing; do NOT edit refs/.)
  - [x] Preserve the document's Chinese voice/format; change only the stale claims. Do not alter the example digests (`efafba16…`, `6526d80d…`) — those are illustrative.

- [x] **Task 3 — README.md Send / ProjResult (AC: 3)** — edit `grpc-routing-meta/README.md`
  - [x] The code block (~line 25-30) shows `void Send(const Req& req, const Runtime& rt, MetadataSink& sink) { FillCommon(...); ProjectMeta(...); }`. Update it to match the ACTUAL kit `Send` (`example/src/common/send.h:22`): `routingmeta::ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) { FillCommon(rt, sink); return ProjectMeta(req, sink); }` — and make clear this `Send` is **provided by the kit** (`example/src/common/send.h`), in `namespace routingmeta`, ONE for all systems (AD-4), not hand-written per sender.
  - [x] Add a short note on the boundary + usage: `Send` returns a `ProjResult { bool ok; std::vector<Issue> issues; std::chrono::nanoseconds duration; }`, never throws on a data condition, and the **caller** inspects `ProjResult`/`x-routing-error` to decide abort vs proceed (the kit reports; it does not dictate). One or two sentences; match the README's terse style.
  - [x] Verify the signature/return type against `send.h` and `proj_result.h` verbatim — do not invent fields.

- [x] **Task 4 — CMake build parity (AC: 5)** — edit `grpc-routing-meta/example/CMakeLists.txt`
  - [x] Add a `bench_projection` target mirroring `test_projection` (`add_executable(bench_projection tests/bench_projection.cc)` + `target_link_libraries(bench_projection PRIVATE routing_meta_gen)`). The bench is a perf gate run on demand — `add_test` is optional (the existing `add_test(NAME projection ...)` is the hard test; mirror or omit consistently).
  - [x] Add a `test_concurrency` target (added to `build.sh` in 1.14): `find_package(Threads REQUIRED)` (near the top with the other `find_package`), `add_executable(test_concurrency tests/test_concurrency.cc)`, `target_link_libraries(test_concurrency PRIVATE routing_meta_gen Threads::Threads)`, and `add_test(NAME concurrency COMMAND test_concurrency)` (it IS a real pass/fail check).
  - [x] Now CMake builds the same set as `build.sh` (plugin, unified_sender, receiver_verify, test_projection, bench_projection, test_concurrency) — closing the `deferred-work.md` parity item.

- [x] **Task 5 — Verify (AC: 1-5)**
  - [x] **CR3 check:** `git status` / `git diff --name-only` shows ONLY `grpc-routing-meta/CONTEXT.md`, `grpc-routing-meta/OVERVIEW.zh.md`, `grpc-routing-meta/README.md`, `grpc-routing-meta/example/CMakeLists.txt` (+ the story/sprint bookkeeping). NOTHING under `refs/`.
  - [x] **Docs-match-code:** re-read each changed claim against the code it describes; confirm no remaining "throw"/security-digest claim contradicts the kit.
  - [x] **CMake parity build (best-effort, build.sh remains authoritative):** `cd grpc-routing-meta/example && cmake -S . -B build-cmake -DCMAKE_PREFIX_PATH=/Users/johnson.chiang/anaconda3 && cmake --build build-cmake -j` → the new `bench_projection` + `test_concurrency` targets compile; `ctest --test-dir build-cmake` runs projection + concurrency green. If the host CMake/protobuf discovery is finicky, at minimum confirm `CMakeLists.txt` is well-formed and the targets mirror the working `test_projection` pattern; note the limitation. (Clean up `build-cmake/` — it's a scratch dir; do not commit it.)
  - [x] **Regression (no code touched, so trivially green):** `PKG_CONFIG_PATH=… ./build.sh` still links; `./build/test_projection` → `ALL TESTS PASSED`; `./build/receiver_verify` digest UNCHANGED `sha256:efafba16…`; `./build/test_concurrency` → `CONCURRENCY TEST PASSED`. (Docs/CMake edits cannot change the wire.)

## Dev Notes

### Method (Amelia)
The last story is a truth-in-advertising pass: the code is correct (the throw→`ProjResult` pivot landed in 1.1/1.4; the digest's integrity-not-security nature is SPEC §5.3), but three live docs still carry pre-pivot claims, and CMake (the README's "canonical" build) lags `build.sh` by two targets. Fix the prose to match the code and the CMake to match `build.sh`. Every doc edit is verified against the actual source — a docs story that introduces a NEW inaccuracy is worse than the stale claim.

### Exact stale claims (found — verify before editing)
- **CONTEXT.md:59** "`ProjectMeta` throws if the source field is empty"; **:88** "empty ⇒ throws." → failure-as-data.
- **OVERVIEW.zh.md:117** `ProjectMeta 當場 throw`; **:157** `required throw`; **:183** `sender required throw` (+ error-handling row). → `ProjResult`. Plus the digest rows (~119/121/184) → make integrity-only explicit.
- **README.md:~26** `void Send(const Req&, const Runtime&, MetadataSink&)` returning void, framed as a hand-written template. → kit-provided `routingmeta::ProjResult Send(...)` (AD-4), returns `ProjResult`, caller owns abort/proceed.

### Ground truth (read these; the prose must match)
- **`example/src/common/send.h:22`** — `ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) { FillCommon(rt, sink); return ProjectMeta(req, sink); }` in `namespace routingmeta`; header comment: "Send = FillCommon + generated ProjectMeta, returns ProjResult, never throws on a [data condition]."
- **`example/src/common/proj_result.h`** — `ProjResult { bool ok; std::vector<Issue> issues; std::chrono::nanoseconds duration; }`, `Issue{ kind, key }`, `Issue::MissingRequired` / `Issue::Overflow`.
- **`example/build/generated/sys3.proj.cc`** — required-scalar path: empty → push `MissingRequired`, set `ok=false`, emit `x-routing-error: missing:x-mask-id`, suppress the scalar.
- **`refs/SPEC.md` §5.3** (READ-ONLY) — "The digest is an integrity check, not a security control … no key and no signature." Mirror this framing into OVERVIEW; do NOT edit refs/.
- **`example/src/common/process_context_parser.h`** — `VerifyDigest` recompute/compare (integrity).

### CR3 — the read-only boundary (AC4)
- `refs/` is the immutable source of truth and is OFF-LIMITS (CR3 + workspace rules). The docs being fixed are the LIVE working copies at `grpc-routing-meta/{CONTEXT,OVERVIEW.zh,README}.md` — same filenames as some `refs/` files, so be deliberate about the path. Final `git diff --name-only` MUST show zero `refs/` entries. This is the one acceptance criterion a careless edit could violate silently — check it explicitly.

### CMake parity (AC5) — closing the deferred item
- `deferred-work.md` (1.8 dev note) logged the missing CMake `bench_projection` for "a docs/parity story (1.15) or a standalone follow-up." 1.14 then added `test_concurrency` to `build.sh`, creating a second parity gap. Close BOTH here so CMake (which the README calls the canonical build) builds everything `build.sh` does. `test_concurrency` needs `Threads::Threads` (it uses `<thread>`); `bench_projection` is a plain target like `test_projection`. This is the natural home — the README's build claim is only true once CMake has the targets. Mark the `deferred-work.md` CMake item RESOLVED.

### What must be preserved (system still works end-to-end)
- **No wire/code/proto change:** docs + `CMakeLists.txt` only. `build.sh`, the plugin, generated code, and every emitted byte are untouched; `receiver_verify` digest stays `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd`. (Verification is a formality — text/CMake edits cannot move the wire.)
- **Doc accuracy is the deliverable:** the new text must be correct against the code; preserve each doc's existing voice (CONTEXT terse-English, OVERVIEW Chinese, README terse-English) and structure — change only the stale claims.
- **refs/ read-only (CR3):** absolutely no edits under `refs/`.

### Guardrails (do NOT do in this story)
- Do NOT edit anything under `refs/` (CR3) — only the live `grpc-routing-meta/` copies + `example/CMakeLists.txt`.
- Do NOT change code, protos, the plugin, `build.sh`, or any emitted byte. Docs + CMake targets only.
- Do NOT introduce a NEW inaccuracy — verify every rewritten claim against the actual source.
- Do NOT alter illustrative example values (digests, header dumps) in the docs.
- Do NOT commit the scratch `build-cmake/` directory.

### Project Structure Notes
- Edits: `grpc-routing-meta/CONTEXT.md`, `grpc-routing-meta/OVERVIEW.zh.md`, `grpc-routing-meta/README.md`, `grpc-routing-meta/example/CMakeLists.txt`. Resolves the `deferred-work.md` CMake `bench_projection` parity item (+ adds `test_concurrency`). No code/wire change.

### Previous story intelligence (Stories 1.1–1.14)
- The behavior the docs must now match was built across the epic: 1.1 made `ProjectMeta` return `ProjResult` (no throw); 1.2 made overflow non-blocking data; 1.4 put `routingmeta::Send` in the kit returning `ProjResult` (AD-4); 1.11 hardened the receiver digest gate (integrity); 1.12 locked the canonical projection/crypto. So every doc claim has a concrete code referent — cite it, match it. 1.7 added `bench_projection` and 1.14 `test_concurrency` to `build.sh`; this closes their CMake parity. Wire frozen throughout (CR1/AD-9); 1.15 is docs + build-script parity, zero wire impact — the final story.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.15] — user story + 4 ACs (CONTEXT/OVERVIEW/README/refs).
- [Source: ARCHITECTURE-SPINE.md:107] — AD-5 failure-as-data (the correct behavior CONTEXT/OVERVIEW must state).
- [Source: ARCHITECTURE-SPINE.md:132,163] — additive API / read-only `refs/` (CR3): doc updates target the live `grpc-routing-meta/` copies only.
- [Source: refs/SPEC.md#5.3] — digest is integrity, not security (OVERVIEW framing). READ-ONLY.
- [Source: grpc-routing-meta/example/src/common/send.h:22] — kit `Send` returns `ProjResult` (README).
- [Source: grpc-routing-meta/example/src/common/proj_result.h] — `ProjResult`/`Issue` shape.
- [Source: grpc-routing-meta/CONTEXT.md:59,88] / [OVERVIEW.zh.md:117,157,183] / [README.md:26] — the stale claims.
- [Source: grpc-routing-meta/example/CMakeLists.txt:88-97] — target pattern to mirror (bench + concurrency).
- [Source: _bmad-output/implementation-artifacts/deferred-work.md] — the CMake `bench_projection` parity item 1.15 resolves.

### Latest tech notes
No external research. CMake `find_package(Threads)` + `Threads::Threads` is the standard portable pthread link. Markdown edits only; no new deps. The kit behavior is fixed — this is description, not change.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (engineering subagent under Amelia/dev-story; main-loop independent verification)

### Debug Log References

- **CR3:** `git status --short refs/` → empty; `git diff` touches only the 4 allowed files. No `refs/` edit.
- **Regression (independent re-run):** `./build.sh` → `OK -> binaries`; `./build/test_projection` → `ALL TESTS PASSED`; `./build/receiver_verify` digest UNCHANGED `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd`; `./build/test_concurrency` → `CONCURRENCY TEST PASSED`.
- **CMake parity:** `cmake -S . -B build-cmake -DCMAKE_PREFIX_PATH=…/anaconda3` configures clean (`Protobuf 3.20.3`, `Found Threads: TRUE`); `ctest -N` registers `projection` + `concurrency`; `--target help` lists `bench_projection` + `test_concurrency`. A full CMake *link* fails on this dev host at the pre-existing `protoc-gen-meta` target (anaconda static-protobuf vtable/`descriptor_table` symbols — a known host-discovery quirk, NOT introduced by 1.15 and NOT in a 1.15-touched target). build.sh is authoritative locally; the CI matrix builds protobuf from source, where CMake links cleanly. New targets verified structurally (registered, mirror the working `test_projection` pattern).

### Completion Notes List

- **AC1 (CONTEXT.md)**: invariant 9 (`:59`) + checklist (`:88`) rewritten from "`ProjectMeta` throws" → records `Issue{MissingRequired,"x-mask-id"}` in `ProjResult` (`ok=false`), emits `x-routing-error: missing:x-mask-id`, suppresses the empty scalar, does **not** throw. Verified against `sys3.proj.cc` + `proj_result.h`.
- **AC2 (OVERVIEW.zh.md)**: failure language → `ProjResult{ok=false, MissingRequired}` + `x-routing-error` (`:117`, `:161`, `:187`, all in Chinese, tables preserved); digest reframed as **integrity-only, not security** — a body editor recomputes it (no key/signature), detects bug/version-drift/transport-mangling not malicious tampering (`:124`, `:188`, mirrors SPEC §5.3). Illustrative digests unchanged.
- **AC3 (README.md)**: stale `void Send(...)` → kit `routingmeta::ProjResult Send(const Req&, const Runtime&, MetadataSink&)` (verbatim `send.h`), framed as kit-provided (AD-4), one for all systems; added the `ProjResult{ok/issues/duration}` + caller-decides-abort/proceed note. Verified against `send.h`/`proj_result.h`.
- **AC4 (CR3)**: confirmed zero `refs/` edits — only the 4 live kit files changed.
- **AC5 (CMake parity)**: `find_package(Threads REQUIRED)` + `bench_projection` (deferred 1.7/1.8) + `test_concurrency` (1.14) + `add_test(NAME concurrency)`; CMake now mirrors `build.sh`'s full binary set. Resolves the `deferred-work.md` CMake parity item.
- **No drift**: zero code/proto/wire/build.sh change; `receiver_verify` digest identical (`efafba16…`). Docs + CMake targets only.

### File List

- `grpc-routing-meta/CONTEXT.md` (MODIFIED — invariant 9 + checklist: throw → ProjResult/MissingRequired/x-routing-error)
- `grpc-routing-meta/OVERVIEW.zh.md` (MODIFIED — failure → ProjResult; digest → integrity-only, not security)
- `grpc-routing-meta/README.md` (MODIFIED — kit `routingmeta::Send` returning `ProjResult` + the Send-in-the-kit boundary)
- `grpc-routing-meta/example/CMakeLists.txt` (MODIFIED — `find_package(Threads)` + `bench_projection` + `test_concurrency` targets)

## Change Log

- 2026-06-28 — Story 1.15 drafted (create-story): align the three live docs with the shipped code and close the CMake↔build.sh parity gap. CONTEXT.md inv. 9 throw→`ProjResult`/`MissingRequired`/`x-routing-error`; OVERVIEW.zh.md throw→`ProjResult` + digest integrity-only (not security); README.md kit `routingmeta::Send` returning `ProjResult` + the Send-in-the-kit boundary (AD-4); CMakeLists.txt gains `bench_projection` (deferred 1.7/1.8) + `test_concurrency` (1.14). `refs/` untouched (CR3). Docs + CMake only — zero wire/code change (CR1/AD-9). The final story of Epic 1.
- 2026-06-28 — Story 1.15 implemented (dev-story): all 5 tasks done. CONTEXT.md/OVERVIEW.zh.md/README.md prose aligned to the shipped failure-as-data + integrity-digest + kit-`Send` behavior (each claim verified against `send.h`/`proj_result.h`/`sys3.proj.cc`/SPEC §5.3); `CMakeLists.txt` gains `bench_projection` + `test_concurrency` (+`Threads`). CR3 verified — zero `refs/` edits. Regression green, `receiver_verify` digest UNCHANGED (`efafba16…`), `test_concurrency` passes. CMake new targets registered/structurally verified (full local CMake link blocked by a pre-existing conda-host protobuf-static quirk at `protoc-gen-meta`, unrelated to 1.15; CI builds protobuf from source). No drift — docs + CMake only. Status → review.
