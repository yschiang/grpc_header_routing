---
baseline_commit: dd5bd706de36f9312cd89528ca13a8a755e2d950
---

# Story 1.7: `bench_projection` proves sub-millisecond

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a build/release engineer,
I want a bench that proves projection is sub-ms across realistic sizes,
so that the "sub-ms / perf observed" claim (criterion H) is evidence, not assertion.

## Acceptance Criteria

1. **AC1 — Bench prints per-call projection time for 1/2/25/60 contexts.** Given a new `tests/bench_projection`, when built and run, then it prints per-call projection time for 1, 2, 25, and 60 contexts. (FR7, BRIEF H)

2. **AC2 — Each measured time is sub-millisecond; non-zero exit on breach.** Given each measured time, when the bench asserts, then each is sub-millisecond (< 1 ms) and the binary exits non-zero if any exceeds 1 ms.

3. **AC3 — `build.sh` builds and links `bench_projection`.** Given `build.sh`, when run, then `bench_projection` builds and links alongside the other binaries.

## Tasks / Subtasks

- [x] **Task 1 — Write `tests/bench_projection.cc` (AC: 1, 2)** — NEW file `example/tests/bench_projection.cc`
  - [x] Local request builder: copy the minimal `sys1Req(int n)` shape from `tests/test_projection.cc:23` (tool_id + n contexts, each with the 7 fields). Do NOT extract a shared header — a ~12-line builder duplicated across two test TUs is fine (YAGNI on the abstraction).
  - [x] For each `N` in `{1, 2, 25, 60}`: build the request ONCE, then time `ProjectMeta(req, sink)` across `kIters` iterations under ONE `steady_clock` interval (start clock → loop K calls → stop clock), and report `per_call = total / kIters`. A **fresh `VectorSink` per iteration** (the sink accumulates headers; reusing one would grow it and flip the byte-overflow path after the first call).
  - [x] **Resolution-robust by construction** (resolves the Story 1.6 deferred item): one clock interval spanning K calls means total elapsed ≫ clock granularity, so `per_call` is meaningful even where a single call would round to 0 on a coarse `steady_clock`. Use `kIters` large enough that total ≫ granularity yet the bench stays instant (e.g. `constexpr int kIters = 2000;`).
  - [x] Prevent `-O2` from eliding the loop: accumulate an observable from each call (e.g. `sink_guard += sink.items.size();` or `+= r.duration.count();`) and print/consume it after the loop so the work is observably used.
  - [x] Print one line per size, e.g. `projection: N=%2d contexts -> %8.3f us/call (%lld ns, %d iters)`. Optionally note the path (N=60 trips the count cap → overflow branch).
  - [x] Sub-ms gate: `constexpr long long kBudgetNs = 1'000'000;` (1 ms). For each size, if `per_call_ns >= kBudgetNs`, print a FAIL line to `stderr` and set a `failed` flag. End: print `BENCH PASSED` / `BENCH FAILED` and `return failed ? 1 : 0;` — explicit non-zero exit (do NOT rely on `assert`, which `-DNDEBUG` would strip; AC2 requires the exit code).

- [x] **Task 2 — Link `bench_projection` in `build.sh` (AC: 3)** — edit `example/build.sh`
  - [x] After the `[test] test_projection` block (`build.sh` step 4), add a `[bench] bench_projection` block mirroring it: `$CXX $CXXFLAGS tests/bench_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/bench_projection"`. Same `GEN_SRCS` (each `.pb.cc` linked exactly once) and `PBFLAGS`.
  - [x] Keep it OUT of the CI-gating negative/round-trip path — it is a perf bench, linked alongside the others, run on demand. (build.sh's final `echo OK` line is unchanged.)

- [x] **Task 3 — Build & verify (AC: 1, 2, 3)**
  - [x] `cd grpc-routing-meta/example && ./build.sh` → `bench_projection` builds and links alongside `unified_sender` / `receiver_verify` / `test_projection` (AC3).
  - [x] `./build/bench_projection` → prints 4 per-call lines (N=1, 2, 25, 60), all sub-ms, ends `BENCH PASSED`, exit 0 (AC1, AC2). `echo $?` → 0.
  - [x] Sanity-check the non-zero-exit contract: it is wired via the explicit `per_call_ns >= kBudgetNs` check + `return 1` (not `assert`). Confirm `./build/test_projection` still → `ALL TESTS PASSED` (no regression; bench is additive).

### Review Findings

_Code review 2026-06-28 (Blind Hunter [diff-only] + Edge Case Hunter [diff+repo] + Acceptance Auditor [diff+spec+architecture], no shared context). 1 patch (comment-only), 0 decision-needed, 0 deferred, 4 dismissed. **Acceptance Auditor: PASS** — 3/3 ACs, no architecture violation, no scope creep; independently verified red (`kBudgetNs=1` → all 4 lines, `BENCH FAILED`, exit 1 via explicit `return`, not `assert`) and green (exit 0). **Edge Case Hunter: PASS** — reconstructed `guard=88000` by hand to verify every path (N=25 full digest = 28 items, N=60 overflow short-circuit = 3 items, non-blocking so `r.ok` stays true). **Blind Hunter: CONCERNS** — one Med (mean-not-max), addressed by the patch below._

- [x] [Review][Patch] Header comment over-claimed "proves each call < 1 ms" — the gate checks the MEAN per-call (`total/kIters`), not the per-call max [grpc-routing-meta/example/tests/bench_projection.cc:2-9]. Reworded to state it proves the **mean** per-call sub-ms as criterion-H evidence ("perf observed"), NOT a per-call tail-latency SLA, with a `ponytail:` note: a per-call max/p99 gate would trip on OS scheduling noise (page faults), not projection cost → **flaky** — the mean-via-single-interval is the deliberate resolution-robust design 1.6's review mandated here. Same patch labels N=60 as the overflow short-circuit case (Edge Hunter L2). Comment-only — rebuilt, still green (`guard=88000`, exit 0).

_Dismissed (4):_
- _Blind L2 / Edge L1 "timed region includes `VectorSink` ctor/dtor" — real but conservative: both reviewers agree it only inflates the number (false-FAIL risk, never false-PASS), and a fresh sink per call is realistic (every real call owns its sink). The ~20× headroom absorbs it._
- _Blind L3 "no warm-up iteration" — first-iter cold-cache cost is amortized across 2000 iters and biases only toward FAIL; the 20× headroom makes it immaterial._
- _Edge L3 / Blind flakiness "1 ms vs ~3–49 µs" — ~20–26× headroom over a 7–75 ms total interval (≫ `steady_clock` granularity on the Linux/macOS matrix); only a pathologically oversubscribed runner sustaining 20×+ slowdown could false-fail. Gating on the mean (not max) keeps it non-flaky._
- _Blind L4 "single uniform input shape per N" — YAGNI for an evidence bench: projection cost scales with context COUNT (varied 1/2/25/60, the criterion-H sizes); field-value-length variety is exercised by the byte/line-overflow tests in `test_projection.cc`. Not a worst-case latency proof, nor meant to be._

## Dev Notes

### Method (Amelia)
Red → green: write the bench with the sub-ms gate and a deliberately-too-tight budget first (e.g. set `kBudgetNs` to a tiny value), confirm it prints times and exits NON-zero (`BENCH FAILED`) — proving the gate actually gates — then restore `kBudgetNs = 1'000'000` and confirm `BENCH PASSED`, exit 0. This is the bench-equivalent of red-green: prove the failure path before trusting the pass.

### What the bench measures (and why 25 vs 60 differ)
- The generated sys1 `ProjectMeta` builds a `std::vector<std::string>` of ALL N contexts (each url-encoded + key-sorted — the field-specific, generated part), then hands them to `EmitProcessContexts` (`src/common/process_context_emit.h:39`).
- **N=1, 2, 25** → within the caps (`kMaxContexts=25`, `kMaxLineBytes=512`, `kMaxTotalMetaBytes=7168`): the FULL path runs — N encodes + one sha256 over the canonical join + N emitted headers. **N=25 is the heaviest in-budget case** (25 contexts ≈ 89 B each → ~3.6 KB projected, well under 7 KB, so it does NOT overflow).
- **N=60** → `count > kMaxContexts` trips overflow: the 60 strings are still built/encoded, but `EmitProcessContexts` returns early (no digest, no per-context emit). So N=60 measures "60 encodes minus the digest/emit". This is the intended point of the 60-context case: prove that even an over-cap request returns fast. Both shapes are asserted sub-ms.
- The bench measures wall-clock around the `ProjectMeta` call (includes the per-iteration `VectorSink` ctor/dtor — realistic, since every real call owns its sink). `ProjectMeta` also self-times into `result.duration` (Story 1.6); the bench's own averaged measurement is the criterion-H evidence. They will agree to within the sink-alloc + clock-read overhead.

### Resolution-robust timing — this story closes the 1.6 deferral
- Story 1.6's review deferred "resolution-robust timing assertions" here: its `duration.count() > 0` is fragile on a coarse `steady_clock`. The bench's design fixes this structurally — **one `steady_clock` interval around K calls**, then divide. Total elapsed (K × per-call) dwarfs any clock granularity, so the per-call figure is sound even on a coarse clock. No single-call `> 0` assertion is used here. Mark the deferred item resolved in `deferred-work.md`.
- Pick `kIters` (e.g. 2000) so total ≫ granularity. At N=60 (~few µs/call) × 2000 ≈ a few ms total — instant, and ≫ the ~10s-of-ns macOS / ns Linux granularity.

### Current state of the files this story touches (read before editing)
- **`example/tests/test_projection.cc`** — the model: `sys1Req(int n, const char* recipe, const char* pad)` at `:23` builds a `sys1::v1::CalculateRequest` with n contexts; `auto r = ProjectMeta(req, sink);` at `:52`. Copy the minimal builder; the bench does not need recipe/pad. Includes to mirror: `common/metadata_sink.h`, `sys1.proj.h`, plus `<chrono>`, `<cstdio>`, `<cstddef>`.
- **`example/src/common/process_context_emit.h`** — caps live here: `kMaxTotalMetaBytes=7168` (`:29`), `kMaxContexts=25` (`:30`), `kMaxLineBytes=512` (`:31`); overflow when `ctxs.size() > kMaxContexts || maxline > kMaxLineBytes || sink.bytes()+projected > kMaxTotalMetaBytes` (`:56`). Read-only here — the bench does not change policy.
- **`example/build.sh`** — step 4 links the apps + test; `GEN_SRCS` (`metadata_options.pb.cc`, `process_context.pb.cc`, and per-system `*.pb.cc` + `*.proj.cc`) and `PBFLAGS` are already assembled. The bench links the SAME set + sys1's generated `ProjectMeta`. Add one `[bench]` line after `[test] test_projection`.
- **`example/src/common/proj_result.h`** — `ProjResult` has `bool ok`, `vector<Issue> issues`, `chrono::nanoseconds duration`. The bench reads none of these for its pass/fail except optionally `duration` as the loop guard.

### What must be preserved (system still works end-to-end)
- **Wire frozen (CR1/AD-9):** the bench is read-only w.r.t. the projection — it adds NO header, changes NO policy/constant, and does not touch the plugin or generated code. `unified_sender` / `receiver_verify` byte output unchanged.
- **Caps single-sourced (AD-7/NFR6):** do NOT hardcode 25/512/7168 in the bench. If a size constant is ever needed, include `process_context_emit.h`. The bench's own knobs (`kIters`, `kBudgetNs`) are bench-local and unrelated to projection policy.
- **No new deps (NFR3/AD-11):** `<chrono>` + `<cstdio>` are stdlib. No benchmark framework (no Google Benchmark) — a plain timed loop is the lazy, dependency-free, CI-portable choice.
- **One timing point unaffected (AD-6):** the bench measures `ProjectMeta` externally; it does NOT add timing inside the kit. `ProjectMeta`'s own self-timing (1.6) is untouched.

### Guardrails (do NOT do in this story)
- Do NOT add a benchmark framework or any new dependency — plain `std::chrono` loop only.
- Do NOT change projection policy, caps, the plugin, generated code, or any wire output.
- Do NOT make the bench part of the CI hard-gate negative-codegen path; it links alongside and runs on demand.
- Do NOT rely on `assert` for the sub-ms gate — use an explicit check + non-zero `return` (AC2 requires the exit code; `assert` is stripped under `-DNDEBUG`).
- Do NOT hand-tune to make numbers look good — if a size is NOT sub-ms, the bench must fail loudly (that is the whole point of criterion H being evidence).

### Reference shape (bench, illustrative — not prescriptive)
```cpp
#include <chrono>
#include <cstdio>
#include <cstddef>
#include "common/metadata_sink.h"
#include "sys1.proj.h"

static sys1::v1::CalculateRequest makeReq(int n) { /* tool_id + n contexts, 7 fields each */ }

int main() {
  constexpr int kIters = 2000;
  constexpr long long kBudgetNs = 1'000'000;          // 1 ms
  const int sizes[] = {1, 2, 25, 60};
  bool failed = false;
  std::size_t guard = 0;
  for (int n : sizes) {
    auto req = makeReq(n);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      routingmeta::VectorSink sink;                    // fresh each call
      auto r = ProjectMeta(req, sink);                 // ADL on sink
      guard += sink.items.size() + (r.ok ? 1 : 0);     // defeat -O2 elision
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long long per = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / kIters;
    std::printf("projection: N=%2d contexts -> %8.3f us/call (%lld ns, %d iters)\n",
                n, per / 1000.0, per, kIters);
    if (per >= kBudgetNs) { std::fprintf(stderr, "FAIL: N=%d exceeded 1 ms (%lld ns)\n", n, per); failed = true; }
  }
  std::printf("%s (guard=%zu)\n", failed ? "BENCH FAILED" : "BENCH PASSED", guard);
  return failed ? 1 : 0;
}
```

### Testing standards
- Same harness philosophy as `tests/test_projection.cc`: plain C++, zero test deps, built via `example/build.sh`. The bench's "test" is its own pass/fail: prints the 4 lines, `BENCH PASSED`, exit 0. The red-green proof (Method, above) is the one runnable check that the gate actually gates.

### Project Structure Notes
- NEW file: `example/tests/bench_projection.cc`. Edit: `example/build.sh` (one `[bench]` link line). No change to plugin, generated code, kit headers, sender, or receiver.

### Previous story intelligence (Stories 1.1–1.6)
- 1.6 made `ProjectMeta` self-time into `ProjResult.duration` and deferred resolution-robust timing to THIS story — the bench's many-iteration interval design is that resolution-robust measurement. 1.5 single-sourced the caps in `process_context_emit.h` (do not re-hardcode them). 1.2 made overflow non-blocking data — relevant to N=60, which trips the count cap but still returns fast (ok=true, one Overflow issue). 1.1/1.3/1.4 established `ProjResult`, the `routingmeta` namespace + ADL resolution of `ProjectMeta(req, sink)`, and the kit `Send`. The bench calls `ProjectMeta` directly (not `Send`) — it measures projection, not the full sender path.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.7] — user story + 3 ACs.
- [Source: refs/BRIEF.md#H] — "Perf observed (plan.md P0.4). `duration` reported per call; a micro-bench prints time for 1/2/25/60 contexts; sub-ms."
- [Source: ARCHITECTURE-SPINE.md#FR7] — projection self-times; bench proves sub-ms.
- [Source: ARCHITECTURE-SPINE.md#AD-6] — `bench_projection` measures `ProjectMeta` directly; one timing point.
- [Source: ARCHITECTURE-SPINE.md#AD-11 / NFR3] — no new runtime/test deps; plain `std::chrono` loop.
- [Source: grpc-routing-meta/example/tests/test_projection.cc:23,52] — `sys1Req` builder + `ProjectMeta` call shape to mirror.
- [Source: grpc-routing-meta/example/src/common/process_context_emit.h:29-31,56] — caps (25/512/7168) and the overflow condition that N=60 trips.
- [Source: grpc-routing-meta/example/build.sh] — step 4 link block to mirror for `[bench]`.
- [Source: deferred-work.md#1-6] — "Resolution-robust timing assertions → Story 1.7"; this story resolves it.

### Latest tech notes
No external research. `std::chrono::steady_clock` averaged over a fixed iteration count is the standard dependency-free micro-bench pattern; monotonic, ns-resolution on the Linux CI matrix (AD-14) and ~10s-of-ns on the macOS dev host. C++17, protobuf 3.20/3.21 pinned. No benchmark framework (NFR3/AD-11).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer) — engineering delegated to a general-purpose subagent; main loop did the create-story, independent verification (re-ran build + bench + regression), and BMad bookkeeping.

### Debug Log References

- Implementation delegated to a subagent (per user request); main loop independently re-built and re-ran before marking review (not trusting the subagent's word).
- Red (subagent): with `kBudgetNs` set to `1` ns, `bench_projection` printed all 4 timing lines, printed `BENCH FAILED`, and exited **1** — proving the sub-ms gate actually gates via the explicit `return failed ? 1 : 0` (not `assert`).
- Green (independently re-run in main loop): clean `./build.sh` links `bench_projection` alongside the others; `./build/bench_projection` → 4 lines, all sub-ms, `BENCH PASSED`, exit 0. Observed per-call (macOS dev host): N=1 ≈ 3.9 µs, N=2 ≈ 6.2 µs, N=25 ≈ 48.6 µs, N=60 ≈ 41.0 µs — max ~49 µs, ≈ 20× under the 1 ms budget.
- Sanity: N=60 (41 µs) < N=25 (49 µs) confirms the overflow short-circuit (count > 25 → no digest/emit). Well-ordered with context count up to the cap, then drops.
- No regression: `./build/test_projection` → `ALL TESTS PASSED`. Scope: `git diff --stat` shows exactly one new file (`tests/bench_projection.cc`) + a 2-line `build.sh` add; plugin, generated code, kit headers, sender, receiver, and wire output untouched.

### Completion Notes List

- **AC1** — `tests/bench_projection.cc` prints one per-call line for N=1/2/25/60 contexts (`projection: N=… -> … us/call (… ns, … iters)`), built from a local `makeReq(n)` mirroring `test_projection.cc`'s `sys1Req`.
- **AC2** — Each measured per-call time is sub-millisecond; the binary exits non-zero on breach via an explicit `per >= kBudgetNs` check + `return failed ? 1 : 0` (NOT `assert` — robust under `-DNDEBUG`). Proven both ways: red (tiny budget → exit 1) and green (1 ms budget → exit 0).
- **AC3** — `build.sh` builds and links `bench_projection` alongside `unified_sender` / `receiver_verify` / `test_projection` (one `[bench]` line, same `GEN_SRCS`/`PBFLAGS`).
- **Resolution-robust timing (closes 1.6 deferral):** measurement averages `kIters=2000` calls inside ONE `steady_clock` interval, so total ≫ clock granularity — the per-call figure is sound even on a coarse clock; no fragile single-call `> 0`. Fresh `VectorSink` per iteration (sink accumulates); a `guard` accumulator defeats `-O2` elision.
- **Scope held:** no policy/cap changes, no new dependency (plain `std::chrono`, no benchmark framework — NFR3/AD-11), no header added, no timing inside the kit, wire byte-identical (CR1/AD-9). Caps stay single-sourced (the bench hardcodes none).

### File List

- `grpc-routing-meta/example/tests/bench_projection.cc` (NEW — sub-ms micro-bench; averages `kIters` `ProjectMeta` calls per size {1,2,25,60} under one `steady_clock` interval; explicit non-zero exit on > 1 ms)
- `grpc-routing-meta/example/build.sh` (MODIFIED — one `[bench] bench_projection` link line after `[test] test_projection`)

## Change Log

- 2026-06-28 — Story 1.7 drafted (create-story): `bench_projection` proves sub-ms for 1/2/25/60 contexts (FR7, BRIEF H); resolution-robust many-iteration timing closes the 1.6 deferral; explicit non-zero exit on breach; no new deps, wire untouched.
- 2026-06-28 — Story 1.7 implemented (dev-story): NEW `tests/bench_projection.cc` + `build.sh` `[bench]` link line. Red (tiny budget → exit 1) then green (1 ms → exit 0); all 4 sizes sub-ms (max ~49 µs, ≈20× under budget) on the macOS dev host; `test_projection` still green. Resolution-robust averaging over `kIters=2000` resolves the 1.6 deferred timing item. Engineering by a subagent, independently re-verified in the main loop. Status → review.
- 2026-06-28 — Story 1.7 code review (3 adversarial subagents, no shared context): Auditor PASS 3/3 ACs, Edge PASS (hand-verified guard=88000 path-by-path), Blind CONCERNS (mean-not-max). 1 comment-only patch (scope the claim to mean-per-call criterion-H evidence + label N=60 overflow; `ponytail:` note on why a max/p99 gate would be flaky), 4 dismissed, 0 deferred. Rebuilt green. Status → done.
