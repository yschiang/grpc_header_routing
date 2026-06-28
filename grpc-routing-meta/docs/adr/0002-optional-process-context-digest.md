# ADR 0002 — process-context digest is optional (send-time), verify-if-present

- **Status:** Accepted
- **Deviates from:** `refs/SPEC.md` §5.3 ("the sender MUST emit `x-process-context-digest` … The verifier MUST recompute and compare")
- **Date:** 2026-06-29

## Context

`refs/SPEC.md` §5.3 makes the digest mandatory whenever context lines are emitted, and makes
verification mandatory. The digest is an **integrity** check (not a security control). Some
callers don't want the extra ~100 bytes/request and the per-request hash; ops may want to shed
that cost from config without a rebuild; and a receiver fleet can be mid-rollout, where some
senders emit the digest and some don't. A hard MUST on both ends makes those impossible.

## Decision

Emission becomes a **send-time argument** `emit_digest` on `EmitProcessContexts`, threaded by
the codegen plugin into a uniform `ProjectMeta(req, sink, bool emit_digest = true)` for every
system. Default **on** = unchanged behavior; a caller passes `false` to omit the digest for that
request. Verification becomes **verify-if-present**: a present digest MUST still be recomputed
and compared (mismatch ⇒ reject); an **absent** digest is skipped and accepted — absence is NOT
treated as drift.

The overflow byte budget is unchanged: the digest header is **pre-charged** against the 7 KB
guard whether or not it is emitted, so the overflow decision is byte-for-byte identical either
way. Count / format / context lines are untouched.

### Why send-time over a compile-time switch

A compile-time macro would be smaller (no codegen/API change) but is per-binary only and forces
a two-build test matrix. The send-time argument lets a single binary decide per request and lets
operators toggle the digest from config without a rebuild, at the cost of one bool on the
generated `ProjectMeta` contract and a runtime branch. The kit chose the runtime knob.

## Consequences

- Every existing 2-arg call site (`ProjectMeta(req, sink)`, `EmitProcessContexts(sink, ctxs)`)
  is unchanged via the default — digest on, mandatory verification of the wire it produces.
- The generated `ProjectMeta` gains one uniform `bool emit_digest` param across all systems
  (unused on scalar-only systems; no `-Wextra`, so no warning).
- Receivers interoperate across a rollout: a digest-on sender is fully verified; a digest-off
  sender is accepted without verification.
- Cost: with the digest off, that request carries no integrity check, so drift from it is not
  caught at the receiver. Accepted as an explicit, per-call trade — the body remains
  authoritative and the request still routes on the common headers.
- One build covers both paths: `test_projection` exercises digest on and off in-process; no
  `-D` matrix.
