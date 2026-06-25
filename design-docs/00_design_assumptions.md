# Design Assumptions — gRPC Routing to RMS / SPC / APC

> Companion to `01_generic_process_domain_model.md` and `02_grpc_routing_metadata_spec.md`.
> Purpose: make explicit the premises the routing-metadata spec relies on, so an EA reviewer can evaluate the design against stated boundaries rather than inferring them. This records the decisions that fix the seams between the domain model and the interface spec.

---

## A. Transaction Grain & Batching

### A-1 Routing grain is tool-level, declared explicitly
A single transaction routes at **tool-level** (`x-routing-grain: tool` by default, Interface Spec §4.2). Routing keys (Layer 1 + Layer 2) are single-valued and stable. Finer-grained context (chamber, lot, recipe) does **not** participate in routing.

### A-2 One transaction may carry multiple process contexts (batch)
A transaction may cover multiple `(lot × chamber × operation × recipe)` combinations — e.g. one tool processing a lot's wafers across several chambers (Domain Model §19). These are carried as **repeated `x-process-context` headers**, one per context (Interface Spec §4.3, §6).

- This is a **batch model**, not client-side fan-out. The caller sends one transaction per tool-level operation; the per-context detail rides in Layer 3.
- Batch does not break routing because routing is tool-level (A-1) and chamber/lot/recipe never appear as routing keys (B-1). The multiplicity is confined to Layer 3 and never reaches the route decision.

### A-3 Process context is the projection grain, not lot info
The projected unit is the full process execution context (`lot + chamber + operation + recipe`, optional wafer/execution IDs), not isolated lot or chamber lists (Interface Spec §3.3, §6.1). This matches the domain model's process-execution concept: in production these dimensions are bound together, not independent.

---

## B. Routing Dimensions

### B-1 Routing keys are stable, single-valued, tool-level
Routing uses only Layer 1 + Layer 2: `x-target-system`, `x-transaction-type`, `x-site-id`, `x-tool-id`, `x-route-profile` (Interface Spec §4.2). No lot / chamber / recipe lists are used for routing (Interface Spec §9.1 anti-pattern).

### B-2 Chamber is not a default routing dimension (but is available to the gateway)
Chamber is not part of the standard routing keys (Layer 1+2) and never attaches to a lot directly (Domain Model §4.1, §21 Rule 1). By default, routing is tool-level and a multi-chamber batch (A-2) keeps single-valued routing.

However, chamber (and other Layer 3 context dimensions) **are present in the metadata** and the gateway **may** read them for a specific route — e.g. context-granular blue/green or canary rollout. Whether a given route does so is a **gateway route-configuration decision**, not fixed by this contract. The contract guarantees the dimension is available and stable; the implementation decides if and how to slice on it (see EA Summary §5, "dimensions vs policy").

> Note on batch + context-granular routing: a batch transaction spans multiple chambers, so there is no single `x-chamber-id` at the envelope level to route on. A route that needs *per-chamber* canary therefore applies to transactions whose grain is a single context (or reads the relevant Layer 3 entry by an agreed rule). This is a per-route engineering choice; the default tool-level path is unaffected.

### B-3 Wafer is not a routing dimension
`wafer_id` is an optional field inside the Layer 3 process context, used for correlation, not route selection.

### B-4 Process / execution context: preprocess + correlation by default; routing use is a gateway choice
`x-process-context` and its `process_node_id` / execution-id fields (Layer 3) are, by default, for gateway preprocess, audit enrichment, and backend correlation/validation against domain facts (Domain Model §18 `PROCESS_NODE`, §8.2 `WAFER_PROCESS_EXECUTION`) — not standard routing keys.

The contract does not *forbid* the gateway from reading a Layer 3 dimension for routing (e.g. context-granular canary); it simply does not *require* it and does not make it a standard key. The decision is the platform engineers', per route. Caveat: Layer 3 may be suppressed under the overflow policy (E-1), so any route relying on a Layer 3 dimension must define behavior when the projection is absent (see OPEN-C).

---

## C. Transport

### C-1 Unary gRPC only
This contract assumes unary calls. Streaming — including high-volume SPC measurement or APC feedback — is out of scope and deferred to a separate streaming extension spec.

### C-2 Repeated-context ordering is not significant
Repeated `x-process-context` headers carry no ordering semantics; consumers must not depend on order. The integrity digest (D-2) is computed over a canonicalized, key-sorted form so it is order-independent.

---

## D. Consistency Enforcement

### D-1 Body is authoritative; headers are exact projections
The gRPC body is the source of truth. Every header value is a sender-side exact projection extracted from that same body (Interface Spec §3.1–3.2). Headers must not carry any value absent from the body, and must never override it. On mismatch, the backend/adapter rejects the request.

### D-2 Projection integrity via digest
When `x-process-context` is projected, `x-process-context-digest` (sha256 over the canonicalized projection, Interface Spec Appendix B) lets the receiver detect projection/serialization mismatch — making consistency verifiable, not merely a discipline. Canonicalization is key-sorted, so the digest is independent of context ordering (C-2).

### D-3 Senders do not hardcode metadata keys
Metadata keys and the transaction registry are fixed by the contract (Interface Spec §4, Appendix D), versioned via `x-contract-version`. Senders import the contract and regenerate on version change; no hardcoded strings.

---

## E. Size & Overflow

### E-1 Metadata budget and overflow policy
Metadata is not a payload channel. Per Interface Spec Appendix C, projection has explicit limits (v1: ≤25 contexts, ≤512 B per context, ≤4 KB total). On overflow, the sender does **not** project the full list; it sets `x-process-context-count` and `x-process-context-overflow: true`, and the backend reads full detail from the body.

- This makes the batch model (A-2) safe under the metadata size limit: large batches degrade gracefully to count + overflow flag rather than bursting the header budget.

---

## Open Items (for EA discussion)

- **OPEN-A** Overflow threshold tuning (E-1): the v1 limits (25 / 512 B / 4 KB) are starting points; confirm against real batch widths and gateway metadata limits.
- **OPEN-B** Digest consumer: who verifies `x-process-context-digest` and when (gateway vs backend/adapter). If the gateway never accesses the body, the digest is backend-only — confirm the cost/benefit of sender-side digest generation.
- **OPEN-C** De-batch under overflow (E-1 + §7.2): if an adapter relies on Layer 3 to de-batch, an overflowed request has no projected contexts — define the adapter's de-batch path from the body in that case.
- **OPEN-D** Header-to-variable naming in the actual APISIX/NGINX environment (e.g. `x-target-system` → `http_x_target_system`) — validate in target env.
- **OPEN-E** Layer 4 (TX-specific profile) is reserved; do not implement until v1 proves insufficient (Interface Spec §10).
- **OPEN-F** Routing policy (blue/green, canary — by tool and/or context, traffic ratios) is gateway route-configuration, owned by platform engineers, not fixed by the contract (B-2, B-4, EA Summary §5). To confirm: the set of context dimensions a route may slice on for canary, and the rule for reading them when a batch spans multiple contexts (ties to OPEN-C).

---

## Decision Log (this version)

| Decision | Choice | Replaces seam |
|---|---|---|
| Routing grain | Tool-level, declared via `x-routing-grain` (A-1) | Previously undefined routing grain |
| Multi-context handling | Batch via repeated `x-process-context` (A-2) | Earlier fan-out assumption — superseded |
| Projection grain | Full process context, not lot info (A-3) | Lot/chamber treated as independent lists |
| Chamber routing | Default tool-level; chamber available in L3 for selective canary (B-2) | Doc1/Doc2 chamber seam |
| Routing policy | Contract defines dimensions; gateway config owns canary/blue-green policy (B-4, OPEN-F) | "context never routes" was too rigid for canary-by-context |
| Consistency | Body-authoritative + sha256 digest (D-1, D-2) | Consistency rule with no enforcement mechanism |
| Size safety | Explicit overflow policy + flag (E-1) | Unbounded metadata growth risk |
