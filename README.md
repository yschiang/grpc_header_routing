# Handoff — gRPC Routing Metadata Design + Kit

Two parts: the **design docs** (the contract, for EA review) and the **header kit** (the sender-side implementation that produces the metadata).

## Start here

| Read | What it is | Audience |
|---|---|---|
| `design-docs/EA_DESIGN_SUMMARY.md` | One-page overview of the whole design + the decisions to endorse | EA / leadership |
| `design-docs/00_design_assumptions.md` | The premises the design rests on (grain, batching, routing dimensions) | EA / architect |
| `design-docs/01_generic_process_domain_model.md` | Business/domain semantics (Lot, Wafer, Tool, Chamber, Recipe Rev, Process Execution) | Domain / data |
| `design-docs/02_grpc_routing_metadata_spec.md` | The routing-metadata contract (header layers, process-context projection, digest, overflow) | Integration / platform |
| `header-kit/README.md` | The codegen kit that implements §02 sender-side; reviewed separately | Sender engineers |

## How the two parts relate

```
design-docs/  = the contract (authority)
   02 spec defines metadata keys, layers, overflow, digest rule
        ▲
        │ implements (references, never redefines)
        │
header-kit/   = sender-side generator
   reads proto options -> generates ApplyMeta() that emits the metadata
```

The core principle across both: **the body is the single source of truth; headers are exact, body-derived projections that cannot drift.**

## Status

- Design docs: four-piece set, internally cross-referenced, no dead links.
- Kit: verified end-to-end against protobuf 3.21.12; output matches spec §8.1; digest, overflow, and build-time guards all confirmed.

## Open items (carried in 00 + EA summary)

OPEN-A overflow thresholds · OPEN-B digest consumer · OPEN-C de-batch under overflow · OPEN-D APISIX header-to-variable naming · OPEN-E Layer 4 reserved.
