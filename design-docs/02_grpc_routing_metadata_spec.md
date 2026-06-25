# 02 gRPC Routing Metadata Spec

## Table of Contents

- [1. Executive Summary](#1-executive-summary)
- [2. Purpose and Scope](#2-purpose-and-scope)
- [3. Core Contract](#3-core-contract)
- [4. Header Layers](#4-header-layers)
- [5. Required Header Summary](#5-required-header-summary)
- [6. Exact Process Context Projection](#6-exact-process-context-projection)
- [7. Responsibility Split](#7-responsibility-split)
- [8. Examples](#8-examples)
- [9. Anti-patterns](#9-anti-patterns)
- [10. Future Extension](#10-future-extension)
- [Appendix A. URL-encoded Query String Rules](#appendix-a-url-encoded-query-string-rules)
- [Appendix B. Canonicalization and Digest](#appendix-b-canonicalization-and-digest)
- [Appendix C. Overflow Policy](#appendix-c-overflow-policy)
- [Appendix D. Minimal TX Registry](#appendix-d-minimal-tx-registry)

---

## 1. Executive Summary

This document defines the **gRPC metadata / header contract** used by APISIX and gateway components to route and preprocess requests from Tool-side systems to backend RMS / SPC / APC services.

The migration path is:

```text
Existing CORBA TX
  → gRPC TX
  → APISIX / Gateway
  → Backend RMS / SPC / APC Service
```

The most important contract is:

> **Body is authoritative. Headers are sender-side extracted exact projections from the body for APISIX routing and gateway preprocess.**

This spec has three active layers and one reserved future layer:

```text
Layer 1: Common Envelope
  - x-request-id
  - x-correlation-id
  - x-contract-version
  - x-source-system
  - x-target-system
  - x-transaction-type

Layer 2: Routing Projection
  - x-site-id
  - x-tool-id
  - x-route-profile
  - x-routing-grain

Layer 3: Exact Process Context Projection
  - x-process-context-count
  - x-process-context-format
  - x-process-context-digest
  - x-process-context repeated

Layer 4: TX-specific Profile
  - reserved for future
```

Core rules:

```text
1. Body is authoritative.
2. Header is exact projection from body at sender stage.
3. x-process-context represents process execution context, not simple lot info.
4. APISIX routes mainly by Layer 1 + Layer 2.
5. Gateway preprocess may use Layer 3.
6. Backend / adapter must validate header-body consistency.
7. If projection exceeds metadata limit, use overflow policy.
```

Example:

```text
x-process-context-count: 2
x-process-context-format: urlencoded-query-string-v1
x-process-context-digest: sha256:<hash>

x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP_V3
x-process-context: lot_id=LOT02&chamber_id=CH-B&operation_id=OP100&recipe_rev_id=RCP_V3
```

---

## 2. Purpose and Scope

### 2.1 Purpose

This document defines routing metadata for gRPC requests that pass through APISIX.

It is designed for systems where:

```text
The gRPC body already contains complete business payload.
APISIX / gateway cannot rely on body parsing for routing.
Some exact body subset is still needed in metadata for routing / preprocess / audit.
```

### 2.2 In Scope

This document defines:

```text
Common request metadata
Routing metadata
Exact process context projection metadata
Header-body consistency principle
Gateway / backend responsibility split
Minimal examples for APC / RMS / SPC
Overflow and digest policy at spec level
```

### 2.3 Out of Scope

This document does not define:

```text
Full protobuf payload schema
Full CORBA-to-gRPC payload mapping
Full RMS / SPC / APC business validation
Full transaction-specific required field matrix
Full APISIX route implementation YAML
```

Domain semantics are defined in:

```text
01_generic_process_domain_model.md
```

---

## 3. Core Contract

### 3.1 Body Is Source of Truth

The gRPC request body is the authoritative business payload.

Examples of body-owned data:

```text
lot_id
wafer_id
tool_id
chamber_id
operation_id
recipe_rev_id
measurement values
APC control details
RMS recipe body / verification payload
```

### 3.2 Header Is Exact Projection

Headers are sender-side extracted exact projections from the body.

They are used for:

```text
APISIX routing
Gateway preprocess
Audit enrichment
Correlation
Basic consistency checking
```

Headers must not become a second independent business payload.

### 3.3 Process Context Is the Correct Projection Grain

For process-sensitive transactions, do not project isolated lot/chamber/recipe lists.

Correct grain:

```text
process context =
lot_id
+ chamber_id
+ operation_id
+ recipe_rev_id
+ optional wafer_id / route_step_id / execution_id
```

Reason:

> In semiconductor production, lot, chamber, operation, and recipe are bound together by process execution context.

---

## 4. Header Layers

## 4.1 Layer 1: Common Envelope

All requests must include Common Envelope headers.

| Header | Required | Example | Purpose |
|---|---:|---|---|
| `x-request-id` | Yes | `REQ-20260625-000001` | Unique request ID |
| `x-correlation-id` | Yes | `CORR-LOT12345-001` | Cross-system correlation |
| `x-contract-version` | Yes | `v1` | Metadata contract version |
| `x-source-system` | Yes | `eap` | Calling system |
| `x-target-system` | Yes | `apc` | Target backend family |
| `x-transaction-type` | Yes | `apc.control.calculate` | Logical transaction type |

Rules:

```text
Header names must be lowercase.
Use hyphen-separated header names.
x-transaction-type must come from controlled registry.
x-contract-version means metadata contract version, not protobuf schema version.
```

---

## 4.2 Layer 2: Routing Projection

Layer 2 contains the routing projection used by APISIX.

| Header | Required | Example | Purpose |
|---|---:|---|---|
| `x-site-id` | Yes | `F18` | Site / fab routing |
| `x-tool-id` | Yes | `ETCH01` | Tool-level routing |
| `x-route-profile` | Yes | `apc-control-batch-v1` | APISIX route profile |
| `x-routing-grain` | Recommended | `tool` | Declares routing grain |

Recommended APISIX route key:

```text
x-target-system
+ x-transaction-type
+ x-site-id
+ x-tool-id
+ x-route-profile
```

Rules:

```text
APISIX v1 routes mainly by Layer 1 + Layer 2.
APISIX should not parse protobuf body for routing.
APISIX should not route by lot/chamber/recipe lists.
x-route-profile must be registered.
```

Recommended `x-routing-grain` values:

| Value | Meaning |
|---|---|
| `system` | Route by target system only |
| `site` | Route by site |
| `tool` | Route by tool |
| `route-profile` | Route by explicit route profile |

Default recommendation:

```text
x-routing-grain: tool
```

---

## 4.3 Layer 3: Exact Process Context Projection

Layer 3 is used when gateway preprocess or audit needs exact process context without parsing body first.

| Header | Required | Example | Purpose |
|---|---:|---|---|
| `x-process-context-count` | Conditional | `2` | Number of process contexts in projection |
| `x-process-context-format` | Conditional | `urlencoded-query-string-v1` | Encoding format |
| `x-process-context-digest` | Recommended when projected | `sha256:<hash>` | Digest of canonicalized projection |
| `x-process-context` | Conditional / Repeated | `lot_id=LOT01&chamber_id=CH-A...` | Exact process context projection |

Layer 3 is required when:

```text
The transaction contains process-execution-level context,
and gateway / adapter preprocess needs exact body-derived context.
```

Layer 3 may be omitted when:

```text
The transaction has no lot / chamber / operation / recipe context.
```

Examples where Layer 3 is commonly useful:

```text
apc.control.calculate
spc.measurement.submit
rms.recipe.verify
```

Examples where Layer 3 may not be needed:

```text
rms.recipe.list
spc.chart.query
apc.health.check
```

---

## 4.4 Layer 4: TX-specific Profile

Layer 4 is reserved.

It will define transaction-specific rules by:

```text
legacy CORBA TX
gRPC transaction type
target backend system
route profile
```

Layer 4 should not be fully implemented in routing metadata v1.

For v1, maintain only a minimal registry. See [Appendix D](#appendix-d-minimal-tx-registry).

---

## 5. Required Header Summary

| Header | Required | Layer |
|---|---:|---|
| `x-request-id` | Yes | Layer 1 |
| `x-correlation-id` | Yes | Layer 1 |
| `x-contract-version` | Yes | Layer 1 |
| `x-source-system` | Yes | Layer 1 |
| `x-target-system` | Yes | Layer 1 / 2 |
| `x-transaction-type` | Yes | Layer 1 / 2 |
| `x-site-id` | Yes | Layer 2 |
| `x-tool-id` | Yes | Layer 2 |
| `x-route-profile` | Yes | Layer 2 |
| `x-routing-grain` | Recommended | Layer 2 |
| `x-process-context-count` | Conditional | Layer 3 |
| `x-process-context-format` | Conditional | Layer 3 |
| `x-process-context-digest` | Recommended when projected | Layer 3 |
| `x-process-context` | Conditional / Repeated | Layer 3 |

---

## 6. Exact Process Context Projection

### 6.1 Why Process Context, Not Lot Info

Do not use simple repeated lot headers such as:

```text
x-lot-info: lot_id=LOT01&chamber_id=CH-A
x-lot-info: lot_id=LOT02&chamber_id=CH-B
```

The semantic grain is not lot alone.

Use:

```text
x-process-context
```

because the projected unit is:

```text
lot + chamber + operation + recipe
```

This matches the process execution concept in the generic domain model.

---

### 6.2 Format

Use:

```text
x-process-context-format: urlencoded-query-string-v1
```

Each `x-process-context` value is one URL-encoded query string.

Example:

```text
x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP_V3
```

If a value contains reserved characters, percent-encode it.

Example:

```text
x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP%2FETCH%2BV3
```

Detailed encoding rules are in [Appendix A](#appendix-a-url-encoded-query-string-rules).

---

### 6.3 Allowed Fields

| Field | Required | Meaning |
|---|---:|---|
| `lot_id` | Recommended | Lot identifier |
| `wafer_id` | Optional | Wafer identifier |
| `chamber_id` | Recommended | Chamber identifier |
| `operation_id` | Recommended | MES operation |
| `route_step_id` | Optional | Route step |
| `recipe_id` | Optional | Recipe logical ID |
| `recipe_rev_id` | Optional | Recipe revision |
| `process_node_id` | Optional | Derived SPC / APC process context |
| `lot_operation_execution_id` | Optional | Lot-level execution ID |
| `wafer_process_execution_id` | Optional | Wafer-level execution ID |

---

### 6.4 Projection Rules

```text
Rule 1:
x-process-context values are extracted from body at sender stage.

Rule 2:
x-process-context must not contain values absent from body.

Rule 3:
One x-process-context header represents one process context.

Rule 4:
Repeated x-process-context headers are allowed.

Rule 5:
Do not encode multiple process contexts into one x-process-context value.

Rule 6:
If header projection and body disagree, backend / adapter must reject the request.

Rule 7:
If projection exceeds metadata limit, use overflow policy.
```

---

## 7. Responsibility Split

## 7.1 Sender Responsibility

Sender must:

```text
Populate the complete gRPC body.
Extract required routing metadata from body.
Generate exact process context projection when required.
Generate digest when process context is projected.
Ensure metadata/body consistency before sending.
```

---

## 7.2 APISIX / Gateway Responsibility

APISIX / gateway may validate:

```text
Required Layer 1 headers exist.
Required Layer 2 headers exist.
x-contract-version is supported.
x-target-system is supported.
x-transaction-type is registered.
x-route-profile is registered.
metadata size is within limit.
```

Gateway preprocess may use Layer 3 for:

```text
audit enrichment
basic format check
context count check
metadata/body consistency check if body access is available
de-batch decision for adapter routing
```

APISIX / gateway should not perform:

```text
recipe qualification
SPC rule evaluation
APC control decision
lot / wafer business state validation
deep domain validation
```

---

## 7.3 Backend / Adapter Responsibility

Backend / adapter must validate:

```text
Header/body consistency
Transaction-specific business requirements
Recipe qualification / deployment / verification
SPC measurement validity
APC control context
Lot / wafer / chamber consistency
```

If metadata and body disagree:

```text
Reject the request.
Log metadata/body mismatch as integration defect.
```

---

## 8. Examples

## 8.1 APC Batch Control

Headers:

```text
x-request-id: REQ-001
x-correlation-id: CORR-001
x-contract-version: v1
x-source-system: eap
x-target-system: apc
x-transaction-type: apc.control.calculate

x-site-id: F18
x-tool-id: ETCH01
x-route-profile: apc-control-batch-v1
x-routing-grain: tool

x-process-context-count: 2
x-process-context-format: urlencoded-query-string-v1
x-process-context-digest: sha256:<hash>

x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP_ETCH_V3
x-process-context: lot_id=LOT02&chamber_id=CH-B&operation_id=OP100&recipe_rev_id=RCP_ETCH_V3
```

Body remains authoritative:

```text
tool_id = ETCH01

items:
  - lot_id: LOT01
    chamber_id: CH-A
    operation_id: OP100
    recipe_rev_id: RCP_ETCH_V3

  - lot_id: LOT02
    chamber_id: CH-B
    operation_id: OP100
    recipe_rev_id: RCP_ETCH_V3
```

---

## 8.2 RMS Recipe Verify

```text
x-request-id: REQ-002
x-correlation-id: CORR-002
x-contract-version: v1
x-source-system: eap
x-target-system: rms
x-transaction-type: rms.recipe.verify

x-site-id: F18
x-tool-id: ETCH01
x-route-profile: rms-recipe-verify-v1
x-routing-grain: tool

x-process-context-count: 1
x-process-context-format: urlencoded-query-string-v1
x-process-context-digest: sha256:<hash>
x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP_ETCH_V3
```

---

## 8.3 SPC Measurement Submit

```text
x-request-id: REQ-003
x-correlation-id: CORR-003
x-contract-version: v1
x-source-system: dcs
x-target-system: spc
x-transaction-type: spc.measurement.submit

x-site-id: F18
x-tool-id: METRO01
x-route-profile: spc-measurement-v1
x-routing-grain: tool

x-process-context-count: 1
x-process-context-format: urlencoded-query-string-v1
x-process-context-digest: sha256:<hash>
x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP_ETCH_V3
```

---

## 9. Anti-patterns

## 9.1 Anti-pattern: Header as Full Payload

Bad:

```text
x-lot-id: LOT01,LOT02,LOT03
x-chamber-id: CH-A,CH-B,CH-C
x-recipe-rev-id: RCP1,RCP2,RCP3
```

Better:

```text
x-process-context-count: 3
x-process-context-format: urlencoded-query-string-v1
x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP1
x-process-context: lot_id=LOT02&chamber_id=CH-B&operation_id=OP100&recipe_rev_id=RCP2
x-process-context: lot_id=LOT03&chamber_id=CH-C&operation_id=OP100&recipe_rev_id=RCP3
```

---

## 9.2 Anti-pattern: APISIX Performs Business Validation

Bad:

```text
APISIX validates recipe qualification.
APISIX calculates APC control decision.
APISIX evaluates SPC rules.
```

Better:

```text
APISIX routes.
Gateway preprocess performs basic metadata checks.
Backend performs business validation.
```

---

## 9.3 Anti-pattern: Free-form Transaction Type

Bad:

```text
x-transaction-type: calcApc
x-transaction-type: APC_QUERY
x-transaction-type: oldTx1
```

Better:

```text
x-transaction-type: apc.control.calculate
x-transaction-type: apc.parameter.query
```

---

## 9.4 Anti-pattern: Header Overrides Body

Bad:

```text
Backend trusts x-process-context when body differs.
```

Better:

```text
Backend treats body as authoritative.
Backend rejects header/body mismatch.
```

---

## 10. Future Extension

Layer 4 is reserved for TX-specific profile.

Future Layer 4 may define:

```text
legacy CORBA TX mapping
transaction-specific required headers
transaction-specific optional headers
transaction-specific preprocess behavior
route-profile-specific gateway rules
idempotency and retry policy
error taxonomy by transaction
```

Do not implement full Layer 4 until routing metadata v1 proves insufficient.

---

# Appendix A. URL-encoded Query String Rules

`x-process-context-format` must be:

```text
urlencoded-query-string-v1
```

Encoding rules:

```text
1. Each x-process-context value is one URL-encoded query string.
2. Key-value pairs are separated by `&`.
3. Key and value are separated by `=`.
4. Keys and values must be percent-encoded when they contain reserved characters.
5. Repeated x-process-context headers are allowed.
6. Do not encode multiple process contexts into one header value.
7. Do not use JSON as x-process-context value in v1.
```

Correct:

```text
x-process-context: lot_id=LOT01&chamber_id=CH-A&operation_id=OP100&recipe_rev_id=RCP%2FETCH%2BV3
```

Incorrect:

```text
x-process-context: [{"lot_id":"LOT01","chamber_id":"CH-A"}]
x-process-context: lot_id=LOT01,LOT02&chamber_id=CH-A,CH-B
```

---

# Appendix B. Canonicalization and Digest

When `x-process-context` is projected, `x-process-context-digest` is recommended.

Purpose:

```text
Detect header/body projection mismatch.
Detect process context ordering / serialization issues.
Support gateway / backend consistency checks.
```

Recommended format:

```text
x-process-context-digest: sha256:<hex_digest>
```

Canonicalization rule:

```text
1. Extract process contexts from body.
2. For each context, include only approved projection fields.
3. Sort fields by key name.
4. Percent-encode values using urlencoded-query-string-v1.
5. Serialize each context as query string.
6. Preserve body item order unless transaction defines a stable sort key.
7. Join serialized contexts with newline.
8. Compute SHA-256 over UTF-8 bytes.
```

Example canonical input:

```text
chamber_id=CH-A&lot_id=LOT01&operation_id=OP100&recipe_rev_id=RCP_ETCH_V3
chamber_id=CH-B&lot_id=LOT02&operation_id=OP100&recipe_rev_id=RCP_ETCH_V3
```

---

# Appendix C. Overflow Policy

Metadata is not a full payload channel. Repeated `x-process-context` must have limits.

Recommended v1 limits:

| Limit | Recommendation |
|---|---:|
| Max process contexts projected | 25 |
| Max single `x-process-context` length | 512 bytes |
| Max total process context metadata length | 4 KB |

If exceeded, do not project all process contexts.

Use:

```text
x-process-context-count: 125
x-process-context-format: urlencoded-query-string-v1
x-process-context-overflow: true
```

Do not include partial context list unless explicitly defined by the route profile.

Backend must read full details from body.

---

# Appendix D. Minimal TX Registry

For v1, maintain a thin transaction registry.

| Transaction Type | Legacy CORBA TX | Target System | Route Profile | Layer 3 Recommended |
|---|---|---|---|---|
| `rms.recipe.verify` | TBD | `rms` | `rms-recipe-verify-v1` | Yes |
| `rms.recipe.download` | TBD | `rms` | `rms-recipe-download-v1` | Optional |
| `spc.measurement.submit` | TBD | `spc` | `spc-measurement-v1` | Yes |
| `spc.rule.evaluate` | TBD | `spc` | `spc-rule-evaluate-v1` | Yes |
| `apc.parameter.query` | TBD | `apc` | `apc-parameter-query-v1` | Optional |
| `apc.control.calculate` | TBD | `apc` | `apc-control-batch-v1` | Yes |

---

# Final Summary

This document defines routing metadata v1.

Core model:

```text
Body = source of truth
Header = sender-side extracted exact projection
Layer 1 + Layer 2 = APISIX routing
Layer 3 = gateway preprocess / audit / consistency projection
Layer 4 = future TX-specific profile
```

Most important conclusion:

> `x-process-context` is not lot-info. It is the exact projection of process execution context: lot + chamber + operation + recipe, extracted from body at sender stage.
