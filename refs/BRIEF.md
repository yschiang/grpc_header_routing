# BRIEF — productionize grpc-routing-meta (shared, methodology-neutral)

Both teams build the **same thing** from the **same inputs**. This file is the
definition of done. What differs between teams is *how you plan and build* — the
target does not.

## What you're given

- `grpc-routing-meta/` — a working **example-grade** kit: body-authoritative
  header projection via a `protoc` plugin (`--meta_out` → `ProjectMeta()`).
  3 systems (sys1/sys2/sys3), a `unified_sender`, a `receiver_verify`, and
  `test_projection`. It builds and runs today.
- `refs/` — read-only reference inputs:
  - **`OVERVIEW.zh.md`** — the value proposition and **the benefit claims you are
    scored on** (§7 「還有什麼好處」 and §10 總比較表).
  - **`SPEC.md`** — normative wire contract (RFC2119). On wire bytes, SPEC wins.
  - **`CONTEXT.md`** — glossary + 10 testable invariants.
  - **`plan.md`** — locked design *decisions* from a grilling session (decisions,
    not an implementation plan). On *mechanism* (esp. the failure model) plan.md
    supersedes OVERVIEW — see criterion C below.

## Your job

Take the prototype to **production grade** so that the build **demonstrably
delivers every benefit claimed in `refs/OVERVIEW.zh.md` §7 and §10** — at
production grade, not example grade. "Production grade" is the gap between *works
on my machine* and *the benefit is actually true and proven*.

## Definition of done (acceptance criteria)

- **A. Portable build.** No hardcoded `/Users/.../anaconda3`. `find_package(Protobuf)`.
  Both `./build.sh` and CMake work on a stock Linux toolchain.
- **B. Proven on a matrix.** GitHub Actions green on Linux × {gcc, clang} ×
  {protobuf 3.20, 3.21}: build + negative-codegen gate + binaries + tests.
- **C. No silent failure** (OVERVIEW §4 / benefit「Error handling」). Three gates,
  all *before* the wire:
  - *build:* bad annotation (dup key, or `(routing.project)` under `repeated`) →
    `protoc --meta_out` **fails**.
  - *sender:* missing `required` scalar (sys3 `x-mask-id`) → surfaced explicitly.
    **plan.md supersedes OVERVIEW's "throw": realize this as
    `ProjResult{ok, issues[], duration}` + an explicit
    `x-routing-error: missing:x-mask-id` header — NO throw.** The benefit
    ("caught at source, never silent") must hold.
  - *overflow:* >7168 B total / >25 contexts / >512 B line → explicit
    `x-process-context-overflow: true` (non-blocking).
  - *receiver:* `x-process-context-digest` recompute + compare → reject on mismatch.
- **D. Exact projection, no drift** (benefit「Quality/一致性」). Digest round-trips;
  canonical key-sorted encoding; `/`→`%2F`.
- **E. One sender path** (benefit「不分歧」). `Send<>()` / `FillCommon`+`ProjectMeta`
  serve sys1/sys2/sys3 with zero `if (system==…)`.
- **F. Policy centralized** (benefit「好維護」). 7168 / 25 / 512 live in one place;
  one plugin.
- **G. Testable invariants** (benefit「可測」). Each CONTEXT.md invariant has an
  assert; codegen negative tests run in CI.
- **H. Perf observed** (plan.md P0.4). `duration` reported per call; a micro-bench
  prints time for 1/2/25/60 contexts; sub-ms.
- **I. Docs match code.** No stale claim (digest "tamper" → integrity-only;
  "throw" → ProjResult).

## Verify (must pass)

```
cd grpc-routing-meta/example && ./build.sh   # plugin builds, codegen runs, binaries link
./build/unified_sender                        # 3 system blocks; empty sys3 mask → x-routing-error + duration
./build/receiver_verify                       # digest OK
./build/test_projection                       # ALL TESTS PASSED
# negative gate: a bad-annotation proto fails codegen (build.sh asserts this)
```

## Rules

- Work **only inside this workspace**. Do not read outside it. Do not look for, or
  at, the other team's work.
- `refs/` is read-only — don't edit it.
- Local git commits encouraged (they show your process). **Do not push to any
  remote.** No co-author trailer; concise messages.
