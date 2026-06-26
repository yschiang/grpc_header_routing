# Design Summary for EA Review — gRPC Routing to RMS / SPC / APC

> For Enterprise Architecture review · Covers three documents: `00_design_assumptions`, `01_generic_process_domain_model`, `02_grpc_routing_metadata_spec`. Implementation tooling (codegen kit) is reviewed separately and is out of scope here.

---

## 1. What this is

A contract for routing transactions from Tool / EAP / DCS clients, through **APISIX**, to backend **RMS / SPC / APC** services — **4 services, ~30 transaction methods** (migrating from legacy CORBA TX). The gateway routes on **gRPC metadata only**, never parsing the protobuf business payload.

The design is split into three layers, each with a distinct owner and change cadence:

| Doc | Layer | Answers | Owner |
|---|---|---|---|
| 01 | Domain model | What production facts mean (Lot, Wafer, Tool, Chamber, Recipe Rev, Process/Measurement facts) | Domain / data |
| 02 | Routing metadata spec | How the gateway routes & preprocesses (header layers, projection, digest, overflow) | Integration / platform |
| 00 | Assumptions | The premises both rely on (grain, batching, routing dimensions, transport) | This design |

**Core contract**: the body is authoritative; headers are sender-side **exact projections** of the body for routing and preprocess. Headers never become a second business payload.

```
   WHAT IS TRUE                              HOW IT'S ROUTED
   ────────────                              ───────────────

   ┌─────────────────────────┐
   │ 01 Domain Model         │  facts: Lot, Wafer, Tool, Chamber,
   │ "what production         │         Recipe Rev, Process Execution
   │  facts mean"            │
   └───────────┬─────────────┘
               │ exact projection (sender-side, body-derived)
               ▼
   ┌─────────────────────────┐         ┌──────────────────────────┐
   │ 00 Assumptions          │ governs │ 02 Routing Metadata Spec │
   │ "grain, batching,       │────────▶│ "header layers, process  │
   │  routing dimensions,    │         │  context projection,     │
   │  transport"            │         │  digest, overflow"       │
   └─────────────────────────┘         └──────────────────────────┘
```

---

## 2. Architecture at a glance

```
Tool / EAP / DCS client
   │  unary gRPC: header projection (routing) + protobuf body (authoritative)
   ▼
APISIX
   ├─ validate Layer 1 + Layer 2 headers, contract version, registered tx-type/profile
   ├─ route by: method path → target-system → transaction-type → site → tool → route-profile
   ├─ (optional) gateway preprocess uses Layer 3 for audit / consistency / de-batch
   └─ forward (no payload parsing, no business decisions)
   ▼
RMS / SPC / APC backends ── own business validation; treat body as source of truth;
                            reject header/body mismatch
```

Backend owns production facts; APISIX is a routing/policy gateway only.

---

## 3. Header layers

```
Layer 1  Common Envelope     x-request-id, x-correlation-id, x-contract-version,
                             x-source-system, x-target-system, x-transaction-type
Layer 2  Routing Projection  x-site-id, x-tool-id, x-route-profile, x-routing-grain
Layer 3  Process Context     x-process-context-count / -format / -digest,
                             x-process-context (repeated, one per context)
Layer 4  TX-specific Profile reserved for future; v1 keeps only a minimal registry
```

Routing uses **Layer 1 + Layer 2 only**. Layer 3 is for gateway preprocess / audit / consistency, not routing.

---

## 4. Key design decisions (and the seams they close)

These are the decisions an EA should weigh; each resolves a seam between Docs 01 and 02.

1. **Body is authoritative; headers are exact projections.** Every header is extracted from the same body, so the two cannot independently diverge. Mismatch → reject.
2. **Routing is tool-level and single-valued.** Layer 1 + Layer 2 keys only; lot/chamber/recipe never route.
3. **Multiple process contexts ride in one transaction as repeated `x-process-context` (batch).** No client-side fan-out. Because routing is tool-level (decision 2), batching does not affect the route decision — the multiplicity is confined to Layer 3.
4. **Process context is the projection grain** (`lot + chamber + operation + recipe`), not isolated lot/chamber lists — matching the domain model's process-execution concept.
5. **Chamber is not a routing dimension**; it lives only inside Layer 3 process context. This is precisely what lets a multi-chamber batch keep tool-level routing.
6. **Integrity via digest + bounded overflow.** A sha256 digest over the canonicalized projection makes consistency verifiable; an explicit overflow policy (count + flag) keeps batches safe under the metadata size limit.

Decisions 2–5 visualized — one tool-level transaction carries a multi-chamber batch in Layer 3, while routing stays single-valued:

```
  One operation: tool ETCH01, a lot's wafers across CH-A / CH-B
  ──────────────────────────────────────────────────────────────

  ROUTING (Layer 1+2, single-valued)        PROCESS CONTEXT (Layer 3, repeated)
  ─────────────────────────────────        ──────────────────────────────────
  x-target-system : apc                     x-process-context-count: 2
  x-transaction-type: apc.control.calculate x-process-context-digest: sha256:...
  x-site-id      : F18                      x-process-context: lot=LOT01&chamber=CH-A&op=OP100&rev=V3
  x-tool-id      : ETCH01                    x-process-context: lot=LOT02&chamber=CH-B&op=OP100&rev=V3
  x-route-profile: apc-control-batch-v1
  x-routing-grain: tool
        │                                          │
        ▼                                          ▼
  APISIX routes on these ──▶ one backend     gateway preprocess / backend
  (chamber NOT here)                         correlates & validates (chamber here)

  ✗ NOT this: chamber as a routing key → ambiguous for a multi-chamber batch
```

---

## 5. Routing model

Routing keys are stable, single-valued, tool-level. APISIX narrows from API surface to a backend:

```
   gRPC method path  /package.Service/Method   ── selects API surface
            ▼  x-target-system        rms / spc / apc          ── route group
            ▼  x-transaction-type     apc.control.calculate …  ── tx-specific backend
            ▼  x-site-id              F18 …                    ── site/fab isolation
            ▼  x-tool-id              ETCH01 …                 ── tool-level
            ▼  x-route-profile        apc-control-batch-v1 …   ── registered route profile
            ▼  selected upstream
```

`x-routing-grain` (default `tool`) declares the intended grain so the gateway need not infer it.

**Separation of concerns — dimensions vs policy.** The spec defines *which stable dimensions are available* for routing; it does not fix *how* a given route uses them. Actual route policy — blue/green and canary rollouts, by tool or (where needed) by a context dimension, with progressive traffic ratios — is **gateway route configuration**, decided per-route by the platform engineers, not hard-coded in the metadata contract.

In practice this means:
- **Default / most transactions**: tool-level routing and canary using Layer 1+2 keys (`x-tool-id`, `x-route-profile`). Batch transactions keep their multi-context payload in Layer 3 untouched.
- **When a route needs finer canary granularity**: the gateway *may* additionally read a Layer 3 context dimension (e.g. chamber, recipe-rev) for that specific route. The data is present in metadata; using it is a route-config choice, applied selectively where the rollout requires it.

This is deliberate: keeping routing policy out of the contract lets canary strategy evolve (ratios, dimensions, per-tool overrides) without a contract version bump. The contract guarantees the *dimensions are there and stable*; the gateway owns *how aggressively to slice on them*.

---

## 6. Cross-cutting policies

- **Size / overflow**: projection bounded (v1: ≤25 contexts, ≤512 B each, ≤4 KB total); on overflow the sender sets count + `x-process-context-overflow: true` and the backend reads full detail from the body.
- **Consistency**: body authoritative; sha256 digest over canonicalized (key-sorted, order-independent) projection; backend rejects header/body mismatch.
- **Responsibility split**: sender projects & ensures consistency; APISIX validates envelope/routing headers and routes; backend owns business validation.
- **Transport**: unary only; streaming deferred to a separate spec.
- **Versioning**: `x-contract-version` mandatory; `x-transaction-type` and `x-route-profile` from a controlled registry; Layer 4 reserved.
- **Security**: no sensitive payloads in metadata; routing metadata is not authorization — backends still enforce domain authz.

---

## 7. What's solid vs what needs EA decision

**Solid**
- Layering (domain ↔ routing) is clean and consistently applied.
- Tool-level single-valued routing is internally consistent; batching does not compromise it.
- Process-context grain matches the domain model; digest + overflow give verifiable consistency and bounded size.

**Open for EA discussion**
- **OPEN-A** Overflow thresholds (25 / 512 B / 4 KB) — confirm against real batch widths and gateway metadata limit.
- **OPEN-B** Digest consumer — gateway (only if it reads body) vs backend; confirm sender-side digest cost/benefit.
- **OPEN-C** De-batch under overflow — define the adapter's body-based de-batch path when Layer 3 is suppressed.
- **OPEN-D** APISIX header-to-variable naming — validate in target environment.
- **OPEN-E** Layer 4 (TX-specific profile) — keep reserved until v1 proves insufficient.

---

## 8. Recommendation to EA

The contract is ready for review as a **v1 unary-only baseline**. The decisions an EA should explicitly endorse are: **(2) tool-level single-valued routing**, **(3) batch via repeated process-context rather than fan-out**, and **(1) body-authoritative exact projection**. Open items A–E are scoped and non-blocking for a v1 rollout; OPEN-A, OPEN-C, and OPEN-D should be validated before production traffic.
