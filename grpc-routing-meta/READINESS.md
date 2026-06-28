# Production-Readiness: grpc-routing-meta/example  →  NOT READY (pending human sign-offs)

_Gate run 2026-06-28 via the `cpp-production-readiness` skill. Scope: whole `example/` tree
(new on branch `productionize-grpc-routing-meta`; 0 files in `main`). Standard: C++17 ·
Multi-threaded scope: projection path only (concurrent-read) · Coverage threshold: 80% lines._

Every automated gate is green and the technical sign-offs (D4b/D5b/D6b/D7) pass on review. The
blocker is **D8 — docs/review, which the owner flagged NEEDS WORK** — plus named GAPs (TSan, IWYU,
CI coverage). No code defect; the gap is docs/review-readiness.

## Discovered toolchain (Step 1)

| Need | Found |
|------|-------|
| build | CMake 3.16+ (`example/CMakeLists.txt`) + `build.sh` fallback; work dir `grpc-routing-meta/example` |
| standard / compilers | C++17; CI matrix g++/clang++ × protobuf 3.20.3/3.21.12; local Apple clang 14, protoc (anaconda) |
| tests | assert()-based binaries (not gtest); `ctest` targets `projection`, `bench`; + negative-codegen gate in `build.sh` |
| coverage | `example/coverage.sh` (gcovr + llvm-cov; gates hand-written `src/`, excludes generated) |
| static analysis | `example/.clang-tidy` + cppcheck; clang-tidy/clang-format pinned to 18.1.8 (pip wheel) |
| format | `.clang-format` (Google, ColumnLimit 100, IndentWidth 2) |
| CI | `.github/workflows/ci.yml` (build.sh + neg-gate + tests + CMake/ctest; grpc smoke) |
| threading | production code single-threaded; projection tested for concurrent-read safety |
| sanitizers | not wired in CI; run ad hoc this session (LSan unsupported on macOS → `leaks`) |

## Dimensions

| # | Dimension | Type | Check | Result | Evidence |
|---|-----------|------|-------|--------|----------|
| 1 | Build & compile | auto-gate | `./build.sh` | **PASS** | exit 0; "OK -> binaries in build/"; warnings: `-Wall` only (no `-Werror`) |
| 2 | Static analysis | auto-gate | cppcheck · clang-tidy (pinned 18.1.8, `-isysroot`) | **PASS** | cppcheck: 0 findings. clang-tidy: 0 findings (after the `Proj` member-init fix; see correction note) |
| 3 | Tests + coverage | auto-gate | `ctest` / binaries + `./coverage.sh` | **PASS** | `test_projection`: "ALL TESTS PASSED"; `bench` exit 0; neg-gate rejects all `tests/negative/*.proto`; coverage **src/ 95%** (136/142) ≥ 80 |
| 4a | Error handling (no swallowed catch) | auto-gate | empty-catch grep | **PASS** | 0 matches in `src/ sender/ receiver/` |
| 4b | Error handling (strategy / RAII) | sign-off | gate-review (AI)¹ | **PASS** | report-don't-throw, consistent; error paths hold no raw resources (pure RAII/std types) |
| 5a | Concurrency (race) | auto-gate | concurrent test; TSan | **PASS (test) · GAP (TSan)** | `test_projection` spins `std::vector<std::thread>`, passes; TSan not run/wired |
| 5b | Concurrency (lock ordering) | sign-off | gate-review (AI)¹ | **PASS** | production path lock-free; only immutable `static const` shared; no deadlock surface (TSan still GAP, D5a) |
| 6a | Code quality (format / includes) | auto-gate | clang-format · IWYU | **ADVISORY · GAP** | tree intentionally hand-aligned (`[+meta]/[app]/[demo]` columns, dense `sha256.h`) — clang-format not enforced (documented); IWYU not installed |
| 6b | Code quality (naming / comments / magic#) | sign-off | gate-review (AI)¹ | **PASS¹** | strong naming + why-comments; nit: `process_context_emit.h:45` `24 + 71` unexplained magic numbers |
| 7 | Observability | sign-off | gate-review (AI)¹ | **PASS** | every failure emits in-band `x-routing-error`/`x-process-context-overflow` + `ProjResult`; opaque-transport-failure designed out; caller-logs by design |
| 8 | Docs & review | sign-off | **yschiang (human)** | **NEEDS WORK** | owner flagged 2026-06-28; doc audit done + live-doc bugs fixed (see note); 16-vs-21 + refs reconciliation pending owner |
| 9 | Memory safety | auto-gate | `leaks` + ASan + UBSan | **PASS** | `leaks --atExit`: 0 leaks (4 binaries); ASan + UBSan (`-fno-sanitize=vptr,function`): clean (3 drivers) |

### D2 clang-tidy — correction (2026-06-28)
The earlier "4 findings" were mostly a **tooling artifact**: clang-tidy ran without the C++
stdlib/SDK include paths, so it parsed a recovery AST (`error: 'cassert' file not found`) and
`cppcoreguidelines-init-variables` misfired on 3 already-initialized variables (`is_msg`, `of`,
`of2`). Re-run with `-isysroot $(xcrun --show-sdk-path)`: those vanished. The one real finding —
`protoc-gen-meta.cc:39` `Proj` member-init (introduced by the earlier cppcheck `= false` fix) — was
resolved by default-initialising all members (`std::string key{}; bool required = false; std::string getter{};`).
clang-tidy now reports **0 findings**. _Lesson: clang-tidy without a real compilation database produces false positives — always pass `compile_commands.json` or the sysroot._

¹ **Sign-offs D4b/D5b/D6b/D7 were reviewed by the cpp-production-readiness gate (AI), delegated by
yschiang on 2026-06-28 — NOT a human attestation.** They pass on review; reopen if a true human
sign-off is required. D6b's magic-number nit (`process_context_emit.h:45`) is non-blocking and not
yet fixed. D8 is a human sign-off: yschiang flagged it **NEEDS WORK** on 2026-06-28.

### D8 — docs audit (2026-06-28)
Audited README / CONTEXT / OVERVIEW.zh / DEMO / SPEC against the shipped code. Wire-contract
content is accurate (numbers, headers, schema, digest, overflow, error model, negative gate all
match). **Fixed in the live tree:** added co-located `SPEC.md` (was missing → dead `[SPEC.md]`
links); removed `archive/` pointers (CONTEXT, OVERVIEW); redirected the dead `example/TESTING.md`
ref → `DEMO.md`; refreshed README layout + doc-links (`grpc_demo/`, bench, `negative/`,
`proj_result.h`, `common_headers.h`, DEMO/SPEC); corrected stale LOC in OVERVIEW (335→~460, 616→~675).
**Held for owner:** (a) 16-vs-21 transaction count — OVERVIEW sizes its tables on ~21, code/protos
have **16** (confirm whether 21 is real-world scope or stale); (b) live `CONTEXT`/`OVERVIEW` diverge
from the read-only `refs/` canonical — source-of-truth reconciliation.

## Blockers (to reach READY)
1. **D8 — NEEDS WORK (owner, 2026-06-28)** — live-doc bugs fixed; still needs owner decision on
   16-vs-21 + refs reconciliation, then owner re-sign. The hard blocker.
2. **GAPs** — TSan run on the concurrency test; IWYU for unused includes; wire cppcheck/clang-tidy/coverage/sanitizers into CI (currently local-only).
3. **(optional)** D6b nit — name the `24 + 71` constants in `process_context_emit.h:45`.

## Commands run (reproduce)
```
$ cd grpc-routing-meta/example
$ ./build.sh                                                   # D1 build + D3 neg-gate
$ ./build/test_projection && ./build/bench_projection         # D3 tests
$ ./coverage.sh                                                # D3 coverage (src/ 95% >= 80)
$ cppcheck --std=c++17 --enable=warning,performance,portability \
    --error-exitcode=1 --suppress=missingIncludeSystem -I src --quiet src sender receiver   # D2 cppcheck
$ clang-tidy <hand-written .cc> -- -std=c++17 -isysroot $(xcrun --show-sdk-path) -Ibuild/generated -Isrc -I<protobuf>/include   # D2 clang-tidy (sysroot REQUIRED, else false positives)
$ grep -rnE 'catch[[:space:]]*\([^)]*\)[[:space:]]*\{[[:space:]]*\}' src sender receiver      # D4a
$ leaks --atExit -- ./build/test_projection   # (+ unified_sender, receiver_verify, bench)    # D9 leaks
$ c++ -std=c++17 -fsanitize=address,undefined -fno-sanitize=vptr,function ... && ./<bin>      # D9 ASan/UBSan
```
_Tooling: cppcheck 2.21, gcovr 8.6, cmake 4.3.4, clang-format/clang-tidy 18.1.8 (pinned, pip wheel)._
