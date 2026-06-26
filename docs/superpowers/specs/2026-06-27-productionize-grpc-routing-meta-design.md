# Design — productionize grpc-routing-meta

**Date:** 2026-06-27 · **Status:** approved (design), pre-plan
**Definition of done:** `refs/BRIEF.md` criteria A–I + its Verify block.
**Locked decisions source:** `refs/plan.md` (grilling-session output), `refs/SPEC.md`, `refs/CONTEXT.md`.
**Scope chosen:** BRIEF A–I + the cheap P1 wins (thread-safety test, GrpcSink compile-smoke in CI). Heavier fuzz rigs deferred.

This is a *productionization* of a working example-grade kit, not a greenfield build. Most design
decisions are already locked by `refs/plan.md`; this doc transcribes them into a build order.

---

## 1. The API pivot — `ProjResult` (criterion C, plan.md P0.3)

Everything else hangs off this. New header `example/src/common/proj_result.h`:

```cpp
namespace routingmeta {
struct Issue { enum Kind { MissingRequired, Overflow } kind; std::string key; };
struct ProjResult { bool ok = true; std::vector<Issue> issues; std::chrono::nanoseconds duration{}; };
}
```

Behavior changes:

- **Generated `ProjectMeta` moves into `namespace routingmeta`** (resolved by ADL via the
  `routingmeta::MetadataSink&` argument), **returns `ProjResult`**, and **times its own body**
  with a single `std::chrono::steady_clock` pair around one return path (satisfies criterion H's
  "duration reported per call").
- **No throw on a data condition** (SPEC §7). A required scalar that is empty:
  1. `result.ok = false`
  2. push `Issue{MissingRequired, "x-mask-id"}`
  3. emit `x-routing-error: missing:x-mask-id`
  4. do **not** emit the empty `x-mask-id` header
- Optional scalar empty → omit header, no issue (unchanged).
- `EmitProcessContexts(MetadataSink&, const std::vector<std::string>&, ProjResult&)` — on overflow,
  push a **non-blocking** `Issue{Overflow}` (`ok` stays `true`) and still emit
  `x-process-context-overflow: true`. Non-overflow path unchanged.
- **`Send<>()` promoted into the kit** as `example/src/common/send.h`:
  ```cpp
  namespace routingmeta {
  template <class Req>
  ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
    FillCommon(rt, sink);
    return ProjectMeta(req, sink);   // ADL-resolved at instantiation
  }
  }
  ```
  Removed from `unified_sender.cc`. `Runtime` stays in `common_headers.h` (global).

Callers (`unified_sender`, `receiver_verify`, `test_projection`) still call `ProjectMeta(req, sink)`
unqualified — ADL via the sink argument finds `routingmeta::ProjectMeta`. `ProjResult` is **not**
`[[nodiscard]]`, so `receiver_verify` may ignore it cleanly.

## 2. Portable build (criterion A, plan.md P0.1)

- `example/build.sh`: remove the `PROTO_HOME=/Users/.../anaconda3` hardcode. Discover the toolchain
  via `pkg-config protobuf` when available; otherwise derive the install prefix from
  `$(command -v protoc)`. `PROTOC`, `CXX`, `CXXFLAGS` overridable by env. Keep the same list-driven
  structure (`CONTRACT` / `SYSTEMS`) and the negative-codegen gate. Add the bench binary.
- `example/CMakeLists.txt`: already uses `find_package(Protobuf REQUIRED)` — keep the working
  `foreach`. Add the `bench_projection` target. **Skipped:** the `add_meta_proto()` helper from
  plan.md — the existing foreach is already DRY across N systems; add only if duplication appears.
- cmake is not installed locally → CMake is validated by CI, not run here.

## 3. CI matrix (criterion B, plan.md P0.2)

`.github/workflows/ci.yml`:

- Matrix: `os: ubuntu` × `cc: {gcc, clang}` × `protobuf: {3.20.3, 3.21.12}`.
- Install protobuf from the pinned GitHub release tarball (deterministic across both versions).
- Each cell: `./build.sh` (includes the negative-codegen gate) **and** the cmake build; then run
  `unified_sender`, `receiver_verify`, `test_projection`, `bench_projection`.
- One extra cheap job: `apt-get install -y libgrpc++-dev`, then compile-smoke a TU that instantiates
  `routingmeta::GrpcSink` under `-DROUTINGMETA_WITH_GRPC` (no live server).
- **Cannot be run or pushed from this workspace** (macOS host, no-push rule). Authored to be
  green-on-push and verified by inspection.

## 4. Perf bench (criterion H, plan.md P0.4)

New `example/tests/bench_projection.cc`: builds requests with 1 / 2 / 25 / 60 contexts, calls
`ProjectMeta`, prints the per-call `duration` (averaged over a few iterations for stability), and
asserts sub-millisecond. Wired into `build.sh`, `CMakeLists.txt`, and CI.

## 5. Tests & invariants (criteria D–G; cheap P1)

`example/tests/test_projection.cc`:

- Rewrite the invariant-9 case: empty required mask → `ProjResult{ok=false}` with a `MissingRequired`
  issue + `x-routing-error: missing:x-mask-id` header + **no** `x-mask-id`, **no throw**.
- Overflow cases (count / bytes / line) → `ok=true` with an `Overflow` issue, plus the existing
  header asserts.
- Assert `result.duration > 0` on a normal projection.
- Keep every existing invariant assert (encoding, key-sort, dynamic count, digest round-trip + tamper,
  count=0, common-6-uniform, selectors absent).
- **+cheap P1:** one thread-safety test — project the same request concurrently from N threads into
  N separate sinks, assert identical output and digest (documents re-entrancy).

## 6. Doc truth (criterion I, plan.md P0.5)

Edit the **live** docs under `grpc-routing-meta/` (the `refs/` copies are read-only):

- `CONTEXT.md`: invariant 6 "tamper" → integrity-only (matches SPEC §5.3); invariant 9
  "ProjectMeta throws" → `ProjResult` issue + `x-routing-error`.
- `OVERVIEW.zh.md`: §4 table + §7 "Error handling" row "throw" → `ProjResult` + `x-routing-error`
  (no throw). The scored **benefit** claim ("錯誤在來源就擋住,不會 silent failure") is preserved;
  only the mechanism wording changes. Any "tamper" framing → integrity-only.
- `README.md`: ensure build instructions and any error-handling claims match the code; mention
  `duration`/bench if useful.

## 7. Build order (TDD, RED → GREEN → REFACTOR)

Lead with the ProjResult pivot — plugin, sender, tests, docs all hinge on it.

1. **ProjResult pivot** — write failing tests (ProjResult shape, `x-routing-error`, no-throw,
   overflow-as-issue, duration) → make green by adding `proj_result.h`, updating
   `process_context_emit.h`, the plugin, `send.h`, and the apps.
2. **Perf bench** — `bench_projection.cc` + wiring.
3. **Portable build** — de-hardcode `build.sh`; confirm green locally via env-pointed protoc.
4. **CI workflow** — author `.github/workflows/ci.yml` (+ GrpcSink smoke job).
5. **Doc truth** — update the three live docs.
6. **Cheap P1** — thread-safety test.

Build/CI/docs are infra: the build run and (on push) the CI run are their tests.

## 8. Verification (BRIEF Verify block)

```
cd grpc-routing-meta/example && ./build.sh    # plugin builds, codegen runs, negative gate passes, binaries link
./build/unified_sender                         # 3 system blocks; empty sys3 mask → x-routing-error + duration
./build/receiver_verify                        # digest OK
./build/test_projection                        # ALL TESTS PASSED (incl. ProjResult, thread-safety)
./build/bench_projection                       # sub-ms for 1/2/25/60 contexts
```
CI authored green on Linux × {gcc,clang} × {protobuf 3.20,3.21}.

## 9. Out of scope

HMAC/keys · cross-language byte spec · install/packaging · `add_meta_proto()` CMake helper ·
heavy SHA-256/url fuzz vectors · parser fuzz rig · spec edits · editing `refs/`.
