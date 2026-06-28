---
baseline_commit: a6ecfe13d3ffa5b1771e87d83324e96514a10780
---

# Story 1.8: Portable build (no machine-specific paths)

Status: review

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a build/release engineer,
I want the build to work on a stock Linux toolchain with no hardcoded paths,
so that anyone can build the kit.

## Acceptance Criteria

1. **AC1 — `build.sh` has no machine-specific paths; uses env overrides + pkg-config.** Given `build.sh`, when inspected, then it contains no hardcoded anaconda/developer path; it uses `PROTOC` (default `protoc` on PATH) and `CXX` (default `c++`) env overrides, and derives protobuf include/lib flags from `pkg-config protobuf`, appending `-lprotoc` for the plugin. (AD-13)

2. **AC2 — `CMakeLists.txt` uses `find_package(Protobuf)`, no absolute toolchain paths.** Given `CMakeLists.txt`, when inspected, then it uses `find_package(Protobuf)` with no absolute toolchain paths. (NFR1)

3. **AC3 — The build still works (plugin, codegen, link, negative gate).** Given `cd grpc-routing-meta/example && ./build.sh` on a stock toolchain, when run, then the plugin builds, codegen runs, binaries link, and the negative gate is green. (BRIEF Verify line 1)

## Tasks / Subtasks

- [x] **Task 1 — Replace the hardcoded toolchain block in `build.sh` with env + pkg-config (AC: 1)** — edit `example/build.sh`
  - [x] Delete the `PROTO_HOME=/Users/johnson.chiang/anaconda3` block (`build.sh:9-12`: `PROTO_HOME` / `PROTOC` / `PB_INC` / `PB_LIB`). NO `/Users/...` or `anaconda` literal may remain anywhere in the file.
  - [x] `PROTOC="${PROTOC:-protoc}"` (already overridable; just drop the `$PROTO_HOME/bin` prefix so the default is `protoc` on PATH). `CXX="${CXX:-c++}"` is already present — keep it.
  - [x] Derive protobuf flags from pkg-config, failing loud if absent (matches the kit's "fail loud, never silent" contract):
    `pkg-config --exists protobuf || { echo "protobuf not found via pkg-config: install libprotobuf-dev / protobuf-devel, or set PKG_CONFIG_PATH=<prefix>/lib/pkgconfig"; exit 1; }`
  - [x] Capture once: `PB_CFLAGS=$(pkg-config --cflags protobuf)` (the `-I<inc>`), `PB_LDIRS=$(pkg-config --libs-only-L protobuf)` (the `-L<lib>`), `PB_LIBDIR=$(pkg-config --variable=libdir protobuf)` (for rpath).
  - [x] Rewrite the three flag sites that referenced `$PB_INC`/`$PB_LIB`:
    - `IPROTO=(-I proto $PB_CFLAGS)` — protoc needs the protobuf include dir for the well-known `google/protobuf/*.proto` that `metadata_options.proto` imports.
    - Plugin link (`build.sh:42-45`): `... -I "$GEN" $PB_CFLAGS $PB_LDIRS -lprotoc -lprotobuf -Wl,-rpath,"$PB_LIBDIR" ...` — order `-lprotoc` BEFORE `-lprotobuf` (libprotoc depends on libprotobuf; left-to-right resolution). libprotoc has no `.pc`, so it is added by hand on the same `-L` as protobuf.
    - `PBFLAGS=(-I "$GEN" -I "$ROOT/src" $PB_CFLAGS $PB_LDIRS -lprotobuf -Wl,-rpath,"$PB_LIBDIR")` — apps/test/bench link only `-lprotobuf`. KEEP `-I "$GEN"` and `-I "$ROOT/src"` (project include dirs, not protobuf — unrelated to portability).
  - [x] Leave the list-driven structure (CONTRACT / SYSTEMS / GEN_SRCS), the negative-codegen gate, the `[bench]` line (Story 1.7), and the final `echo OK` UNCHANGED. This is a toolchain-discovery swap only — no change to what is built or linked.

- [x] **Task 2 — Verify `CMakeLists.txt` already satisfies AC2; do not regress it (AC: 2)** — inspect `example/CMakeLists.txt`
  - [x] Confirm by inspection: it uses `find_package(Protobuf REQUIRED)` (`:13`), `${Protobuf_PROTOC_EXECUTABLE}` (`:26`), `${Protobuf_INCLUDE_DIRS}`/`${Protobuf_LIBRARIES}`, and `find_library(PROTOC_LIB ...)` with relative HINTS (`:16-17`) — **no absolute `/Users`/anaconda path**. AC2 is already met by the existing file.
  - [x] Therefore NO CMake edit is required for AC2. Do not add absolute paths. (If a trivially-safe consistency nit surfaces, prefer leaving CMake untouched — the AC is inspection-only and already green.)
  - [x] NOTE (do NOT fix here): `CMakeLists.txt` does not build `bench_projection` (added to `build.sh` only, Story 1.7). That is a build.sh↔CMake parity gap, not a portability defect, and is out of 1.8's ACs — log it to `deferred-work.md`, do not scope-creep it into this story.

- [x] **Task 3 — Build & verify on this dev host, with the portability caveat documented (AC: 3)**
  - [x] This conda dev host has `protoc` on PATH (anaconda's, 3.20.3) but `pkg-config` does NOT find protobuf on its default path (no `protobuf.pc` exported). The `.pc` exists at `…/anaconda3/lib/pkgconfig/protobuf.pc`. So verify with: `cd grpc-routing-meta/example && PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig ./build.sh`. (On a stock Linux box with `libprotobuf-dev`, `protobuf.pc` is on the default path and plain `./build.sh` works — that is the AC3 "stock toolchain" target. Setting `PKG_CONFIG_PATH` is the conda-host workaround, NOT something build.sh should hardcode — hardcoding it would re-introduce a machine-specific path and fail AC1.)
  - [x] Expect: `protoc --version` prints (libprotoc 3.20.3), plugin builds, per-system codegen runs, `[neg ]` negative-codegen gate prints `ok (rejected)` for every fixture and the build does NOT abort, all four binaries (`unified_sender`/`receiver_verify`/`test_projection`/`bench_projection`) link, final `OK -> binaries`.
  - [x] Regression: `./build/test_projection` → `ALL TESTS PASSED`; `./build/bench_projection` → `BENCH PASSED` (exit 0); `./build/receiver_verify` → digest OK; `./build/unified_sender` → 3 system blocks. Wire output byte-identical (this story changes only HOW the toolchain is found, not WHAT is compiled — CR1/AD-9).
  - [x] Grep proof for AC1: `grep -nE "anaconda|/Users/|PROTO_HOME" build.sh` → no matches; `grep -n "pkg-config" build.sh` → present.

## Dev Notes

### Method (Amelia)
Red → green is awkward for a build-config story (there is no failing unit test to write first). The equivalent discipline: (1) confirm the CURRENT `build.sh` only builds because of the hardcoded anaconda paths (it would fail on a box without that exact prefix) — that is the "red" the story removes; (2) make the swap; (3) "green" = `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig ./build.sh` reproduces the identical successful build (plugin + codegen + negative gate + 4 binaries) with zero machine-specific literals in the file, and all four binaries still pass their self-checks. The proof obligation is "same build result, no hardcoded paths."

### The exact swap (toolchain discovery only — nothing about WHAT is built changes)
- **Before** (`build.sh:9-12,21-22,42-44`): `PROTO_HOME=/Users/johnson.chiang/anaconda3` → `PROTOC=$PROTO_HOME/bin/protoc`, `PB_INC=$PROTO_HOME/include`, `PB_LIB=$PROTO_HOME/lib`; flags hardcode `-I "$PB_INC" -L "$PB_LIB" -Wl,-rpath,"$PB_LIB"`.
- **After**: `PROTOC="${PROTOC:-protoc}"`, `CXX="${CXX:-c++}"`; `pkg-config --exists protobuf` guard; `PB_CFLAGS=$(pkg-config --cflags protobuf)`, `PB_LDIRS=$(pkg-config --libs-only-L protobuf)`, `PB_LIBDIR=$(pkg-config --variable=libdir protobuf)`; substitute those into `IPROTO`, the plugin link, and `PBFLAGS`. Add `-lprotoc` by hand for the plugin (no `.pc`), ordered before `-lprotobuf`.
- This is purely how the compiler/headers/libs are located. The proto list, codegen invocations, link sets (`GEN_SRCS`), negative gate, and the four output binaries are untouched. Hence the wire/byte output is identical (CR1/AD-9).

### pkg-config output on this host (verified during story creation)
```
$ PKG_CONFIG_PATH=/Users/johnson.chiang/anaconda3/lib/pkgconfig pkg-config --cflags --libs protobuf
-I/Users/johnson.chiang/anaconda3/include -L/Users/johnson.chiang/anaconda3/lib -lprotobuf
$ … --variable=libdir protobuf  →  /Users/johnson.chiang/anaconda3/lib
$ … --modversion protobuf        →  3.20.3
$ which protoc                    →  /Users/johnson.chiang/anaconda3/bin/protoc  (on PATH)
```
protobuf 3.20.3 is one of the pinned CI-matrix versions (AD-14) — the dev-host build mirrors a CI cell.

### Why `PKG_CONFIG_PATH` must NOT go into `build.sh`
The conda host needs `PKG_CONFIG_PATH=<prefix>/lib/pkgconfig` for `pkg-config` to find protobuf. That is an ENVIRONMENT concern the operator sets — baking the anaconda prefix into `build.sh` would re-introduce exactly the machine-specific literal AC1 forbids and break portability on a stock box. build.sh stays env-agnostic: it calls `pkg-config protobuf` and fails loud (with a hint to set `PKG_CONFIG_PATH`) if the toolchain is not discoverable. Stock Linux (`apt install libprotobuf-dev protobuf-compiler`) puts `protobuf.pc` on the default path → plain `./build.sh` works.

### Current state of the files this story touches (read before editing)
- **`example/build.sh`** — list-driven manual build mirroring CMake. Machine-specific literals at `:9-12` (`PROTO_HOME`/`PROTOC`/`PB_INC`/`PB_LIB`); flag arrays `IPROTO` (`:21`) and `PBFLAGS` (`:22`) and the plugin link (`:42-44`) consume `$PB_INC`/`$PB_LIB`. `CXX="${CXX:-c++}"` already at `:14`. The `[bench]` line is at `:81-82` (Story 1.7). The negative-codegen gate (`:47-66`) must stay green.
- **`example/CMakeLists.txt`** — ALREADY portable: `find_package(Protobuf REQUIRED)` (`:13`), `Protobuf_PROTOC_EXECUTABLE` (`:26`), `Protobuf_INCLUDE_DIRS`/`Protobuf_LIBRARIES`, `find_library(PROTOC_LIB … HINTS …)` (`:16-17`, relative hints only). No absolute paths. Optional gRPC via `find_package(gRPC CONFIG QUIET)` behind `ROUTINGMETA_WITH_GRPC`. No edit needed for AC2. (Lacks a `bench_projection` target — deferred, see below.)

### What must be preserved (system still works end-to-end)
- **Same build product (CR1/AD-9):** identical generated code, identical link sets, identical four binaries, byte-identical wire output. Only toolchain discovery changes.
- **Negative-codegen gate stays green (criterion G):** the `[neg ]` loop must still reject every `tests/negative/*.proto` (build aborts if any is accepted). Don't disturb it.
- **No new deps (NFR3/AD-11):** `pkg-config` is a standard build tool present on every stock toolchain (and on this host: `/usr/local/bin/pkg-config`). It is not a runtime dependency of the kit.
- **`CXX`/`PROTOC` overrides keep working:** the CI matrix (AD-14) builds Linux × {gcc, clang} × {protobuf 3.20.3, 3.21.12} by setting `CXX` and pointing `pkg-config`/`PROTOC` at the chosen protobuf — the env-override design is what makes that matrix possible.

### Guardrails (do NOT do in this story)
- Do NOT hardcode ANY absolute path (including `PKG_CONFIG_PATH`) into `build.sh` or `CMakeLists.txt`.
- Do NOT change which protos are compiled, the codegen commands, the link sets, the negative gate, or any generated/kit/app code. Toolchain-discovery swap ONLY.
- Do NOT add `bench_projection` to `CMakeLists.txt` here (1.7 parity gap → deferred-work, not this story's ACs).
- Do NOT add a new dependency or a build-system framework. Plain pkg-config + env overrides.
- Do NOT "fix" CMake — AC2 is inspection-only and already satisfied; touching it risks regressing a green AC.

### Deferred / out-of-scope (log, don't do)
- **CMake lacks a `bench_projection` target** — `build.sh` builds it (Story 1.7) but `CMakeLists.txt` builds only `test_projection`. build.sh↔CMake parity gap; not a portability defect. Append to `deferred-work.md` under a 1.8 heading; a future story (or a one-line CMake add) can close it.

### Testing standards
- No unit test changes. The "test" is the build itself: `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig ./build.sh` succeeds end-to-end (plugin + codegen + negative gate + 4 binaries), then the four binaries pass their existing self-checks (`test_projection` → ALL TESTS PASSED, `bench_projection` → BENCH PASSED, `receiver_verify` → digest OK, `unified_sender` → 3 blocks). Plus the grep proofs for AC1.

### Project Structure Notes
- Edit: `example/build.sh` (toolchain block + 3 flag sites). Inspect-only: `example/CMakeLists.txt` (already compliant). No code/generated/test changes. One `deferred-work.md` append.

### Previous story intelligence (Stories 1.1–1.7)
- 1.7 added the `[bench]` line to `build.sh` and a `bench_projection` binary — keep that line working through the swap (it uses `PBFLAGS`, which is being rewritten, so re-verify the bench still links and passes). The earlier stories (1.1–1.6) only touched `src/`, generated code, and tests — none touched `build.sh`'s toolchain block, so this is the first portability change. The CMake file was authored portable from the start (find_package), so the divergence is only in `build.sh`.

### References
- [Source: _bmad-output/planning-artifacts/epics.md#Story 1.8] — user story + 3 ACs.
- [Source: refs/BRIEF.md#A] — "Portable build. No hardcoded `/Users/.../anaconda3`. `find_package(Protobuf)`. Both `./build.sh` and CMake work on a stock Linux toolchain."
- [Source: refs/BRIEF.md#Verify] — line 1: `cd grpc-routing-meta/example && ./build.sh` (plugin builds, codegen runs, binaries link).
- [Source: ARCHITECTURE-SPINE.md#AD-13] — toolchain portability: env overrides (`PROTOC`/`CXX`) + pkg-config discovery; no machine-specific paths.
- [Source: ARCHITECTURE-SPINE.md#AD-14] — pinned CI matrix Linux × {gcc,clang} × {protobuf 3.20.3, 3.21.12}; the env-override design enables it.
- [Source: ARCHITECTURE-SPINE.md#NFR1] — CMake uses `find_package(Protobuf)`, no absolute toolchain paths.
- [Source: grpc-routing-meta/example/build.sh:9-12,21-22,42-44] — the hardcoded toolchain block + the three flag sites to rewrite.
- [Source: grpc-routing-meta/example/CMakeLists.txt:13,16-17,26] — already portable (find_package + relative HINTS).

### Latest tech notes
No external research. `pkg-config protobuf` is the canonical way to locate protobuf headers/libs on a stock toolchain; `--libs-only-L` isolates the `-L` so `-lprotoc -lprotobuf` link order stays correct (libprotoc has no `.pc` and ships alongside libprotobuf). protobuf 3.20.3 on this host; CMake `find_package(Protobuf)` is the stock module. C++17. No new runtime deps.

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Amelia / Senior Software Engineer) — engineering delegated to a general-purpose subagent; main loop did create-story, independent verification (re-ran the build + 4 binaries + greps + fail-loud guard), and BMad bookkeeping.

### Debug Log References

- Implementation delegated to a subagent (per user request); main loop independently re-ran everything before marking review.
- Green (independently re-run in main loop, `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig ./build.sh`): `protoc libprotoc 3.20.3`, plugin builds, `[neg ]` gate prints `ok (rejected)` for all 3 fixtures and does NOT abort, all four binaries link, `OK -> binaries`. Self-checks: `test_projection` → `ALL TESTS PASSED`; `bench_projection` → `BENCH PASSED` exit 0; `receiver_verify` → `digest check: OK (header matches body)`; `unified_sender` → 3 system blocks.
- Red/fail-loud proof (main loop): with `PKG_CONFIG_PATH` unset, `./build.sh` prints `protobuf not found via pkg-config: install libprotobuf-dev / protobuf-devel, or set PKG_CONFIG_PATH=<prefix>/lib/pkgconfig` and exits **1** — no silent fallback to a stale path.
- AC1 grep (boundary-corrected — see note): `grep -nE "anaconda|/Users/|PROTO_HOME|PB_INC|PB_LIB([^D]|$)" build.sh` → **no matches**; `grep -n pkg-config build.sh` → present.
- Scope: `git diff --stat` shows exactly `build.sh` changed in the code dir; CMake, generated code, kit, tests untouched.

### Completion Notes List

- **AC1** — `build.sh` drops the `PROTO_HOME=/Users/.../anaconda3` block: `PROTOC="${PROTOC:-protoc}"`, `CXX="${CXX:-c++}"`, a `pkg-config --exists protobuf` fail-loud guard, and `PB_CFLAGS`/`PB_LDIRS`/`PB_LIBDIR` from `pkg-config` feeding `IPROTO`, the plugin link (`-lprotoc -lprotobuf`, protoc first), and `PBFLAGS`. No machine-specific literal remains.
- **AC2** — `CMakeLists.txt` already uses `find_package(Protobuf REQUIRED)` + `Protobuf_PROTOC_EXECUTABLE`/`Protobuf_INCLUDE_DIRS`/`Protobuf_LIBRARIES` + `find_library(PROTOC_LIB … HINTS …)` with no absolute paths. Inspection-only; left untouched (touching a green AC risks regressing it).
- **AC3** — full build verified green on this host (plugin + codegen + negative gate + 4 binaries), all self-checks pass, wire byte-identical (toolchain-discovery swap only — CR1/AD-9). protobuf 3.20.3 = a pinned CI-matrix cell (AD-14).
- **AC1 grep nuance:** the new var `PB_LIBDIR` contains `PB_LIB` as a substring, so a naïve `…|PB_LIB` pattern self-matches it. The real proof uses a word-boundary-corrected pattern `PB_LIB([^D]|$)` (empty) — confirming zero leftover `PB_INC`/`PB_LIB`/`PROTO_HOME`/anaconda literals. (The story's own Task-3 grep `anaconda|/Users/|PROTO_HOME` is also empty.)
- **Scope held:** only `build.sh` changed; no protos/codegen/link-sets/negative-gate/code touched; `PKG_CONFIG_PATH` deliberately NOT hardcoded (the operator sets it; baking it in would re-break AC1). CMake↔build.sh `bench_projection` parity gap logged to `deferred-work.md`, not scoped in.

### File List

- `grpc-routing-meta/example/build.sh` (MODIFIED — toolchain block replaced with `PROTOC`/`CXX` env overrides + `pkg-config protobuf` discovery and fail-loud guard; 3 flag sites rewritten; everything else byte-identical)

## Change Log

- 2026-06-28 — Story 1.8 drafted (create-story): make `build.sh` toolchain-portable (drop hardcoded anaconda paths; `PROTOC`/`CXX` env overrides + `pkg-config protobuf` for flags, `-lprotoc` for the plugin); `CMakeLists.txt` already satisfies AC2 (find_package, no absolute paths) — inspection only. Verify on this conda host via `PKG_CONFIG_PATH=…/anaconda3/lib/pkgconfig`. CMake↔build.sh `bench_projection` parity gap deferred.
- 2026-06-28 — Story 1.8 implemented (dev-story): `build.sh` toolchain block replaced with `PROTOC`/`CXX` env overrides + `pkg-config protobuf` discovery (`--cflags`/`--libs-only-L`/`--variable=libdir`) and a fail-loud guard; 3 flag sites rewritten; `-lprotoc` added by hand (no `.pc`), ordered before `-lprotobuf`. CMake unchanged (already compliant). Green build + 4 binary self-checks verified; fail-loud guard exits 1 when protobuf undiscoverable; zero machine-specific literals (boundary-corrected grep). Wire byte-identical (CR1/AD-9). Engineering by a subagent, independently re-verified in the main loop. Status → review.
