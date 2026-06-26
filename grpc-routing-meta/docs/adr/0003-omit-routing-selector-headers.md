# 0003. Sender omits the four routing-selector headers

Status: Accepted
Date: 2026-06-27

## Context

The original spec (`archive/02_grpc_routing_metadata_spec.md` §4–5, now superseded) listed
`x-target-system`, `x-transaction-type`, `x-route-profile`, and `x-routing-grain` as
sender-emitted headers. The kit's sender emits only the six common headers.

## Decision

**Do not emit** those four. `x-target-system` and `x-transaction-type` are already carried
by the gRPC method `:path` (APISIX routes on the path); `x-route-profile` and
`x-routing-grain` are APISIX *route configuration*, not sender data. Emitting them as
metadata is redundant and introduces a drift risk (two sources for one fact).

## Consequences

- **+** Smaller, non-redundant header set; impossible for a selector header to disagree
  with the `:path`.
- **−** The wire diverges from the original spec; a reader comparing against `archive/02`
  sees the gap. Mitigated by the SUPERSEDED banner on that file and by `SPEC.md` §2.
- Reversal is cheap (re-add to `FillCommon`) **if** a router ever needs a selector out of
  the path — but that would reintroduce the drift risk this decision removes.
