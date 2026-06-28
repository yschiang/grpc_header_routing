---
name: cpp-production-readiness
description: Use when a C++ module, PR, or repo is about to ship and needs a production-readiness gate / acceptance check / 上線前驗收 / 驗收 before merge or release. Triggers — "is this ready to ship?", "production readiness", "pre-merge gate", "verify/accept this module", 上線前把關. Validates and leaves evidence only; to WRITE or refactor C++ use cpp-pro.
license: MIT
metadata:
  author: generic C++ readiness gate
  version: "2.1.0"
  domain: language
  role: gate
  scope: verification
  output-format: markdown-report
  related-skills: cpp-pro
---

# C++ Production-Readiness Gate

Verification counterpart to **cpp-pro**. cpp-pro *writes* the code; this *signs off* on it.
It does not edit, restyle, or add features — it runs checks and emits an evidence report.
Works on any C++ repo: it **discovers** the real toolchain at run time rather than assuming one.

## Iron rule

Each dimension resolves to **exactly one of two things — no vibes in between**:

- **auto-gate** — a command that runs in THIS repo and returns a binary PASS/FAIL (exit code or
  threshold). The dimension + PASS condition are the contract; the command is an *example* — you
  **discover** the real one (Step 1), never paste one from memory. Tool/instrumentation absent →
  **GAP** (not PASS); offer to wire it.
- **sign-off** — a check that cannot be mechanized (observability, naming, "strategy is consistent",
  review approval). A **named human attests**, recorded as evidence (who + when). No attestor = not done.

No subjective PASS condition dressed as an automated check. No anonymous or self-granted sign-off.
No fabricated pass.

## When to use

- A module/PR/repo is about to merge or release and someone wants sign-off ("ready to ship?", 上線前驗收).
- **NOT** for writing/refactoring code → **cpp-pro**. **NOT** for one bug → systematic-debugging.

## Step 1 — Discover the toolchain (MANDATORY, every run)

Probe the repo; use what you find. Don't proceed until each row is answered (value found, or GAP).

| Need | Probe | Maps to |
|------|-------|---------|
| build system | `ls CMakeLists.txt BUILD.bazel WORKSPACE Makefile meson.build *.sh` | cmake `--build` / `bazel build` / `make` / `meson compile` / repo build script |
| C++ standard | grep `CXX_STANDARD` / `-std=` in build files & CI | c++17 / 20 / 23 |
| compilers | CI files, `CMAKE_CXX_COMPILER` | gcc / clang (+ versions) |
| test runner | test dirs; grep `gtest`/`catch2`/`doctest`/`add_test`/`enable_testing`; `assert(` | `ctest` / `bazel test` / a gtest\|catch2 binary / assert-exe |
| coverage | grep `--coverage`/`-fprofile`/`gcov`/`lcov`/`llvm-cov`; coverage script | gcovr/lcov pipeline, else **GAP** |
| static analysis | `ls .clang-tidy .cppcheck*`; `command -v clang-tidy cppcheck` | clang-tidy (+ `compile_commands.json`) / cppcheck, else **GAP** |
| formatter | `ls .clang-format`; `command -v clang-format` | clang-format — **pin a version** (skew = phantom diffs) |
| CI | `ls .github/workflows .gitlab-ci.yml azure-pipelines.yml` | mirror exactly what CI runs |
| threading | grep `std::thread`/`mutex`/`atomic`/`pthread`/`Threads::Threads` | decides whether D5 applies |
| sanitizers | grep `-fsanitize` | ASan/TSan/UBSan wired, else GAP |

Then **confirm scope** with the user: file / module / repo; C++ standard; is a multi-threaded path in
scope; coverage threshold (default 80% lines). `${FILES}` = scope's hand-written `.cc`/`.h`
(e.g. `git ls-files '**/*.cc' '**/*.h' | grep -vE '/(build|third_party|vendor|generated)/'`).

## The 8 dimensions

**Read the PASS condition + Type as the contract. The command is an example to adapt per repo.**

| # | Dimension | PASS condition (the contract) | Type | Example check |
|---|-----------|-------------------------------|------|---------------|
| 1 | **Build & compile** | build exits 0, no errors. Warnings are reported; they FAIL only if the project sets `-Werror`/opts in | auto-gate | run the discovered build; capture exit + warning count |
| 2 | **Static analysis** | **no _new_ findings** vs base/changed-lines (not "zero on the whole tree") | auto-gate | `clang-tidy-diff.py`/cppcheck on the diff vs `<base>`, or against a committed baseline; no tool/config → GAP |
| 3 | **Tests + coverage** | test runner exits 0; the project's own **edge/negative gates** run too; line coverage over **hand-written** code ≥ threshold (generated/vendored/third_party excluded) | auto-gate | discovered test runner; coverage script or instrument (`--coverage`+gcovr) → `--fail-under-line <T>` |
| 4 | **Error handling** | (a) no empty/swallowed catch; (b) error-return/exception strategy consistent + resources freed on every error path | (a) auto-gate · (b) sign-off | (a) `grep -rnE 'catch[[:space:]]*\([^)]*\)[[:space:]]*\{[[:space:]]*\}' ${dirs}` → PASS if none. (b) reviewer attests |
| 5 | **Concurrency** (only if threading detected) | (a) concurrency tests pass + TSan reports no race; (b) lock ordering / no deadlock | (a) auto-gate (TSan may be GAP) · (b) sign-off | (a) run concurrent tests; rebuild `-fsanitize=thread`. (b) reviewer attests |
| 6 | **Code quality** | (a) **changed** lines formatter-clean (pinned version) + no unused includes; (b) naming, why-not-what comments, no magic numbers, no dead code | (a) auto-gate · (b) sign-off | (a) `git clang-format <base> -- ${FILES}` → no diff; IWYU if present. (b) reviewer attests |
| 7 | **Observability** | every failure path surfaces a signal a monitor can catch (log/metric/error return); key paths logged at levels | sign-off | grep the log/error-emit surface as *evidence for the reviewer's call* — the grep is not the gate |
| 8 | **Docs & review** | named reviewer approved; docs updated for the change; breaking changes recorded | sign-off | `git diff --name-only <base>... -- '*.md'`, `git log --oneline <base>...` as evidence; PR/reviewer approval attests |

## Run protocol

1. Step 1 discovery + scope confirmation.
2. Run every **auto-gate**. Record exact command, exit code/threshold, 1–3 line evidence excerpt.
3. For each **GAP**, state it plainly. Wire it only if the user wants; otherwise GAP = not-verified, **not** PASS.
4. For each **sign-off**, put the question to a named human (user/reviewer); record the attestation
   as evidence ("confirmed by <name>, <date>"). An un-attested sign-off blocks READY.
5. **Verdict:** `READY` only if every in-scope auto-gate is PASS **and** every sign-off is attested
   (or an explicitly accepted GAP / N-A). Any FAIL or un-attested sign-off → `NOT READY`.

## Report format (always emit)

```markdown
# Production-Readiness: <scope>  →  READY | NOT READY
Standard: c++<N> · Multi-threaded scope: yes/no · Coverage threshold: <T>%

| # | Dimension | Type | Check (command) | Result | Evidence |
|---|-----------|------|-----------------|--------|----------|
| 1 | Build & compile | auto-gate | <discovered cmd> | PASS | exit 0 |
| 7 | Observability | sign-off | reviewer | PASS | confirmed by <name>, <date> |
| ... | | | | | |

## Blockers (if NOT READY)
- D<n>: <what failed / un-attested> — <command or who-owes-sign-off> — <evidence>

## Commands run (reproduce)
$ <every auto-gate command, in order>
```

The "Commands run" block is mandatory — anyone must be able to paste it and reproduce the auto-gates.

## Common mistakes

- **Dressing a sign-off as an auto-gate.** Observability / naming / "consistent strategy" / review
  aren't commands. Mark them sign-off and get a *named* attestation — don't fake a grep PASS.
- **Faking a GAP as PASS.** Tool/instrumentation absent → GAP. Don't invent a passing command.
- **Assuming instead of discovering.** Step 1 is mandatory; a command from memory or another repo isn't evidence.
- **"Zero findings" / whole-tree formatting on existing code.** Backlog ≠ regression. Gate *new*
  findings / *changed* lines; pin tool versions; intentional/dense blocks → `// clang-format off`, not a rewrite.
- **Counting generated / vendored / third_party in the coverage threshold.** Measure hand-written code only.
- **Building assert-based tests with `-DNDEBUG`.** Compiles out every `assert()` → tests pass while checking nothing.
- **PASS on a partial run, or editing/restyling code.** Incomplete = not READY. This is a gate; cpp-pro fixes.
```
