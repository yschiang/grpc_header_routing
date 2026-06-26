# Production-grade plan — grpc-routing-meta

Sharpened via `/grill-me`. Takes the kit from example → production. All forks decided.
Live execution status is in [`progress.md`](progress.md).

## Decisions locked

| Dimension | Decision | Consequence |
|---|---|---|
| Byte reach | All C++ | No cross-lang spec/vectors. KEEP hand-rolled url_encode/sha256 — harden tests, don't swap in OpenSSL. |
| Digest threat | Accidental drift only | No HMAC/keys. Digest = integrity, NOT security; fix docs. |
| Digest value | Independent deploys → keep | Justified by sender/verifier version-skew. Document the reason. |
| Failure | Report, don't dictate | `void` → `ProjResult{ok, issues[], duration}`; missing mask-id → issue + explicit `x-routing-error`, NO throw. |
| Observability | Returned result, not baked-in logger | Caller logs/metrics from `issues`; reads `duration` for tracing. |
| Packaging | Deferred | Build-hardening happens regardless. |
| Perf | Trace, don't tune | `duration` in result + micro-bench; ms-level bar, already met (µs). |

## The pivot API change

```cpp
struct Issue { enum Kind { MissingRequired, Overflow } kind; std::string key; };
struct ProjResult { bool ok = true; std::vector<Issue> issues; std::chrono::nanoseconds duration{}; };

template <class Req>
ProjResult Send(const Req&, const Runtime&, MetadataSink&);   // FillCommon + ProjectMeta, timed
```
Generated `ProjectMeta` (now in `namespace routingmeta`, found by ADL via the sink arg) returns `ProjResult`; missing `x-mask-id` → `Issue` + `x-routing-error: missing:x-mask-id` (no throw); overflow → non-blocking `Issue` alongside the existing `x-process-context-overflow` header.

## P0 — must-have
1. **Portable build** — kill hardcoded `/Users/johnson.chiang/anaconda3`; `find_package(Protobuf)`; `add_meta_proto()` CMake fn; validate `CMakeLists.txt` (never run here); `build.sh` = de-hardcoded fallback.
2. **CI** — GitHub Actions, Linux × {gcc, clang} × {protobuf 3.20, 3.21}; build + negative-codegen gate + 3 binaries.
3. **Failure-as-data** — `ProjResult` change; promote `Send` into kit; `EmitProcessContexts` reports overflow as issue; update tests + invariant 9.
4. **Perf tracing** — `duration` in result + bench printing per-call time for 1/2/25/60 contexts.
5. **Doc truth** — fix invariant 6 "tamper" → integrity-only; add version-skew rationale.

## P1 — recommended (all four, trimmed)
- **Crypto vectors** — SHA-256 boundary (empty, 55/56-byte edge, multi-block) + url round-trip fuzz.
- **Parser tests + policy** — negative no-crash cases + document lenient/dup-key-last-wins. Not a full fuzz rig.
- **Thread-safety** — document re-entrancy + one concurrent test.
- **GrpcSink in CI** — compile + smoke the `ROUTINGMETA_WITH_GRPC` adapter. Not a live server.

## P2 — note, don't build
Wire-contract `v1→v2` evolution doc; perf optimization only if a budget appears.

## Open decisions — ratify cross-team (Sender dept ↔ us)
1. Default failure policy: **abort** (recommended) vs proceed-on-error.
2. Who owns / consumes `x-routing-error` in APISIX (only if proceed-on-error).
3. `x-routing-error` header name + value format (freeze once agreed).

## Verification
CI green on the matrix · `unified_sender` shows `x-routing-error` + `duration` on an empty-mask-id sys3 call · `test_projection` `ALL TESTS PASSED` incl. new vectors/parser/thread · `receiver_verify` OK · bench sub-ms.

## Out of scope
HMAC/keys · cross-language · install/packaging (deferred) · SPC · spec edits.

## Method
- **Planning:** `/grill-me` (done).
- **Implementation:** TDD (superpowers) — RED → GREEN → REFACTOR per behavior change.

Size: P0 ≈ 5–6 md, P1 ≈ 2–3 md. Sequence: lead with **P0.3** (ProjResult) — plugin, sender, tests, docs all pivot on it.
