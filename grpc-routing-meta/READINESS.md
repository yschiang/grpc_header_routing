# Production-Readiness: grpc-routing-meta/example  →  READY

_Gate re-run 2026-07-09 via the `cpp-production-readiness` skill (previous snapshot 2026-06-28
superseded; its D2 tooling lesson is kept below). Scope: whole `example/` tree on branch
`productionize-grpc-routing-meta`. Standard: C++17 · Multi-threaded scope: projection path only
(concurrent-read) · Coverage threshold: 80% lines._

Changes audited since the last snapshot: sys2 RMS reshape (`x-recipe-id` projection, real-shape
camelCase `rqst_RMS_GetRecipeSet`, four sender patterns), plugin `lowercase_name()` getter fix,
TUTORIAL.zh.md, README/DEMO/OVERVIEW sync, sanitizer + coverage cells added to `ci.yml`.

**Every automated gate is green** — including the previously-GAP'd TSan (now clean locally AND
wired in CI) and coverage (now 100%). **D8 signed off by yschiang (human), 2026-07-09.**
IWYU remains a named GAP, accepted by the owner with the READY sign-off.

## Discovered toolchain (Step 1)

| Need | Found |
|------|-------|
| build | CMake (`example/CMakeLists.txt`) + `build.sh`; work dir `grpc-routing-meta/example` |
| standard / compilers | C++17; CI matrix g++/clang++ × protobuf 3.20.3/3.21.12; local Apple clang (arm64), protoc 3.20.3 (anaconda) |
| tests | assert()-based binaries; `ctest` targets `projection`, `bench`; negative-codegen gate (9 fixtures, rejected-for-the-right-reason) in `build.sh` |
| coverage | `example/coverage.sh` (gcovr; gates hand-written `src/`, excludes generated) — also a CI job |
| static analysis | `example/.clang-tidy` + cppcheck 2.21; clang-tidy/clang-format 18.1.8 (pinned) |
| format | `.clang-format` (Google, ColumnLimit 100) — advisory, not enforced (hand-aligned columns documented) |
| CI | `.github/workflows/ci.yml`: build matrix + neg-gate + tests + CMake/ctest + gRPC smoke + **ASan/UBSan cell + TSan cell + coverage job** |
| threading | production code single-threaded; projection tested for concurrent-read safety |
| sanitizers | ASan/UBSan/TSan wired in CI; run locally this session (LSan unsupported on macOS → `leaks --atExit`) |

## Dimensions

| # | Dimension | Type | Check | Result | Evidence (2026-07-09) |
|---|-----------|------|-------|--------|----------|
| 1 | Build & compile | auto-gate | `./build.sh` · CMake+ctest | **PASS** | build.sh exit 0 ("OK -> binaries"); cmake build + `ctest` 2/2 (local cmake is a Rosetta x86_64 binary → needs `-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_PREFIX_PATH=<protobuf>`; environment quirk, not a repo defect) |
| 2 | Static analysis | auto-gate | cppcheck · clang-tidy (18.1.8, `-isysroot`) | **PASS** | cppcheck: 0 findings; clang-tidy on plugin/sender/receiver/tests: 0 findings |
| 3 | Tests + coverage | auto-gate | binaries + neg-gate + `./coverage.sh` | **PASS** | "ALL TESTS PASSED" (incl. sys2 RMS + camelCase real-shape blocks); neg-gate 9/9; coverage **src/ 100%** (143/143) ≥ 80 |
| 4a | Error handling (no swallowed catch) | auto-gate | empty-catch grep | **PASS** | 0 matches in `src/ sender/ receiver/ tests/` |
| 4b | Error handling (strategy / RAII) | sign-off | gate-review (AI)¹ | **PASS** | report-don't-throw unchanged; new sender patterns forward `ProjResult` identically; pure RAII/std types |
| 5a | Concurrency (race) | auto-gate | concurrent test + TSan | **PASS** | concurrent-`ProjectMeta` test green; **TSan build clean** (`-fsanitize=thread`, local) — previous GAP closed; TSan cell also in `ci.yml` |
| 5b | Concurrency (lock ordering) | sign-off | gate-review (AI)¹ | **PASS** | production path lock-free; only immutable `static const` shared |
| 6a | Code quality (format / includes) | auto-gate | clang-format · IWYU | **ADVISORY · GAP** | hand-aligned columns documented (unchanged policy); **IWYU still not installed → GAP** |
| 6b | Code quality (naming / comments / magic#) | sign-off | gate-review (AI)¹ | **PASS** | previous nit resolved: `process_context_emit.h:47-50` now names the `24/7/64→71` digest byte constants in the comment |
| 7 | Observability | sign-off | gate-review (AI)¹ | **PASS** | unchanged: in-band `x-routing-error` / `-overflow` + `ProjResult`; new empty-recipe sender pattern demos it end-to-end |
| 8 | Docs & review | sign-off | **yschiang (human)** | **PASS** | **confirmed by yschiang, 2026-07-09** — TUTORIAL.zh.md added (adopter path incl. §1.1 toolchain compat); README/DEMO/OVERVIEW synced to sys2 RMS state; 16-vs-21 framing accepted |
| 9 | Memory safety | auto-gate | ASan+UBSan + `leaks` | **PASS** | ASan+UBSan (`-fno-sanitize=vptr,function`): 3 drivers clean; `leaks --atExit`: 0 leaks × 4 binaries |

¹ Sign-offs D4b/D5b/D6b/D7 reviewed by the cpp-production-readiness gate (AI), delegated by
yschiang (2026-06-28, re-reviewed 2026-07-09) — NOT a human attestation; reopen if a true human
sign-off is required.

### Carried lesson (from 2026-06-28 run)
clang-tidy without a real compilation database / sysroot parses a recovery AST and produces
confident false positives — always pass `compile_commands.json` or `-isysroot $(xcrun --show-sdk-path)`
and verify zero `clang-diagnostic-error` before trusting findings.

## Blockers
None. D8 attested by yschiang (2026-07-09). Accepted GAP: IWYU not installed
(unused-include check unverified) — accepted by the owner with the READY sign-off.

## Commands run (reproduce, 2026-07-09)
```
$ cd grpc-routing-meta/example
$ ./build.sh                                                   # D1 + D3 neg-gate + tests
$ ./coverage.sh                                                # D3 coverage (src/ 100% >= 80)
$ cppcheck --std=c++17 --enable=warning,performance,portability \
    --error-exitcode=1 --suppress=missingIncludeSystem -I src --quiet src sender receiver   # D2
$ clang-tidy src/plugin/protoc-gen-meta.cc sender/unified_sender.cc receiver/receiver_verify.cc \
    tests/test_projection.cc -- -std=c++17 -isysroot $(xcrun --show-sdk-path) \
    -Ibuild/generated -Isrc -I<protobuf>/include                                            # D2
$ grep -rnE 'catch[[:space:]]*\([^)]*\)[[:space:]]*\{[[:space:]]*\}' src sender receiver tests  # D4a
$ clang++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize=vptr,function -fno-sanitize-recover=undefined ... && ./<3 drivers>        # D9
$ clang++ -std=c++17 -O1 -g -fsanitize=thread -pthread tests/test_projection.cc ... && ./test_tsan  # D5a
$ leaks --atExit -- ./build/<test_projection|unified_sender|receiver_verify|bench_projection>   # D9
$ cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=<protobuf-prefix> -DCMAKE_OSX_ARCHITECTURES=arm64 \
    && cmake --build build/cmake -j && ctest --test-dir build/cmake                          # D1
```
_Tooling: cppcheck 2.21, gcovr 8.6, clang-format/clang-tidy 18.1.8 (pinned), Apple clang arm64._
