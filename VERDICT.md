# VERDICT — A/B: superpowers vs BMAD on productionizing grpc-routing-meta

_Re-judged 2026-06-29 after a full read of both codebases (not just metrics). Both built
+ run locally (protoc 3.20.3, cmake, g++/clang++). gRPC parts needing `grpc++` noted
CI-only, not penalized. Anchor: `refs/OVERVIEW.zh.md` §7/§10 + `refs/BRIEF.md`._

- **A** = superpowers (`brainstorming` → `writing-plans` → TDD), `../ab-superpowers/`
- **B** = BMAD (`bmad-prd` → architecture → epics/stories → dev/review), `../ab-bmad/`

## Two corrections to the first verdict (both had unfairly favored B)

1. **`kHpackEntryOverhead` DRY** — first verdict said A duplicated the `32` constant with a
   "must match" comment. **False.** Both single-source it in `metadata_sink.h`; A's
   `process_context_emit.h` only comments back to it. A has no DRY break here.
2. **Receiver reject path** — first verdict said only B exercises the digest reject path.
   **False** (a truncated capture on my side). A's `receiver_verify` runs accept **and**
   tamper-reject (CH-A→CH-X, non-zero exit on failure) with an explicit PASS/FAIL
   self-check. Both are complete.

Also re-scoped: **Send ownership**. Orchestration (compose `FillCommon`+`ProjectMeta`,
decide abort/proceed) belongs to the **sender department**, not the kit. A kept Send
sender-side (correct); B put it in the kit (over-reach) — and BMAD's own architecture had
flagged this as AD-4 ("Send not in kit") then *reverted* to conform to plan.md P0.3.

## Gate — both PASS

Green build, all binaries, `test_projection` ALL PASSED, digests reproduce the OVERVIEW
bytes exactly, empty-mask → `x-routing-error`+`ok=false` (no throw), overflow → explicit
non-blocking flag. (B's `build.sh` needs `PKG_CONFIG_PATH` on a non-pkg-config toolchain;
A builds out-of-box via prefix discovery.)

## Corrected scorecard

| 面向 | A | B | edge |
|---|---|---|:--:|
| **架構正確性(Send 所有權)** | sender-side(對) | kit 內(越界;AD-4 提對又 revert) | **A** |
| Clean code 核心(plugin/parser/sink/proj_result) | 幾乎逐字相同 | 幾乎逐字相同 | 平 |
| 註解自足性 | 不綁文件編號 | 夾 AD-/FR-/NFR- 編號 | A(小) |
| magic number / `duration_cast` | `24+71` 裸寫;duration 隱式轉 | 有註解;顯式 cast | B(小) |
| **測試深度/嚴謹** | 69 asserts,全 invariant、乾淨 | 181 asserts:25/26·512/513·7168/7169 逐 byte 卡界、10 mask path、獨立 digest preimage、NIST 1M KAT、co-occurrence | **B(大)** |
| **記憶體安全 CI**(ASan/UBSan/TSan/coverage) | 無 | 有 | **B** |
| Build 健壯(out-of-box)+ 雙 build-path CI | build.sh+CMake 皆驗;隨處 build | 需 PKG_CONFIG_PATH;CI 只 build.sh | **A** |
| 整合實證 | real-wire gRPC client+server | 只 compile-smoke | **A** |
| 程式碼精簡 | 1244 行 / 17 commits | 1641 行 / 38 commits + 4225 行 process 文件 | A |
| 失敗模型 / doc-truth / receiver(accept+reject) | 對 | 對 | 平 |

## Bottom line — near-tie, clear division of strengths

- **A wins on design + robustness + leanness:** Send ownership correct (B's kit-Send is a
  real boundary defect to undo), builds anywhere, dual build-path CI, real-wire demo,
  leaner and self-contained code.
- **B wins on verification + safety:** test rigor is a tier above (byte-exact overflow
  bounds, all 10 mask paths, NIST KATs, co-occurrence), plus ASan/UBSan/TSan + coverage.

After correcting my two earlier errors, the core clean-code is a genuine tie, and **A takes
a slight overall edge** on the single most fundamental axis — **architectural correctness**
(the Send boundary B got wrong, even after its process surfaced the right answer). But B's
**test + memory-safety discipline is the best-in-class reference here**.

**Practical recommendation:** adopt **A as the base** (right design, lean, builds anywhere)
and **graft B's test suite + sanitizer/coverage CI** — each side's strongest part combined.

## Each team's fixes (unchanged)

- **A:** add ASan/UBSan + coverage CI; deepen unit asserts toward B's boundary-lock level;
  make the sender-owned Send explicit (sample + wiring contract) and record the plan.md
  P0.3 deviation as an ADR.
- **B:** move Send out of the kit (restore AD-4); `build.sh` prefix-discovery fallback so it
  builds out-of-box; exercise the CMake path in CI; move AD-/FR- IDs out of shipped code.
