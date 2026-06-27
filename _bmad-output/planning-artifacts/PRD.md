# Product Requirements Document — Productionize `grpc-routing-meta`

| Field | Value |
|---|---|
| Project | `gprc_header_routing` (kit: `grpc-routing-meta`) |
| Method | BMAD (Team B) |
| Phase | 2 — Planning |
| Document owner | John (PM) |
| Status | Draft → for readiness review |
| Authoritative "done" | `refs/BRIEF.md` (acceptance criteria A–I + Verify block) |
| Locked design decisions | `refs/plan.md` (supersedes `OVERVIEW.zh.md` on the failure model) |
| Normative wire contract | `refs/SPEC.md` (wins on wire bytes) |
| Glossary + invariants | `refs/CONTEXT.md` (10 testable invariants) |
| Benefit claims scored | `refs/OVERVIEW.zh.md` §7「還有什麼好處」 + §10 總比較表 |

---

## 1. Goals and Background Context

### 1.1 Background

`grpc-routing-meta` is a C++ kit that performs **body-authoritative header projection**:
the protobuf request body is the single source of truth, and every routing header is an
exact projection *of* the body, generated at the sender by a `protoc` plugin
(`--meta_out` → `ProjectMeta()`). APISIX routes on the headers without parsing the body,
and a receiver can recompute a digest to prove the headers never drifted from the body.

The kit exists and runs today at **example grade**: three systems (sys1/sys2/sys3, 16
transactions), a `unified_sender`, a `receiver_verify`, and `test_projection`. It builds
via a hand-written `build.sh` and a CMake file.

The benefit claims the project is scored on are enumerated in `OVERVIEW.zh.md` §7 and §10:
Effort, governance/docs, schedule, **error handling (never silent)**, quality/consistency
(no drift), single sender (no divergence), maintainability (centralized policy),
decoupling, onboarding, **testability**.

### 1.2 The gap

The kit demonstrates the design but does not yet *prove* it at production grade. Concretely:

1. The build is **not portable** — `build.sh` hardcodes a developer-specific anaconda path.
2. There is **no CI** proving the kit on a real toolchain matrix.
3. The failure model is **example-grade**: a missing required scalar **throws**, which the
   locked plan (`plan.md`) explicitly replaces with failure-as-data
   (`ProjResult` + `x-routing-error`). The "caught at source, never silent" benefit must
   hold without a throw.
4. **No performance evidence** exists for the "sub-ms" / observability claim.
5. **Docs carry stale claims** ("throw", digest "tamper") that contradict the locked design.

### 1.3 Goals

| ID | Goal | BRIEF criterion |
|---|---|---|
| G-A | Portable build: no hardcoded toolchain path; `find_package(Protobuf)`; both `build.sh` and CMake build on a stock Linux toolchain | A |
| G-B | Proven on a matrix: GitHub Actions green on Linux × {gcc, clang} × {protobuf 3.20, 3.21} — build + negative-codegen gate + binaries + tests | B |
| G-C | No silent failure: three gates *before the wire* (build / sender / overflow) plus the receiver digest gate, all explicit and observable | C |
| G-D | Exact projection, no drift: digest round-trips; canonical key-sorted encoding; `/`→`%2F` | D |
| G-E | One sender path: `Send<>()` (= `FillCommon` + `ProjectMeta`) serves sys1/sys2/sys3 with zero `if (system==…)`, and lives in the kit | E |
| G-F | Policy centralized: the 7168 / 25 / 512 limits live in exactly one place; exactly one plugin | F |
| G-G | Testable invariants: every `CONTEXT.md` invariant has an assert; codegen negative tests run in CI | G |
| G-H | Perf observed: `duration` reported per call; a micro-bench prints time for 1/2/25/60 contexts; sub-ms | H |
| G-I | Docs match code: no stale claim (digest "tamper" → integrity-only; "throw" → ProjResult) | I |

### 1.4 Success metrics

The four commands below are the **BRIEF-literal Verify block** and must pass verbatim:

```
cd grpc-routing-meta/example && ./build.sh   # plugin builds, codegen runs, binaries link, negative gate green
./build/unified_sender                        # 3 system blocks; empty sys3 mask → x-routing-error + duration
./build/receiver_verify                       # digest OK
./build/test_projection                       # ALL TESTS PASSED
```

Promoted to an explicit success gate (criterion-H evidence; **beyond** the BRIEF-literal block):

```
./build/bench_projection                      # sub-ms for 1/2/25/60 contexts
```
Plus: the CI workflow (criterion B) is correct on the matrix; the negative-codegen gate
fails the build when an annotation is malformed.

### 1.5 Change log

| Date | Version | Description | Author |
|---|---|---|---|
| 2026-06-27 | 0.1 | Initial PRD from BRIEF + plan.md (locked decisions) | John (PM) |
| 2026-06-27 | 0.2 | Phase-tightening: solutioning deferred to architecture (CR2, §4, §6); strict plan.md P1 scope added (§3.4); bench labeled criterion-H evidence (§1.4) | John (PM) |

---

## 2. Users and stakeholders

This is platform/infrastructure software; "users" are roles in the adoption chain, not
end consumers. There is **no UI**, so no UX workflow applies.

| Role | Cares about | Touchpoint |
|---|---|---|
| Platform / contract team | one plugin + shared contracts; centralized policy; codegen correctness | `protoc-gen-meta`, `metadata_options.proto`, `process_context.proto`, `process_context_emit.h` |
| System provider (sys1/sys2/sys3) | annotate proto only; build-time rejection of bad annotations | `sysN.proto`, negative-codegen gate |
| Sender / client developer | two lines, no per-system code; errors surfaced as data | `Send<>()`, `ProjResult` |
| Gateway (APISIX) | route on small headers; never parse body; explicit overflow flag | header set, `x-process-context-overflow` |
| Backend / receiver | prove header↔body consistency | `VerifyDigest`, `receiver_verify` |
| Build / release engineer | portable build + green CI on a real matrix | `build.sh`, `CMakeLists.txt`, `.github/workflows` |

---

## 3. Requirements

### 3.1 Functional Requirements

- **FR1 — Failure-as-data (the pivot).** `ProjectMeta` and `Send` MUST NOT throw on a data
  condition. They return `routingmeta::ProjResult { bool ok; std::vector<Issue> issues;
  std::chrono::nanoseconds duration; }`. (`refs/plan.md` "pivot API change"; supersedes
  `OVERVIEW` §4 "throw".)
- **FR2 — Missing required scalar.** When a `required` `(routing.project)` scalar (today:
  sys3 `x-mask-id`) is empty, `ProjectMeta` MUST: (a) record an `Issue{MissingRequired,
  "x-mask-id"}` with `ok=false`; (b) emit `x-routing-error: missing:x-mask-id`; (c) NOT
  emit the empty scalar header. No throw. (BRIEF C; SPEC §7.)
- **FR3 — Overflow as non-blocking data.** When projecting would exceed any of: total kit
  metadata > 7168 B, count > 25, or one context value > 512 B — `ProjectMeta` MUST emit
  `x-process-context-overflow: true`, suppress context lines and the digest, still emit
  `count` + `format`, AND record a non-blocking `Issue{Overflow}` while leaving `ok=true`.
  (BRIEF C; SPEC §5.4; plan.md P0.3.)
- **FR4 — Build-time gate (fail loud).** The plugin MUST reject, with non-zero protoc exit:
  `(routing.project)` not on a non-repeated scalar leaf (repeated, message-typed, or under a
  repeated field) and duplicate projected keys. (SPEC §9; CONTEXT inv. 9.) *(present — keep)*
- **FR5 — Receiver gate.** The receiver MUST recompute `x-process-context-digest` over the
  canonical (`\n`-joined) contexts and reject on mismatch. (SPEC §5.3.) *(present — keep)*
- **FR6 — One sender path in the kit.** `Send` MUST be a single template, defined in the
  kit (not the demo app), that calls `FillCommon` then `ProjectMeta`, with the generated
  `ProjectMeta` overload selected by request type (ADL) and **zero** per-system branching.
  (BRIEF E; CONTEXT inv. 10.)
- **FR7 — Perf trace.** Every `ProjResult.duration` MUST be populated with the measured
  projection time. A `bench_projection` binary MUST print per-call time for 1/2/25/60
  contexts and assert each is sub-millisecond. (BRIEF H; plan.md P0.4.)
- **FR8 — Canonical projection (regression-guarded).** Per-context value = `(routing.pctx)`
  fields key-sorted (`ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech`),
  `&`-joined, each URL-encoded (RFC 3986 unreserved verbatim, else `%XX` uppercase, space
  `%20`); empty field → `Key=`. Digest = `sha256:` + hex over `\n`-joined contexts. (SPEC
  §5–6; CONTEXT inv. 1,4,6.) *(present — keep + harden tests)*

### 3.2 Non-Functional Requirements

- **NFR1 — Portability.** No machine-specific paths. CMake uses `find_package(Protobuf)`.
  `build.sh` discovers the toolchain (env override → derive from `protoc` location). Builds
  on a stock Linux toolchain with gcc or clang.
- **NFR2 — Proven matrix.** CI builds + tests on Linux × {gcc, clang} × {protobuf 3.20,
  3.21}, runs the negative-codegen gate, and runs all three binaries plus the bench.
- **NFR3 — No new runtime dependencies.** Keep the hand-rolled `url_encode` and `sha256`
  (harden their tests, do not swap in OpenSSL). gRPC stays optional. (plan.md "Byte reach".)
- **NFR4 — Determinism.** Canonical encoding is byte-identical run-to-run; digest is
  reproducible; key order is fixed.
- **NFR5 — Thread-safety.** `ProjectMeta` is pure (reads the request, writes a per-call
  sink; no shared mutable state) and therefore re-entrant. Documented as an invariant.
- **NFR6 — Centralized policy.** The three numeric limits and the digest/overflow policy
  live only in `process_context_emit.h`; there is exactly one codegen plugin.
- **NFR7 — Observability without coupling.** The kit performs no logging or metrics itself;
  callers read `issues` (logs/metrics) and `duration` (tracing) from the returned result.
  (plan.md "Observability".)

### 3.3 Compatibility requirements (brownfield)

- **CR1 — Wire contract unchanged.** No SPEC byte changes. The only *new* header is
  `x-routing-error` (already provisional in SPEC §7/§2), emitted only on a missing required
  scalar. Digest, encoding, overflow, count/format semantics are unchanged.
- **CR2 — Projection API surface evolves additively.** The generated projection entry point
  changes from returning nothing to returning a structured result (ok / issues / duration);
  source compatibility is preserved for callers that ignore it. The precise type placement
  and overload-resolution mechanism are an architecture decision (phase 3).
- **CR3 — `refs/` is read-only.** Live docs to update are the `grpc-routing-meta/` copies
  (`CONTEXT.md`, `OVERVIEW.zh.md`, `README.md`); `refs/` and `SPEC.md` are not edited.

### 3.4 Hardening requirements (plan.md P1 — all four, trimmed)

Per `refs/plan.md` P1 ("recommended — all four, trimmed"), these ship in minimal form
(fidelity to the locked plan; this is a methodology A/B run):

- **HR1 — Crypto/encoding vectors.** SHA-256 known-answer vectors (empty, 55/56-byte
  boundary, multi-block) + URL-encode round-trip across reserved / space / high-byte. *(not a fuzz rig)*
- **HR2 — Parser robustness.** Receiver-side negative no-crash cases (a malformed `%`-escape
  is passed through literally per SPEC §6); document lenient parse + dup-key-last-wins. *(not a full fuzz rig)*
- **HR3 — Thread-safety.** Document `ProjectMeta` re-entrancy (NFR5) + one concurrent test.
- **HR4 — gRPC adapter in CI.** Compile-smoke the `ROUTINGMETA_WITH_GRPC` GrpcSink adapter
  in CI. *(compile only — not a live server)*

---

## 4. Technical assumptions

- **Repository:** single workspace; work only inside it; local commits, no push.
- **Language/toolchain:** C++17; Protobuf with `libprotoc` (for the plugin); gRPC optional
  (`ROUTINGMETA_WITH_GRPC`). Local box has protoc 3.20.3 (anaconda) and Apple clang only;
  CMake is absent locally → CMake correctness is validated in CI, not locally (plan.md).
- **Testing:** in-tree `assert`-based test binary (no framework), per plan.md "TDD
  (superpowers) — RED → GREEN → REFACTOR per behavior change". Codegen negative tests are
  proto fixtures asserted by `build.sh` and CI.
- **CI provider:** GitHub Actions. CI MUST pin protobuf 3.20 **and** 3.21 deterministically
  across gcc and clang; the pinning mechanism (package source / container image) is an
  architecture decision (phase 3).

---

## 5. Epic list (detail in `epics-and-stories.md`)

| Epic | Title | Primary BRIEF criteria |
|---|---|---|
| E1 | Failure-as-data pivot + one sender path | C, E, (F) |
| E2 | Perf observability | H |
| E3 | Portable build + CI matrix | A, B (+ HR4) |
| E4 | Testable-invariants hardening | G, D (+ HR1–HR3) |
| E5 | Documentation truth | I |

**Sequencing rationale (plan.md):** lead with the `ProjResult` pivot (E1) — the plugin,
sender, tests, and docs all pivot on it. Perf (E2) depends on `ProjResult.duration`.
Build/CI (E3) must accommodate the new bench binary. Tests (E4) and docs (E5) follow the
code. F (centralized policy) is already satisfied and is *ratified*, not rebuilt.

---

## 6. Risks and mitigations (product / scope level)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| CI cannot be literally "green" without a push (workspace rule forbids push) | High | Med | Author the workflow to mirror exactly the steps verified locally; verify the local toolchain path end-to-end |
| Scope creep beyond plan.md (into P2, or gold-plating P1) | Med | Low | Lock scope to P0 + plan.md's four trimmed P1 items (§3.4); P2 explicitly excluded (§7) |
| Wire-contract regression while refactoring the failure model | Low | High | CR1: no SPEC byte changes; digest / encoding / overflow / count semantics asserted unchanged by tests |

Technical / architecture risks (overload resolution, toolchain ABI, build mechanics) are
owned by the architecture document (phase 3), not this PRD.

---

## 7. Out of scope

HMAC / keys / signatures (digest is integrity, not security); cross-language byte spec or
vectors; install / packaging; `v1→v2` wire evolution; SPC; any edit to `refs/` or `SPEC.md`.
plan.md **P2** is excluded: the `v1→v2` evolution doc and speculative perf tuning. (plan.md
P1 hardening is **in** scope — see §3.4.)
