---
baseline_commit: 18ccfc3
---

# Story 1.14: Thread-safe, re-entrant projection

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a sender developer,
I want `ProjectMeta` to be safe to call concurrently,
so that a multithreaded sender is correct without locks.

## Acceptance Criteria

1. **AC1 — Re-entrant by construction, documented as an invariant (AD-12, NFR5).** `ProjectMeta` reads the request and writes only a per-call sink, with no shared mutable state; this re-entrancy is **documented as an invariant** in the kit.

2. **AC2 — Concurrent test proves no data race (HR3).** With N threads projecting concurrently, the results are **identical** to the single-threaded projection and there is **no data race** — the concurrent test is clean across repeated runs AND under a thread sanitizer.

## Tasks / Subtasks

> `ProjectMeta` is ALREADY re-entrant (verified): its body uses only locals (`result`, `ctxs`, `s`), reads `req` by const-ref, and writes only the caller-owned `sink`; `MetadataSink::bytes_` / `VectorSink::items` are per-instance; every `static` on the path is `static const` (immutable lookup tables — sha256 `K`, hex tables, the constant key string). This story DOCUMENTS that invariant and PROVES it with a concurrent test under ThreadSanitizer. No projection/wire logic change — the plugin edit is COMMENT-ONLY (emitted preamble); the rest is a new test + build/CI wiring.

- [x] **Task 1 — Document the re-entrancy invariant (AC: 1, AD-12)** — edit `src/plugin/protoc-gen-meta.cc` (emitted comment only)
  - [x] Extend the ProjectMeta preamble the plugin emits (`protoc-gen-meta.cc:197-198` for the `.cc`, and the `.h` decl preamble ~`:176`/`:187`) to state the AD-12/NFR5 invariant, e.g.: *"Re-entrant (AD-12/NFR5): reads `req`, writes ONLY the per-call `sink`; holds no shared mutable state, so concurrent calls on distinct sinks need no locks."* This makes EVERY generated `ProjectMeta` self-document the invariant at its definition site.
  - [x] This is a comment inside the emitted (generated) source — generated `*.proj.{h,cc}` live under `build/` and are **gitignored**, so only the committed plugin source changes; emitted metadata/bytes and generated LOGIC are unchanged. (If a more discoverable committed surface is wanted, also add one line to `src/common/metadata_sink.h`'s top contract comment — optional; the plugin preamble is the authoritative spot.)

- [x] **Task 2 — Concurrent projection test (AC: 2, HR3)** — NEW `tests/test_concurrency.cc`
  - [x] Build a single-threaded BASELINE: project a representative request (a sys1 multi-context request via the existing pattern, e.g. 4–8 contexts with `recipe_id="R/A"` etc.) into one `VectorSink`; snapshot its `items` (full key/value/order sequence, incl. digest) as the expected result.
  - [x] Spawn N threads (e.g. `N=8`), each running M iterations (e.g. `M=2000`): every iteration projects the SAME shared `const` request into a THREAD-LOCAL `VectorSink` and compares its `items` to the baseline; on any mismatch, set an `std::atomic<bool> mismatch` (and record once). All threads read the one shared `const CalculateRequest` concurrently (exercises concurrent immutable reads) and write only their own per-thread sink (no shared writes).
  - [x] Join all threads; `assert(!mismatch)`; print `CONCURRENCY TEST PASSED`. Plain `assert`, `<thread>`/`<atomic>`, no test framework, no new deps. Keep it deterministic (equality to baseline) so the functional run is a hard check independent of the sanitizer.
  - [x] (Optional, strengthens HR3) include one sys3 scalar request projected concurrently too, to cover the scalar path — only if it stays simple.

- [x] **Task 3 — Wire into build.sh + a CI ThreadSanitizer gate (AC: 2)** — edit `build.sh` and `.github/workflows/ci.yml`
  - [x] `build.sh`: compile + run `tests/test_concurrency.cc` with `-pthread` (functional check on every build), mirroring the `test_projection` step (`build.sh:104-105`); add a `[test] test_concurrency` line; link the generated set the same way. It must print `CONCURRENCY TEST PASSED`.
  - [x] `.github/workflows/ci.yml`: add a step (after the existing self-checks) that compiles `tests/test_concurrency.cc` with `-fsanitize=thread -pthread -O1 -g` (+ the generated sources / includes the test needs) and RUNS it — the HR3 "clean under sanitizer" gate. Mirror the existing self-check / gRPC-smoke step structure; use the cell's `$CXX`/`$PROTOC` and the same `-I` include flags as `build.sh`. A TSan-reported race MUST fail the job (TSan exits non-zero by default; optionally set `TSAN_OPTIONS=halt_on_error=1`).
  - [x] Keep the TSan gate minimal and self-contained (one representative cell is sufficient if matrix-wide is awkward — document the choice, as 1.9 did for the gRPC smoke).

- [x] **Task 4 — Build & verify (AC: 1, 2)**
  - [x] `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; `[neg ]` gate unchanged; `[test] test_concurrency` prints `CONCURRENCY TEST PASSED`.
  - [x] Local ThreadSanitizer proof (macOS clang supports it): compile `tests/test_concurrency.cc` with `-fsanitize=thread -pthread` against the generated sources and RUN it → exits 0, **no TSan race report** (records the HR3 evidence the no-remote-run constraint otherwise defers; CI pins it for Linux).
  - [x] Regression: `./build/test_projection` → `ALL TESTS PASSED`; `./build/receiver_verify` → digest UNCHANGED `sha256:efafba16…`; `./build/bench_projection` → BENCH PASSED; `./build/unified_sender` runs. Confirm NO emitted byte changed (plugin edit was comment-only).

## Dev Notes

### Method (Amelia)
Re-entrancy is already TRUE — this story makes it (a) explicit as a documented invariant and (b) proven by a concurrent test that is clean both functionally (N×M projections all byte-equal to the single-thread baseline) and under ThreadSanitizer (no data race). The test is the deliverable AD-12 names ("one concurrent test (HR3)"). The "no shared mutable state" claim is verified below; the test guards against a future regression that introduces shared state (e.g. a `static` accumulator).

### Why ProjectMeta is re-entrant (verified, AC1 evidence)
- **Generated `ProjectMeta` (`build/generated/sys1.proj.cc`):** declares only locals (`ProjResult result`, `_proj_t0`, `std::vector<std::string> ctxs`, `std::string s`); reads `req` by const-ref; writes only the passed-in `routingmeta::MetadataSink& sink`. Calls `UrlEncode` (pure), `EmitProcessContexts(sink, ctxs)` (writes the per-call sink), `std::chrono::steady_clock::now()` (thread-safe). No `static`/global mutable.
- **`MetadataSink` (`src/common/metadata_sink.h`):** `bytes_` is a per-instance member; `Add` mutates only `*this`. `VectorSink::items` is per-instance. The sink is constructed by the caller per call → no sharing unless the caller shares one (the test gives each thread its own).
- **Helpers:** `UrlEncode` (`url_encode.h`), `Sha256Hex` (`sha256.h`), `EmitProcessContexts` (`process_context_emit.h`) — all read inputs + write locals/sink; every `static` in them is `static const` (immutable). No global mutable state anywhere on the projection path.
- Therefore concurrent `ProjectMeta` calls on DISTINCT sinks share only immutable data (the const request + the static-const tables) → no data race, no locks needed (AD-12/NFR5).

### Documentation surface (AC1 "documented as an invariant")
- Primary: the plugin's emitted ProjectMeta preamble (`protoc-gen-meta.cc`), so every generated entrypoint self-documents the invariant at its definition. Generated files are gitignored; only the committed plugin source carries the comment in git. No emitted-byte/behavior change (comment in generated source, compiled away).
- The kit has no `.md` doc file under `example/`; Story 1.15 reconciles the root `*.md` docs. Keeping the invariant in the plugin preamble (and optionally `metadata_sink.h`) feeds 1.15.

### Concurrent test design (Task 2, HR3)
- Equality-to-baseline is the functional oracle: identical inputs MUST yield byte-identical `items` on every thread/iteration; any divergence (torn write, shared accumulator) flips the atomic. ThreadSanitizer is the race oracle: it catches unsynchronized shared access even when the output happens to look right. Both are needed — a data race can produce correct-looking output yet still be UB.
- N=8 × M=2000 gives ample interleaving; tune if runtime is a concern (the bench shows per-call ≈ sub-ms, so 16k projections is fast). Threads read ONE shared `const` request (concurrent immutable reads are safe) and write per-thread sinks (no shared writes) — exactly the AD-12 usage contract.

### ThreadSanitizer (Task 3/4, the HR3 "under sanitizer" gate)
- Local: macOS clang supports `-fsanitize=thread`; run the TSan build once to record clean (the dev-host proof, analogous to 1.9's local gRPC smoke). CI pins it on Linux.
- CI: add a TSan compile+run step to `.github/workflows/ci.yml` mirroring the self-check / gRPC-smoke steps (use the cell's `$CXX`, the same `-I` flags + generated sources as `build.sh`). TSan exits non-zero on a race → fails the job. One representative cell is acceptable if matrix-wide is awkward (document it, per 1.9's precedent).
- Do NOT add `-fsanitize=thread` to `build.sh`'s default flags (it would slow every build and isn't portable to all dev toolchains) — keep the default `build.sh` run functional (`-pthread` only) and the sanitizer in CI + an on-demand local run.

### What must be preserved (system still works end-to-end)
- **No wire/projection byte changes (CR1/AD-9):** the plugin edit is the emitted COMMENT only — generated logic, emitted metadata, headers, and the digest are byte-identical. `receiver_verify`'s digest stays `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd`; `test_projection` still `ALL TESTS PASSED`.
- **`assert`-based harness, no new deps:** `<thread>`/`<atomic>` are stdlib; `-pthread` is the only new link flag. No test framework.
- **The negative gate + all prior locks stay green:** Task 3 only ADDS a test + a CI step; it must not alter the `[neg ]` gate or existing self-checks.

### Guardrails (do NOT do in this story)
- Do NOT change `ProjectMeta`'s logic, `EmitProcessContexts`, the sink's behavior, `UrlEncode`/`Sha256Hex`, or any emitted byte. Re-entrancy is already correct — document + test only.
- Do NOT add `static`/global mutable state to "optimize" (it would BREAK the invariant the test now guards).
- Do NOT make `build.sh` default to a sanitizer build (portability/perf) — TSan goes in CI + on-demand local.
- Do NOT introduce a new dependency or test framework — extend with one plain-`assert` `.cc`.

### Project Structure Notes
- New: `tests/test_concurrency.cc`. Edits: `src/plugin/protoc-gen-meta.cc` (emitted comment only), `build.sh` (compile+run the new test), `.github/workflows/ci.yml` (TSan gate). No proto/sender/receiver/wire change.

### Previous story intelligence (Stories 1.6–1.13)
- 1.6 made `ProjectMeta` self-time (the `_proj_t0`/`duration` locals — already per-call); 1.7 benched it sub-ms (so 16k projections in the concurrent test are cheap). 1.9 set up the CI matrix and the pattern of adding a compile/run gate (gRPC smoke) — the TSan gate mirrors it (incl. the "representative cell is acceptable, document it" fallback). 1.12 locked the projected bytes (so the concurrent baseline-equality is well-defined); 1.13 added receiver-side robustness. 1.14 closes the sender-side concurrency invariant (AD-12/NFR5/HR3). Wire frozen throughout (CR1/AD-9) — 1.14 adds a test + doc + CI, no wire impact.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.14] — user story + 2 ACs.
- [Source: ARCHITECTURE-SPINE.md:139-142] — AD-12: pure, re-entrant projection; "documented as an invariant + one concurrent test (HR3)".
- [Source: ARCHITECTURE-SPINE.md:164] — cross-cutting: projection pure/re-entrant (AD-12).
- [Source: grpc-routing-meta/example/build/generated/sys1.proj.cc] — generated `ProjectMeta` body (locals + per-call sink; re-entrancy evidence).
- [Source: grpc-routing-meta/example/src/common/metadata_sink.h] — `MetadataSink`/`VectorSink` per-instance state (no global).
- [Source: grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:176,187,197-212] — emitted ProjectMeta preamble (doc surface for Task 1).
- [Source: grpc-routing-meta/example/build.sh:104-107] — test build wiring (Task 3 mirror).
- [Source: grpc-routing-meta/example/.github/workflows/ci.yml:43-59] — self-check / gRPC-smoke steps (TSan gate mirror).

### Latest tech notes
No external research. `std::thread`/`std::atomic` (C++17 stdlib), `-pthread`, and clang/gcc `-fsanitize=thread` (ThreadSanitizer) are standard. macOS clang TSan verified available on the dev host. No new deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (engineering subagent under Amelia/dev-story — interrupted mid-run after edits; main loop completed the CI gate verification + bookkeeping and independently re-ran build/tests/TSan)

### Debug Log References

- Build: `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh` → links; `[neg ]` gate unchanged; `[test] test_concurrency` built; `OK -> binaries`.
- `./build/test_concurrency` → `CONCURRENCY TEST PASSED` (functional equality oracle, 8×2000 projections).
- **Local ThreadSanitizer proof (HR3):** `c++ -std=c++17 -fsanitize=thread -pthread -O1 -g tests/test_concurrency.cc build/generated/*.pb.cc build/generated/*.proj.cc -I build/generated -I src -I <pb-inc> -L <pb-lib> -lprotobuf …` → `/tmp/test_concurrency_tsan` → `CONCURRENCY TEST PASSED`, **exit 0, no race report**.
- Regression: `./build/test_projection` → `ALL TESTS PASSED`; `./build/receiver_verify` → digest UNCHANGED `sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd`; `./build/bench_projection` → `BENCH PASSED`; `./build/unified_sender` → exit 0.

### Completion Notes List

- **AC1 (re-entrant, documented)**: verified no shared mutable state on the projection path (generated `ProjectMeta` uses only locals + the per-call sink; `MetadataSink::bytes_`/`VectorSink::items` per-instance; every `static` is `static const`). Documented the AD-12/NFR5 invariant in the plugin's emitted ProjectMeta preamble — BOTH the emitted `.h` decl and `.cc` definition (`protoc-gen-meta.cc`) — so every generated entrypoint self-documents it. Comment-only: generated `*.proj.{h,cc}` are gitignored; only committed plugin source changed; zero emitted bytes (digest unchanged proves it).
- **AC2 (no data race, HR3)**: new `tests/test_concurrency.cc` — single-thread baseline, then N=8 × M=2000 threads each project the one shared `const` request into a thread-local `VectorSink` and compare `items` byte-for-byte to the baseline; `std::atomic<bool> mismatch`; `assert(!mismatch)`. Two oracles: functional equality (every build) + ThreadSanitizer (CI gate + the local run above). Local TSan clean → the dev-host HR3 proof the no-remote-run constraint otherwise defers.
- **Wiring**: `build.sh` builds `test_concurrency` with `-pthread` (matches the build-only convention for the other tests; the main loop / CI self-checks run it). `.github/workflows/ci.yml`: added `./build/test_concurrency` to the matrix-wide self-checks, plus a single-cell (`clang++`/`3.21.12`) `-fsanitize=thread` compile+run gate with `TSAN_OPTIONS=halt_on_error=1` (representative-cell pattern per 1.9's gRPC smoke; functional run is still matrix-wide).
- **Process note**: the dev-story subagent was interrupted by the user after completing all four file edits but before Task-4 verification; the main loop verified the CI gate edit, ran the full build + functional + local-TSan + regression suite, and confirmed no drift.
- **No drift**: `ProjectMeta` already re-entrant; TSan clean; only a plugin comment + a new test + build/CI wiring; zero emitted bytes changed (digest identical).

### File List

- `grpc-routing-meta/example/tests/test_concurrency.cc` (NEW — N×M concurrent-projection equality test, `<thread>`/`<atomic>`, `-pthread`)
- `grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc` (MODIFIED — emitted ProjectMeta preamble comment for the AD-12 invariant, `.h` + `.cc`; no codegen logic change)
- `grpc-routing-meta/example/build.sh` (MODIFIED — build `test_concurrency` with `-pthread`)
- `.github/workflows/ci.yml` (MODIFIED — `test_concurrency` in self-checks + a single-cell ThreadSanitizer race gate)

## Change Log

- 2026-06-28 — Story 1.14 drafted (create-story): document + prove `ProjectMeta` re-entrancy (AD-12/NFR5/HR3). Verified no shared mutable state (locals + per-call sink; every `static` is `static const`). Adds a plugin emitted-preamble invariant comment, a new `tests/test_concurrency.cc` (N×M projections byte-equal to the single-thread baseline, atomic mismatch flag), a `build.sh` functional run (`-pthread`), and a CI ThreadSanitizer gate; local macOS TSan run records the dev-host proof. No wire/projection change — comment + test + CI only (CR1/AD-9 preserved).
- 2026-06-28 — Story 1.14 implemented (dev-story): all 4 tasks done. Plugin emitted-preamble invariant comment (`.h`+`.cc`, comment-only), new `tests/test_concurrency.cc` (8×2000), `build.sh` `-pthread` build, CI self-check + single-cell TSan gate. Build links, `[test] test_concurrency` → `CONCURRENCY TEST PASSED`; local ThreadSanitizer run clean (exit 0, no race); `test_projection` `ALL TESTS PASSED`; `receiver_verify` digest UNCHANGED (`efafba16…`); bench PASSED. (Dev-story subagent interrupted after edits; main loop completed verification.) No drift — zero emitted bytes changed. Status → review.
