# REQUIREMENTS — gRPC Header Kit

> Spec document · line-item traceable · companions: SUMMARY.md (summary), PROPOSAL.md (usage), CONTEXT.md (decisions)

ID scheme: FR = functional requirement, NFR = non-functional / constraint, CON = design convention. Each item carries an acceptance criterion (AC).

---

## 1. Scope

- **In**: derive three gRPC metadata headers from an existing proto message, for APISIX context-based routing.
- **Out**: body (XML payload) serialization, the APISIX route rules themselves, RPC transport.
- **Existing assets**: 4 services / 30 tx methods / payloads already generated; this kit only adds the header layer.

---

## 2. Functional Requirements (FR)

### FR-1 Auto-generated headers
- Headers are derived from the proto message instance; senders neither hand-write headers nor maintain an external mapping file.
- **AC**: after editing a proto field and re-running codegen, header output changes accordingly with no change to sender business logic.

### FR-2 Three domain-context headers
- `tool-header` — cardinality **1**, required.
- `lot-header` — cardinality **1..N**.
- `mask-header` — cardinality **0..N**.
- **AC**: no tool field → error; no lot → error; no mask → valid (skipped).

### FR-3 Cardinality expression
- Multiple entries use gRPC **repeated metadata** (same key, multiple values); no index suffixes (`lot-header-0/1`).
- **AC**: 25 lots produce 25 same-named `lot-header` entries.

### FR-4 APISIX header routing
- Routing depends only on header fields; the gateway never parses the payload.
- **AC**: routing still resolves correctly with payload contents removed (headers only).

### FR-5 Header is a payload projection
- Every header value must already exist in the body; headers carry no information absent from the body.
- **AC**: each header `key=value` maps to a field in the same message.

### FR-6 Selective field annotation
- Only fields annotated with `(routing.header)` enter headers; un-annotated fields stay in the body.
- **AC**: un-annotated fields appear in no header.

---

## 3. Non-Functional Requirements / Constraints (NFR)

### NFR-1 gRPC compliance
- Metadata keys lowercase; values restricted to printable ASCII (0x20–0x7E).
- Implementation: values are always URL-encoded (RFC 3986; space as `%20`, not `+`).
- **AC**: field values containing CJK / special characters become valid ASCII metadata after encoding.

### NFR-2 Metadata size ≤ 8 KB
- Total metadata per request must not exceed 8 KB (hard limit).
- Verified: worst case 25 lots + 3 masks ≈ 1.5 KB (~19% of budget, ~81% headroom); theoretical ceiling ~127 lots.
- **AC**: header total under the typical worst-case cardinality is < 8 KB, no degradation strategy required.

### NFR-3 No hardcoding on version change
- On interface version change, senders only import the new proto; no edits to header-generation code.
- **AC**: after adding / changing annotated fields, a sender rebuild takes effect with no hardcoded strings.

### NFR-4 Extensibility
- The mechanism must support adding header groups or fields in future, backward-compatible.
- **AC**: adding a group does not break existing senders' compilation or output.

### NFR-5 Strong consistency
- Body and header must not drift; omissions must be caught at compile time (plugin path).
- **AC**: annotating a non-scalar field → protoc build fails with a clear message.

### NFR-6 Cross-product generalization
- The same mechanism applies across dozens of tx with no per-tx special-casing.
- **AC**: different tx messages reuse the same option + sample with no per-tx custom logic.

---

## 4. Design Conventions (CON)

### CON-1 Fixed domain → header mapping
- Groups `TOOL`/`LOT`/`MASK` map to fixed `tool-header`/`lot-header`/`mask-header`; senders never set the metadata key name.

### CON-2 Scalar-only in headers
- Only scalar fields supported (string/int/bool, etc.); nested messages unsupported.
- Violations fail at plugin build time (see NFR-5 AC).

### CON-3 Frozen format
- URL-encode rules are frozen once shipped; changes require sender/receiver coordination (else decode mismatches).

### CON-4 `order` is best-effort
- `order` only controls output ordering, not correctness (receiver looks up by key). proto3 `order: 0` is treated as unset; number from 1.

### CON-5 Contract SSOT in repo
- `header_options.proto` is the single source for the contract, versioned; senders import, never copy. Changing a group's semantics = breaking change.

---

## 5. Traceability Matrix (requirement → design / verification)

| Requirement | Design means | Verification status |
|---|---|---|
| FR-1 / NFR-3 / NFR-5 | proto custom option + codegen | ✅ plugin verified end-to-end |
| FR-2 / FR-3 | repeated metadata + cardinality checks | ✅ 25-lots output compared |
| FR-4 / FR-5 / FR-6 | header = scalar projection subset | ✅ output byte-identical |
| NFR-1 | shared url_encode.h | ✅ unit-tested (incl. CJK) |
| NFR-2 | capacity estimate | ✅ 1.5 KB / 19% |
| NFR-4 / NFR-6 | extensible group enum | ✅ multi-level package / multi-tx |
| CON-2 | non-scalar build-time interception | ✅ protoc fails with error |

---

## 6. Open Items (OPEN)

- **OPEN-1** How APISIX consumes repeated headers (per-item / merged) + actual gateway metadata limit.
- **OPEN-2** Formal confirmation of the cardinality upper bound (currently 25 lots + 3 masks as the design worst case).
- **OPEN-3** Future need for nested messages in headers.
- **OPEN-4** Register extension number `50001` in the proto registry.
