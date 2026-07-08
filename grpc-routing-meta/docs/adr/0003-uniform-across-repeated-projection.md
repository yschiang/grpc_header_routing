# ADR 0003 — `uniform_across_repeated`: verified-uniform scalar projection

- **Status:** Accepted
- **Extends:** SPEC §4/§7/§9 (a new, opt-in projection shape; no existing rule is relaxed by default)
- **Date:** 2026-07-08

## Context

A real sys3 batch shape exists that the v1 contract could not express: one request
carries `repeated Job jobs`, and **every job repeats the same mask id** — a fleet
convention, not something protobuf can state in the schema. The routing decision
needs that one value as `x-mask-id`, but:

- `(routing.project)` under a repeated field is **rejected** at codegen (SPEC §9,
  `bad_project_under_repeated.proto`) — deliberately, so an accidentally-misplaced
  tag cannot silently vanish.
- Changing the body (hoisting a single scalar out of the batch) is off the table:
  existing TXs must not change.
- Hand-extracting `jobs(0).mask_id()` in the Sender violates the one-sender rule
  (CONTEXT invariant 10: no per-system code, no hand-written header extraction).

## Decision

Add an **explicit opt-in** to `ProjectField`:

```proto
string mask_id = 1 [(routing.project) = {
  key: "x-mask-id", required: true, uniform_across_repeated: true
}];
```

Valid **only** on a string scalar that is a **direct field of exactly one repeated
message** (one level; a flag with no repeated ancestor, or below a second repeated
level, fails codegen — uniformity across a cross-product is undefined). The
generated `ProjectMeta`:

1. projects **element[0]**'s value to the header, and
2. **verifies every element agrees**. Divergence is a **blocking** issue:
   `ok=false`, `Issue::Inconsistent`, `x-routing-error: inconsistent:<key>`, and
   **no** scalar header — routing a batch on job[0]'s value while the rest disagree
   would misroute them (hard-fail decided over log-and-proceed).
3. An **empty** repeated field with `required: true` is `MissingRequired`, exactly
   like an empty scalar.

The default behavior is unchanged: an **unflagged** `(routing.project)` under a
repeated field is still rejected loudly. The flag is a promise made in the contract
proto — reviewed where the schema is reviewed — not a codegen default.

## Why not the alternatives

- **Relax the repeated-rejection rule and "take the first":** turns the guard rail
  off for everyone; an accidental tag under `repeated` would silently project
  element[0]. The whole point of the negative gate is that this class of mistake is
  a build error.
- **Sender-side extraction:** violates invariant 10, and the uniformity check would
  exist only where a team remembered to write it. In codegen, every adopter gets the
  verification for free (policy centralized, criterion F).
- **Project all N as repeated `x-mask-id` headers + count:** a much larger wire
  change, hard to route on in APISIX, and pointless when the values are identical
  by convention.

## Consequences

- The annotation makes a previously implicit fleet invariant ("all jobs carry the
  same mask") **checked on every send** and fail-loud on violation — new coverage,
  not just new syntax.
- `Issue` gains an `Inconsistent` kind; `x-routing-error` gains the value form
  `inconsistent:<key>` (same PROVISIONAL status as `missing:<key>`, SPEC §7).
- Wire bytes are unchanged for every existing message; the flag is build-time-only
  metadata (`FieldOptions` extension), so adopting it does not change field numbers
  or serialized bodies.
- Uniform keys share the single-header namespace with plain projected scalars:
  duplicate-key validation covers both collection paths.
- Fixtures: `bad_uniform_not_repeated.proto` (flag with no repeated ancestor) and
  `bad_uniform_nested_repeated.proto` (flag below a second repeated level) join the
  negative gate; `BatchSubmitRequest` in `sys3.proto` is the positive shape,
  asserted in `test_projection` (agree / diverge / empty).
