---
baseline_commit: be15e8678a9fac2f2315ba9ae57ce52a5130410c
---

# Story 1.9: Proven on a pinned CI matrix

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a build/release engineer,
I want CI to prove the kit on a real matrix,
so that it cannot silently bind to one compiler or protobuf version.

## Acceptance Criteria

1. **AC1 — A GitHub Actions matrix workflow exists.** Given `.github/workflows/`, when inspected, then a GitHub Actions workflow defines a matrix Linux × {gcc, clang} × {protobuf 3.20.3, 3.21.12}, building each protobuf from source at its pinned tag into a job-local prefix, cached on the tag. (AD-14)

2. **AC2 — Each job runs the full local proof + the gRPC smoke.** Given each matrix job, when it runs, then it performs: build + negative-codegen gate + `unified_sender` + `receiver_verify` + `test_projection` + `bench_projection` + a gRPC compile-smoke of the `ROUTINGMETA_WITH_GRPC` `GrpcSink` adapter. (NFR2, HR4)

3. **AC3 — Mirrors local steps; validated by local-path equivalence (no remote run).** Given the workspace forbids push, when the workflow is authored, then it mirrors exactly the locally-verified steps and is validated by local-path equivalence, not a remote green run. (PRD §6)

## Tasks / Subtasks

- [x] **Task 1 — gRPC compile-smoke TU: instantiate the `GrpcSink` + ADL path (AC: 2, HR4)** — NEW file `example/tests/grpc_smoke.cc` (resolves the Story-1.3 deferred item)
  - [x] Create a tiny standalone TU whose ONLY job is to prove the gRPC adapter compiles and the ADL call resolves. Under `#ifdef ROUTINGMETA_WITH_GRPC`, in a `main()`, place a compile-only (never-executed) instantiation:
    ```cpp
    #include "common/metadata_sink.h"   // defines routingmeta::GrpcSink under the ifdef (pulls <grpcpp/grpcpp.h>)
    #include "sys1.proj.h"              // declares routingmeta::ProjectMeta(const sys1::v1::CalculateRequest&, MetadataSink&)
    int main() {
    #ifdef ROUTINGMETA_WITH_GRPC
      if (false) {                       // compile-only: never runs, no live channel/server (HR4)
        grpc::ClientContext ctx;
        routingmeta::GrpcSink sink(&ctx);
        sys1::v1::CalculateRequest req;
        ProjectMeta(req, sink);          // UNQUALIFIED -> ADL resolves routingmeta::ProjectMeta via the GrpcSink arg
      }
    #endif
      return 0;                          // flag OFF: trivial, harmless TU
    }
    ```
  - [x] The point is the **unqualified** `ProjectMeta(req, sink)` call: `GrpcSink` derives from `routingmeta::MetadataSink`, so ADL on its `routingmeta::` associated namespace must find the generated `routingmeta::ProjectMeta`. This is the exact ADL path the design depends on but that NO translation unit exercised before (deferred-work.md, Story-1.3 review). Match the file's banner-comment style.
  - [x] Do NOT link gRPC. The smoke is **compile-only** (`-fsyntax-only` / `-c`), so it needs gRPC + protobuf HEADERS but no gRPC lib and no server.

- [x] **Task 2 — Author `.github/workflows/ci.yml` (AC: 1, 2, 3)** — NEW file at the repo root `.github/workflows/ci.yml`
  - [x] `strategy.matrix`: `cxx: [g++, clang++]` × `protobuf: ['3.20.3', '3.21.12']` (4 cells); `fail-fast: false`; pin the runner to `ubuntu-22.04` (reproducible cache key + apt set), NOT `ubuntu-latest`.
  - [x] **Protobuf from source at the pinned tag, cached on the tag (AC1):** `actions/cache@v4` with `path: <job-local prefix>` (e.g. `${{ github.workspace }}/pb-${{ matrix.protobuf }}`) and `key: protobuf-${{ matrix.protobuf }}-ubuntu-22.04`. On cache miss, `git clone --depth 1 --branch v${{ matrix.protobuf }} https://github.com/protocolbuffers/protobuf`, then build+install into the prefix with `-Dprotobuf_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON`. **CMake source dir differs by version:** 3.20.3 keeps `CMakeLists.txt` under `protobuf/cmake/` (use `-S protobuf/cmake`); 3.21.x moved it to the repo root (use `-S protobuf`). Pick the dir that exists (`[ -f protobuf/CMakeLists.txt ] && S=protobuf || S=protobuf/cmake`).
  - [x] **Per-cell toolchain selection via the env knobs Story 1.8 made portable** — no build.sh edit needed: `CXX=${{ matrix.cxx }}`, `PROTOC=<prefix>/bin/protoc`, `PKG_CONFIG_PATH=<prefix>/lib/pkgconfig` (so `pkg-config --variable=includedir/libdir protobuf` resolves the cell's prefix), and `LD_LIBRARY_PATH=<prefix>/lib` (belt-and-suspenders for the self-check runtime; build.sh already bakes the rpath to `$PB_LIBDIR`).
  - [x] **Build + negative gate (AC2):** one step, `working-directory: grpc-routing-meta/example`, `run: ./build.sh`. This single command does the plugin build, per-system codegen, the `[neg ]` negative-codegen gate (aborts non-zero on a regression — free in CI), and links all four binaries. Do NOT re-implement the gate in YAML.
  - [x] **Self-checks (AC2):** a step running `./build/unified_sender`, `./build/receiver_verify`, `./build/test_projection`, `./build/bench_projection` — these are the BRIEF "Verify" lines; each exits non-zero on failure (`test_projection`→`ALL TESTS PASSED`, `bench_projection`→`BENCH PASSED`/exit 0).
  - [x] **gRPC compile-smoke (AC2/HR4):** a step installing gRPC headers (`sudo apt-get install -y libgrpc++-dev`) and compiling the Task-1 TU `-fsyntax-only`: `"$CXX" -std=c++17 -fsyntax-only -DROUTINGMETA_WITH_GRPC -I build/generated -I src -I "$(pkg-config --variable=includedir protobuf)" $(pkg-config --cflags grpc++) tests/grpc_smoke.cc`. Put the cell's protobuf `-I` BEFORE the grpc cflags so the pinned protobuf headers win over apt's. (See the coupling note in Dev Notes — if a cell's protobuf headers clash with apt-grpc's expectation, restrict the smoke to a representative cell.)
  - [x] **No-push reality (AC3):** add a top-of-file comment stating the workflow is authored to mirror the locally-verifiable steps and is validated by local-path equivalence (the workspace forbids pushing to any remote, so it cannot be green-validated here). Triggers may be `push`/`pull_request`/`workflow_dispatch` for correctness, but they never fire in this workspace.

- [x] **Task 3 — Local-path-equivalence verification (AC: 3)** — prove the mirrored steps run locally; document the mapping
  - [x] Run the per-cell sequence locally on the dev host (protobuf 3.20.3 via `PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig`): `cd grpc-routing-meta/example && ./build.sh` → green (plugin + negative gate + 4 binaries); then the four self-checks (`test_projection`→ALL TESTS PASSED, `bench_projection`→BENCH PASSED exit 0, `receiver_verify`→digest OK, `unified_sender`→3 blocks). This is the same command sequence the workflow's build + self-check steps run.
  - [x] **gRPC smoke locally:** `grpc++` IS present on this dev host (anaconda, 1.46.1, built against protobuf 3.20.x). Compile the Task-1 TU exactly as CI will: `"$CXX" -std=c++17 -fsyntax-only -DROUTINGMETA_WITH_GRPC -I build/generated -I src -I "$(pkg-config --variable=includedir protobuf)" $(pkg-config --cflags grpc++) tests/grpc_smoke.cc` → must succeed (exit 0). This is a REAL local verification of the HR4 deliverable, not just inspection.
  - [x] **YAML validity:** confirm `.github/workflows/ci.yml` parses (e.g. `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml'))"`).
  - [x] **Document the equivalence mapping** (in the story's Dev Agent Record / Completion Notes): each CI step ↔ the exact local command it mirrors. State plainly the inherent limit: the OTHER cells (clang, protobuf 3.21.12) and the from-source protobuf build cannot be executed here (no remote run, Linux-only); they are encoded via the same env-override mechanism the dev-host cell proves. That limit IS the AC3 contract (local-path equivalence, not a remote green run).
  - [x] Do NOT push, do NOT add a remote, do NOT attempt to trigger the workflow.

## Dev Notes

### Method (Amelia)
There is no failing unit test to write first; the discipline here is **proof-by-local-equivalence**. (1) Write the gRPC smoke TU and prove it compiles locally with `-DROUTINGMETA_WITH_GRPC` (red→green: without the `if(false){…ProjectMeta(req,sink)…}` body the ADL path is unproven; with it, a real compile confirms ADL resolves). (2) Author the workflow so every step is a literal mirror of a command you ran locally. (3) "Green" = build.sh + 4 self-checks + the gRPC smoke all pass locally, AND the YAML parses, AND the step↔command mapping is documented. The matrix cells you cannot run (clang, 3.21.12, from-source protobuf) are validated by construction — they differ only in the env knobs Story 1.8 made portable.

### Why `build.sh` is the per-cell driver (not CMake)
`build.sh` does, in one command: plugin build + per-system codegen + the `[neg ]` negative-codegen gate + linking all four binaries. CMake (`enable_testing()`/ctest) builds only `unified_sender`/`receiver_verify`/`test_projection`, has **no** negative-codegen gate, and **no** `bench_projection` target (the CMake↔build.sh parity gap deferred from 1.7/1.8). So driving CI from `build.sh` gets AC2's "build + negative gate + bench" for free; CMake would miss two of them. (BRIEF criterion A still wants both `build.sh` and CMake to *work* — that is satisfied by Story 1.8 — but *proving the matrix* is done via `build.sh`.)

### Protobuf 3.20.3 / 3.21.12 must be built from source (why AD-14 says so)
A given Ubuntu release ships exactly ONE `libprotobuf-dev` from apt — you cannot get both 3.20.3 AND 3.21.12 (let alone those exact patch levels) from the distro. Hence AD-14's "build from source at a pinned tag into a job-local prefix, cached on the tag." These versions PREDATE protobuf's Abseil hard-dependency (introduced in protobuf 22.x / 4.x), so a shared (`BUILD_SHARED_LIBS=ON`) build resolves transitive deps via `NEEDED` — no extra link flags, matching the kit's `-L … -lprotobuf` link (Story 1.8 review, dismissed F3). The cell points `PROTOC`/`PKG_CONFIG_PATH` at the prefix; `build.sh` does the rest unchanged.

### gRPC compile-smoke (HR4) — the design + the coupling risk
- HR4 wants the `GrpcSink` adapter (`src/common/metadata_sink.h:60-74`, guarded by `#ifdef ROUTINGMETA_WITH_GRPC`, wraps `grpc::ClientContext::AddMetadata`) proven to still COMPILE — "compile only, not a live server." The adapter's surface is tiny (a `ClientContext*` + `AddMetadata`), so a header-only `-fsyntax-only` check is sufficient and needs no gRPC lib, no link, no server.
- The smoke also resolves the **Story-1.3 deferred ADL item**: nothing ever called `ProjectMeta(req, grpcSink)`, so the namespaced-ADL resolution the design relies on was untested. The Task-1 TU's `if(false){ … ProjectMeta(req, sink); }` exercises exactly that at compile time.
- **Coupling risk (call it out, don't pretend it away):** apt `libgrpc++-dev` was built against the distro's protobuf, while the cell uses a from-source 3.20.3/3.21.12. For a `-fsyntax-only` check of the minimal GrpcSink surface this is almost always fine (grpc++ public headers don't hard-pin a protobuf patch; `ProjectMeta` only touches the generated message + `sink.Add`). Put the cell's `-I "$(pkg-config --variable=includedir protobuf)"` BEFORE the grpc cflags so the pinned protobuf headers win. If a cell ever fails to compile the smoke purely from this header clash, the fallback is to run the gRPC smoke in ONE representative cell (e.g. `g++` × `3.21.12`) rather than all four — AC2 says "each matrix job," so prefer all-cells, but document the fallback. On the dev host this risk is absent (anaconda grpc++ 1.46.1 was built against protobuf 3.20.x, matching the host's 3.20.3).

### Current state of the things this story touches (read before editing)
- **`.github/`** — does NOT exist. Greenfield; create `.github/workflows/ci.yml` at the REPO ROOT (`/Users/johnson.chiang/workspace/ab-bmad`), the only place GitHub Actions reads workflows. (In-workspace; the existing untracked `.claude/` shows root-level tool dirs are fine.)
- **`example/build.sh`** — post-1.8: `PROTOC="${PROTOC:-protoc}"`, `CXX="${CXX:-c++}"`, protobuf via `pkg-config --variable=includedir/libdir`, fail-loud guard, `[neg ]` gate, four binaries, rpath baked to `$PB_LIBDIR`. **Do NOT edit** — the matrix drives it purely by env (`CXX`/`PROTOC`/`PKG_CONFIG_PATH`).
- **`example/src/common/metadata_sink.h:58-74`** — the `GrpcSink` class under `#ifdef ROUTINGMETA_WITH_GRPC`; `#include <grpcpp/grpcpp.h>` is inside the ifdef. `ROUTINGMETA_WITH_GRPC` is referenced only here + `CMakeLists.txt:75,78,81` + `README.md:60`. No `.cc` uses it today.
- **`example/CMakeLists.txt`** — has the optional gRPC path (`find_package(gRPC CONFIG QUIET)` + `gRPC::grpc++`), but we do NOT use CMake's gRPC LINK for the smoke (linking pulls the version coupling in; `-fsyntax-only` avoids it). Leave CMake untouched.

### What must be preserved (system still works end-to-end)
- **No change to the build product (CR1/AD-9):** this story adds CI + a compile-only TU. It does NOT touch `build.sh`, the plugin, generated code, kit headers, or any wire output. The four binaries and their self-checks are byte-identical.
- **The negative gate stays the CI hard-gate** (criterion G): it runs inside `build.sh`; CI inherits it. Don't duplicate or weaken it.
- **The bench's mean-budget gate is noise-robust** (Story 1.7): in CI it runs and its 1 ms mean-per-call budget applies; do NOT tighten it to a per-call/p99 gate (would flake on shared runners).
- **No new runtime deps (NFR3/AD-11):** gRPC stays OPTIONAL (compile-smoke only, behind the flag). The kit's normal build needs no gRPC. CI installs gRPC headers only for the smoke step.

### Guardrails (do NOT do in this story)
- Do NOT edit `build.sh`, the plugin, generated code, kit headers, sender, receiver, or `test_projection`/`bench_projection`. New files only: `.github/workflows/ci.yml` + `tests/grpc_smoke.cc`.
- Do NOT push, add a remote, or try to trigger/green the workflow. AC3 is local-path equivalence by inspection + local runs.
- Do NOT link gRPC or stand up a server — the smoke is `-fsyntax-only` (compile only, HR4).
- Do NOT add the `bench_projection`/negative-gate CMake parity here (deferred to 1.15). Do NOT make CMake the CI driver.
- Do NOT hardcode a developer/anaconda path into the workflow (it runs on Linux runners with from-source protobuf prefixes).

### Reference shape (workflow, illustrative — not prescriptive)
```yaml
# CI matrix (AD-14). NOTE: the workspace forbids pushing to any remote, so this
# workflow is validated by local-path equivalence (it mirrors the exact commands
# run locally per cell), NOT by a remote green run.
name: ci
on: [push, pull_request, workflow_dispatch]
jobs:
  matrix:
    runs-on: ubuntu-22.04
    strategy:
      fail-fast: false
      matrix:
        cxx: [g++, clang++]
        protobuf: ['3.20.3', '3.21.12']
    env:
      PREFIX: ${{ github.workspace }}/pb-${{ matrix.protobuf }}
      CXX: ${{ matrix.cxx }}
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y build-essential clang pkg-config cmake git libgrpc++-dev
      - id: pbcache
        uses: actions/cache@v4
        with: { path: '${{ env.PREFIX }}', key: 'protobuf-${{ matrix.protobuf }}-ubuntu-22.04' }
      - if: steps.pbcache.outputs.cache-hit != 'true'
        run: |
          git clone --depth 1 --branch v${{ matrix.protobuf }} https://github.com/protocolbuffers/protobuf
          [ -f protobuf/CMakeLists.txt ] && S=protobuf || S=protobuf/cmake
          cmake -S "$S" -B protobuf/build -DCMAKE_INSTALL_PREFIX="$PREFIX" \
            -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
          cmake --build protobuf/build -j --target install
      - name: build + negative gate + binaries
        working-directory: grpc-routing-meta/example
        env:
          PROTOC: ${{ env.PREFIX }}/bin/protoc
          PKG_CONFIG_PATH: ${{ env.PREFIX }}/lib/pkgconfig
          LD_LIBRARY_PATH: ${{ env.PREFIX }}/lib
        run: ./build.sh
      - name: self-checks
        working-directory: grpc-routing-meta/example
        env: { LD_LIBRARY_PATH: '${{ env.PREFIX }}/lib' }
        run: ./build/unified_sender && ./build/receiver_verify && ./build/test_projection && ./build/bench_projection
      - name: gRPC compile-smoke (HR4)
        working-directory: grpc-routing-meta/example
        env: { PKG_CONFIG_PATH: '${{ env.PREFIX }}/lib/pkgconfig' }
        run: |
          "$CXX" -std=c++17 -fsyntax-only -DROUTINGMETA_WITH_GRPC \
            -I build/generated -I src -I "$(pkg-config --variable=includedir protobuf)" \
            $(pkg-config --cflags grpc++) tests/grpc_smoke.cc
```

### Testing standards
- No unit-test changes. The "tests" are: the local re-run of `build.sh` + the four binary self-checks (existing self-checks, unchanged), and a real local `-fsyntax-only` compile of `tests/grpc_smoke.cc` with `-DROUTINGMETA_WITH_GRPC` (the HR4 proof). The CI YAML must parse. The equivalence mapping (CI step ↔ local command) is the AC3 artifact.

### Project Structure Notes
- NEW: `.github/workflows/ci.yml` (repo root) and `grpc-routing-meta/example/tests/grpc_smoke.cc`. No edits to existing code/build files. The smoke TU resolves the Story-1.3 deferred ADL item — update `deferred-work.md` to mark it RESOLVED.

### Previous story intelligence (Stories 1.1–1.8)
- 1.8 made `build.sh` portable via `CXX`/`PROTOC` env + `pkg-config --variable=includedir/libdir`; its Dev Notes explicitly frame that as "what ENABLES the matrix" — 1.9 is the consumer. 1.8's High finding (stock-Linux `--cflags` stripping) is already fixed, and AD-14's from-source prefix is non-system anyway. 1.7's bench is run in CI but its mean-budget gate must stay (noise-robust). 1.3 deferred the `GrpcSink`+ADL instantiation to THIS story (HR4) — the Task-1 TU closes it. The CMake bench/negative-gate parity gap (1.7/1.8) stays deferred (1.15) and is the reason CI drives from `build.sh`, not CMake.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.9 (lines 278-296)] — user story + 3 ACs.
- [Source: epics.md:51 (HR4)] — compile-smoke the `ROUTINGMETA_WITH_GRPC` GrpcSink adapter in CI (compile only — not a live server).
- [Source: ARCHITECTURE-SPINE.md#AD-14] — GitHub Actions, Linux × {gcc,clang} × {protobuf 3.20.3, 3.21.12}, each protobuf built from source at a pinned tag into a job-local prefix cached on the tag; each job runs build + negative gate + binaries + bench + gRPC smoke; NO push → validated by local-path equivalence.
- [Source: ARCHITECTURE-SPINE.md#AD-13] — `build.sh` env-override + pkg-config discovery is the mechanism each cell uses.
- [Source: refs/BRIEF.md#B + Verify] — matrix criterion + the exact local steps to mirror.
- [Source: grpc-routing-meta/example/build.sh] — the per-cell driver (build + negative gate + 4 binaries); honors `CXX`/`PROTOC`/`PKG_CONFIG_PATH`.
- [Source: grpc-routing-meta/example/src/common/metadata_sink.h:58-74] — `GrpcSink` under `#ifdef ROUTINGMETA_WITH_GRPC`; the ADL target.
- [Source: _bmad-output/implementation-artifacts/deferred-work.md (Story-1.3 item)] — "Instantiate the GrpcSink + ADL path → Story 1.9 (HR4)"; the Task-1 one-liner is quoted there.

### Latest tech notes
No external research needed. GitHub Actions matrix + `actions/cache@v4` + from-source protobuf at a pinned tag is the standard pattern AD-14 names. protobuf 3.20.3 uses `cmake/` as the CMake source dir; 3.21.x moved it to the repo root — handle both. `-fsyntax-only` is the canonical compile-smoke (parse + semantic-check, no codegen/link), exactly right for HR4. gRPC stays optional (NFR3/AD-11). C++17.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer) — research + engineering delegated to general-purpose subagents; main loop authored the story, independently re-verified (smoke compile both ways, YAML parse + matrix assertion, scope), and did BMad bookkeeping.

### Debug Log References

- Research + implementation delegated to subagents (per user request); main loop independently re-ran the load-bearing checks before marking review.
- gRPC smoke (independently re-run, dev host, grpc++ 1.46.1): `c++ -std=c++17 -fsyntax-only -DROUTINGMETA_WITH_GRPC -I build/generated -I src -I "$(pkg-config --variable=includedir protobuf)" $(pkg-config --cflags grpc++) tests/grpc_smoke.cc` → **exit 0** (one benign `-Wdeprecated` from grpc++'s own `auth_context.h`; no `-Werror`). WITHOUT the flag (no grpc on the line) → **exit 0** (trivial main). Confirms the ADL path resolves *and* the TU is harmless when the flag is off.
- YAML (independently parsed): `.github/workflows/ci.yml` → `YAML OK`; matrix asserts `cxx=['g++','clang++']` × `protobuf=['3.20.3','3.21.12']` = 4 cells; steps = checkout → install deps → cache protobuf → build-protobuf-from-source(on miss) → `build.sh` → 4 self-checks → gRPC smoke.
- No-extra-include needed: `sys1.proj.h` transitively pulls `sys1.pb.h` (the `CalculateRequest` type resolves).
- Scope: `git status --short` → only `.github/` and `grpc-routing-meta/example/tests/grpc_smoke.cc` new; `build.sh`/plugin/generated/kit/CMake untouched.

### Completion Notes List

- **AC1** — `.github/workflows/ci.yml` defines `runs-on: ubuntu-22.04`, `matrix: cxx∈{g++,clang++} × protobuf∈{3.20.3,3.21.12}`, `fail-fast: false`; each protobuf built from source at `--branch v<ver>` into a job-local prefix `pb-<ver>`, cached via `actions/cache@v4` keyed `protobuf-<ver>-ubuntu-22.04`. Handles the 3.20-`cmake/` vs 3.21-root CMakeLists location.
- **AC2** — each job runs `./build.sh` (= plugin build + per-system codegen + `[neg ]` negative-codegen gate + links all four binaries), then the four self-checks (`unified_sender`/`receiver_verify`/`test_projection`/`bench_projection`), then the `-fsyntax-only -DROUTINGMETA_WITH_GRPC` gRPC compile-smoke of `tests/grpc_smoke.cc`. Per-cell toolchain selected purely via `CXX`/`PROTOC`/`PKG_CONFIG_PATH` (Story 1.8's portability) — no `build.sh` edit.
- **AC3** — top-of-file comment states the no-push / local-path-equivalence contract. Proven locally: `build.sh` + 4 self-checks green (protobuf 3.20.3), and a **real** `-fsyntax-only` smoke compile (grpc++ present). Inherent limit (the AC3 contract, not a gap): the clang and 3.21.12 cells and the from-source protobuf build cannot run here (no remote, Linux-only); they are encoded by the same env-override mechanism the dev-host cell proves.
- **HR4 / 1.3 deferral** — `tests/grpc_smoke.cc` does the guarded `if(false){ grpc::ClientContext ctx; routingmeta::GrpcSink sink(&ctx); … ProjectMeta(req, sink); }` with the call UNQUALIFIED, exercising the namespaced-ADL resolution no TU touched before. Marked RESOLVED in `deferred-work.md`.
- **Scope held / CR1·AD-9** — new files only; `build.sh`/plugin/generated/kit headers/CMake untouched; wire byte-identical. gRPC stays optional (compile-smoke only, behind the flag) — no new runtime dep (NFR3/AD-11).

**Local-path-equivalence mapping (AC3 artifact):**
| CI step | local command (dev host, protobuf 3.20.3) | result |
|---|---|---|
| build + negative gate + binaries | `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig ./build.sh` | plugin + `[neg ]` ×3 rejected + 4 binaries, `OK ->` |
| self-checks | `./build/{unified_sender,receiver_verify,test_projection,bench_projection}` | ALL TESTS PASSED · BENCH PASSED(0) · digest OK · 3 blocks |
| gRPC compile-smoke (HR4) | `c++ -std=c++17 -fsyntax-only -DROUTINGMETA_WITH_GRPC -I build/generated -I src -I "$(pkg-config --variable=includedir protobuf)" $(pkg-config --cflags grpc++) tests/grpc_smoke.cc` | exit 0 |
| (matrix axes: clang++, protobuf 3.21.12, from-source build) | not runnable locally — encoded by the same `CXX`/`PROTOC`/`PKG_CONFIG_PATH` knobs | n/a (AC3 limit) |

### File List

- `.github/workflows/ci.yml` (NEW — GitHub Actions matrix Linux × {g++,clang++} × {protobuf 3.20.3, 3.21.12}, from-source-cached protobuf, per-cell `build.sh` + self-checks + gRPC `-fsyntax-only` compile-smoke)
- `grpc-routing-meta/example/tests/grpc_smoke.cc` (NEW — compile-only HR4 smoke: guarded `GrpcSink` + unqualified `ProjectMeta(req, sink)` ADL instantiation; resolves the Story-1.3 deferral)

## Change Log

- 2026-06-28 — Story 1.9 drafted (create-story; research delegated to a subagent, authored in the main loop): GitHub Actions matrix Linux × {gcc,clang} × {protobuf 3.20.3, 3.21.12} from-source-cached, each cell running `build.sh` (build + negative gate + 4 binaries) + self-checks + a `-fsyntax-only` gRPC `GrpcSink`/ADL compile-smoke (resolves the 1.3 deferral, HR4). CI driven by `build.sh` (CMake lacks the gate + bench). No push → AC3 proven by local-path equivalence (build.sh + self-checks + a real local gRPC smoke compile, since grpc++ is present on the dev host). New files only: `.github/workflows/ci.yml`, `tests/grpc_smoke.cc`.
- 2026-06-28 — Story 1.9 implemented (dev-story): NEW `.github/workflows/ci.yml` (4-cell matrix, from-source protobuf cached on tag, `build.sh` driver + self-checks + gRPC `-fsyntax-only` smoke) and `tests/grpc_smoke.cc` (guarded unqualified `ProjectMeta(req, GrpcSink)` ADL instantiation — resolves the 1.3 deferral). Verified locally: build + 4 self-checks green, real smoke compile exit 0 (and trivial without the flag), YAML parses + matrix asserts 4 cells. No `build.sh`/code/CMake edits; wire byte-identical. Engineering by subagents, independently re-verified in the main loop. Status → review.
