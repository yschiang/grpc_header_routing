# ADR 0001 — `Send` ownership stays in the Sender, not the kit

- **Status:** Accepted
- **Deviates from:** `refs/plan.md` P0.3 ("Failure-as-data … promote `Send` into kit")
- **Date:** 2026-06-29

## Context

`plan.md` P0.3 lists, as one consequence of the failure-as-data pivot, "promote
`Send` into the kit." `Send<>()` is the one-call wrapper that composes the two
real building blocks:

```cpp
template <class Req>
ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  FillCommon(rt, sink);            // 6 common headers
  return ProjectMeta(req, sink);   // generated body projection
}
```

The delivered kit does **not** export `Send`. It exports `FillCommon`,
`ProjectMeta`, `ProjResult`, and `MetadataSink`; `Send` lives in the sample
`sender/unified_sender.cc` and is documented as Sender-owned.

## Decision

Keep `Send` in the Sender department. The kit's authoritative surface is the two
building blocks plus the result/sink types. `Send` is an optional convenience
wrapper each sender team writes (or inlines) for itself.

## Why P0.3 is wrong on ownership grounds

P0.3 contradicts decisions `plan.md` itself locks, and the BRIEF/OVERVIEW framing:

1. **"Report, don't dictate" (plan.md, Failure row).** `Send` does two things
   beyond the building blocks: it *composes* `FillCommon`+`ProjectMeta`, and it
   *decides* what the result means (it is the natural seam for an abort-vs-proceed
   policy — see the still-open cross-team decision in `plan.md` "Open decisions" #1).
   Both are orchestration. Baking the composition order and the policy seam into
   the kit is the kit dictating caller behavior — exactly what this row forbids.

2. **"Observability is the caller's" (plan.md, Observability row).** The result of
   `Send` (`ok`, `issues`, `duration`) is consumed for logging/metrics/abort by the
   caller. The thing that reads `ProjResult` and acts on it belongs to the same
   department that owns observability — the Sender.

3. **BRIEF criterion E accepts either surface.** Criterion E ("One sender path")
   is satisfied by "`Send<>()` **/** `FillCommon`+`ProjectMeta` serve sys1/sys2/sys3
   with zero `if (system==…)`." The two-call surface already meets E with no
   per-system branching; `Send` is not required to satisfy any acceptance criterion.

4. **OVERVIEW frames `Send` as optional.** OVERVIEW describes the sender adding the
   two building blocks "(或包成 `Send`)" — *or wrap them as `Send`* — i.e. `Send` is
   explicitly an optional packaging, not the kit's contract.

Promoting `Send` into the kit would also force the kit to own `Runtime` and the
`FillCommon` ordering as public contract, widening the authoritative surface for
no benefit the building blocks don't already provide.

## Consequences

- Kit API is the two building blocks + `ProjResult` + `MetadataSink`. Documented as
  the "Wiring contract" in `README.md`.
- `sender/unified_sender.cc`'s `Send` is marked **SAMPLE wiring**: illustrative, not
  a kit guarantee.
- The abort-vs-proceed policy (plan.md Open decision #1) lands in the Sender's `Send`,
  where it belongs, without a kit change.
- Cost of the deviation: each sender team writes a ~3-line wrapper (or inlines the two
  calls). Accepted — that wrapper is where their own orchestration/policy lives anyway.
