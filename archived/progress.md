# Progress — grpc-routing-meta production hardening

Living tracker for [`plan.md`](plan.md). Update as each step lands.

Legend: ☐ pending · ◐ in progress · ☑ done · ⊘ blocked

_Last updated: 2026-06-27_

## Phase status

| # | Phase | Status | Notes |
|---|---|---|---|
| P0.3 | Failure-as-data (ProjResult) | ◐ | scaffolding in (proj_result/send/emit); plugin behavior next (TDD) |
| P0.5 | Doc truth (digest + no-throw) | ◐ | SPEC.md + ADRs done; CONTEXT.md tamper/no-throw fixes pending |
| P0.4 | Perf tracing (duration + bench) | ☐ | duration plumbed via Send; bench pending |
| P0.1 | Portable build (kill anaconda path) | ☐ | |
| P0.2 | CI matrix (Linux × gcc/clang × pb 3.20/3.21) | ☐ | |
| P1 | Hardening (crypto/parser/threads/grpc) | ☐ | all four, trimmed |

## P0.3 — Failure-as-data (active, TDD)

Done:
- ☑ `grpc-routing-meta/example/src/common/proj_result.h` — `Issue{MissingRequired,Overflow}` + `ProjResult{ok,issues,duration}`
- ☑ `.../src/common/send.h` — `routingmeta::Send<>()` (FillCommon + ProjectMeta, timed)
- ☑ `.../src/common/process_context_emit.h` — `EmitProcessContexts(..., ProjResult&)` pushes `Overflow` issue

Next (RED → GREEN):
- ☐ RED: tests in `.../tests/test_projection.cc` — empty mask ⇒ `!ok` + `x-routing-error: missing:x-mask-id` + no `x-mask-id`; overflow ⇒ non-blocking `Overflow` issue, `ok` stays true; valid mask ⇒ `ok`, no issues
- ☐ GREEN: `.../src/plugin/protoc-gen-meta.cc` — generate `ProjResult ProjectMeta(...)` in `namespace routingmeta`; required-missing → issue + `x-routing-error`, no throw; pass `result` to `EmitProcessContexts`; drop `<stdexcept>`
- ☐ `.../sender/unified_sender.cc` — drop local `Send`, include `common/send.h`; capture result; demo `duration` + empty-mask `x-routing-error`
- ☐ `./build.sh` green: plugin compiles, negative gate passes, `ALL TESTS PASSED`, `receiver_verify` OK

## Design interview (grill-with-docs)

Resolving the failure/routing model branch-by-branch. Terms land in
`grpc-routing-meta/CONTEXT.md` glossary; hard-to-reverse decisions in
`grpc-routing-meta/docs/adr/`.

- ☐ **Q1 (open)** — routing model per system: what does APISIX route each system on?
  (determines which projection failures are *blocking* vs *non-blocking*)

## Open decisions (cross-team: Sender dept ↔ us)
- ☐ Default failure policy: abort (recommended) vs proceed-on-error
- ☐ APISIX consumer/owner of `x-routing-error`
- ☐ `x-routing-error` name + value format — freeze once agreed

## Docs

Product (in `grpc-routing-meta/`):
- `SPEC.md` — normative wire contract (supersedes `archive/02`)
- `CONTEXT.md` — project context + testable invariants (glossary references SPEC)
- `docs/adr/` — ADRs (0001 all-C++ · 0002 digest integrity · 0003 drop selectors)
- `example/TESTING.md` — run procedure

Process (repo root):
- `plan.md` — productionization plan
- `progress.md` — this tracker

## Log
- 2026-06-27 — Grilled scope → `plan.md`. Started P0.3: `proj_result.h`, `send.h`, overflow-issue in emit helper. Pivoted to TDD.
- 2026-06-27 — Authored `SPEC.md` (normative); superseded `archive/02`. Split: CONTEXT = project context, SPEC = normative.
- 2026-06-27 — Started `docs/adr/` (0001–0003). Began grill-with-docs design interview (Q1 open).
- 2026-06-27 — Moved `plan.md` + `progress.md` to the repo root (reverted a brief `_superpowers/` nesting per request).
