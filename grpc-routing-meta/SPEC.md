# SPEC — gRPC routing-metadata wire contract

**Status:** normative · **Contract version:** `v1` · supersedes the prior (pre-`v1`) routing-metadata spec.

This is the byte-level contract a sender emits and a verifier reads. The key words
**MUST**, **MUST NOT**, **SHOULD**, **MAY** are RFC 2119. Rationale, requirements, and
the test checklist live in [`CONTEXT.md`](CONTEXT.md) — this document states only what
is on the wire. Where the two ever disagree, **this document wins**.

The contract is implemented by C++ on both ends (one shared kit); it is **not** a
cross-language byte spec — see `CONTEXT.md` for that scope decision.

---

## 1. Core contract

The protobuf **body is authoritative**. Every header value MUST be an exact projection
*of* the body — a header value MUST NOT exist without a body source. APISIX and gateway
components route/preprocess on the headers and never parse the body; because each header
is a projection, it cannot drift from the body (and §5.3 lets a verifier prove it didn't).

## 2. Header set (v1)

| Header | Layer | Value | Presence |
|---|---|---|---|
| `x-request-id` | common | sender-supplied, unique per request | always |
| `x-correlation-id` | common | sender-supplied | always |
| `x-contract-version` | common | `v1` (constant) | always |
| `x-source-system` | common | sender identity (e.g. `eap`) | always |
| `x-site-id` | common | runtime | always |
| `x-tool-id` | common | runtime / body | always |
| `x-process-context-count` | pctx | decimal `0..N` = body `repeated` size | always |
| `x-process-context-format` | pctx | `urlencoded-query-string-v1` (constant) | always |
| `x-process-context-digest` | pctx | `sha256:` + 64 hex (§5.3) | iff count>0, not overflow, and the sender requested it (`emit_digest`, default yes) |
| `x-process-context` | pctx | one canonical context (§5.1), repeated | iff count>0 and not overflow |
| `x-process-context-overflow` | pctx | `true` | iff overflow (§5.4) |
| `x-mask-id` | scalar | URL-encoded body scalar | sys3 only, iff source non-empty |
| `x-routing-error` | error | `missing:<key>` (**PROVISIONAL** §7) | iff a required scalar was empty |

The sender MUST NOT emit `x-target-system`, `x-transaction-type`, `x-route-profile`, or
`x-routing-grain`: the first two are carried by the gRPC method `:path`; the latter two are
APISIX route configuration, not sender data. *(These were normative in the superseded spec
and are intentionally dropped.)*

## 3. Common headers (Layer 1)

The six common headers MUST be filled uniformly for every system, with no per-system
branching. `x-contract-version` MUST be `v1`. The rest are sender-supplied runtime/identity
values; `x-request-id` MUST be unique per request.

## 4. Domain scalar (`x-mask-id`)

A `(routing.project)`-tagged body scalar, reached by a nested-field walk. If the source
field is **non-empty**, the sender MUST emit `x-mask-id` = `UrlEncode(value)` (§6). If it is
**empty** and the projection is `required`, see §7 (the sender MUST NOT throw and MUST NOT
emit an empty `x-mask-id`).

## 5. Process-context projection (Layer 3)

A request carries `repeated ProcessContext`. Each context projects to one
`x-process-context` line.

### 5.1 Canonical context encoding

Each `x-process-context` value MUST be the context's `(routing.pctx)` fields, **sorted by
key** (ASCII), joined by `&`, as `Key=UrlEncode(Value)`. For the shared 7-field schema the
sorted key order is exactly:

```
ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech
```

An empty field MUST be emitted as `Key=` (present-but-empty) — it is a faithful projection,
not an omission. Contexts appear in **body order** across the repeated `x-process-context`
headers.

### 5.2 count / format

`x-process-context-count` MUST equal the body `repeated` size (`0..N`) and MUST be emitted
**always**, including on overflow and when count is 0. `x-process-context-format` MUST be the
constant `urlencoded-query-string-v1` and MUST likewise always be emitted. When count is `0`,
the sender MUST emit count + format only — **no** digest, **no** context lines.

### 5.3 Digest — integrity, not security

When context lines are emitted, the sender emits
`x-process-context-digest = "sha256:" + SHA256_hex(C)` **by default**, where `C` is the emitted
`x-process-context` values joined by `\n` (newline) in emission order. Emission is a per-call
sender choice: `ProjectMeta(req, sink, emit_digest)` (default `true`) — passing `false` omits
the digest while still emitting the context lines. The verifier verifies **if present**: when
`x-process-context-digest` is present it MUST recompute and compare and reject on mismatch; when
it is **absent** the verifier MUST treat it as "not verified", **not** as drift, and accept.
(See ADR 0002.)

The digest is an **integrity** check, **not** a security control. It detects header/body
inconsistency from a sender bug, projection-version skew between an independently-deployed
sender and verifier, or transit mangling. It provides **no** protection against a party that
can edit the body: such a party recomputes the digest. There is no key and no signature.

### 5.4 Overflow policy

The sender MUST emit `x-process-context-overflow: true` and MUST suppress all
`x-process-context` lines and the digest, **iff** any of:

- `count > 25`, **or**
- any single context value `> 512` bytes, **or**
- total kit-emitted metadata `> 7168` bytes (§5.5).

`count` and `format` MUST still be emitted. Overflow is **non-blocking**: the request still
routes on the common headers, and the backend reads full detail from the body. The byte check
is independent of the count cap (25 wide contexts can exceed 7168 B).

### 5.5 Size accounting

Total metadata size MUST be measured the way gRPC/HPACK accounts it: `sum(name.size +
value.size + 32)` over every entry (RFC 7541 §4.1, the `32`-byte per-entry overhead). The
`7168` budget counts only headers the kit emits; transport/auth headers (`:path`,
`:authority`, `te`, `grpc-*`, tokens) are not included and leave headroom under the hard limit.

## 6. URL-encoding (frozen)

Applied to every projected key **and** value:

- RFC 3986 **unreserved** (`A–Z a–z 0–9 - _ . ~`) MUST be emitted verbatim.
- Every other byte MUST be `%XX`, hex digits **uppercase**.
- Space MUST be `%20` (never `+`).

Decoding is the exact inverse. A malformed escape on the receive side (a trailing `%` or a
non-hex digit) is passed through literally and MUST NOT crash the parser.

## 7. Failure & error semantics — report, don't dictate

A projection **reports**; the caller **decides**. `ProjectMeta`/`Send` MUST NOT throw on a
data condition. A missing **required** scalar (today: `x-mask-id`, sys3) MUST:

1. record a `MissingRequired` issue in the returned `ProjResult` (`ok = false`), and
2. emit `x-routing-error: missing:<key>`, and
3. **not** emit the empty scalar header.

The caller inspects `ok`, feeds `issues` to its own metrics/logs, and chooses to abort the
RPC or proceed. The kit performs no logging or metrics itself. Overflow (§5.4) is likewise
reported as a **non-blocking** `Overflow` issue (`ok` stays `true`).

> **PROVISIONAL** (pending cross-team ratification, Sender dept ↔ gateway — see §10): the
> `x-routing-error` header **name** and **value format**, the default caller policy
> (abort vs proceed), and whether APISIX consumes `x-routing-error` (dead-letter / default
> route) or the caller aborts before send.

## 8. Versioning

`x-contract-version` identifies the contract; this document is `v1`. A breaking change to any
rule above MUST bump the version. The verifier MUST reject an unknown contract version. (The
`v1→v2` negotiation procedure is out of scope for v1.)

## 9. Build-time enforcement

The contract is enforced at codegen, not just at runtime (fail loud, never silent). The
`protoc-gen-meta` plugin MUST reject — with a non-zero exit — any `(routing.project)` that is
not on a **non-repeated scalar leaf** (repeated, message-typed, or reached under a repeated
field), and any duplicate projected key. See `example/tests/negative/`.

## 10. Open / provisional (cross-team)

To be ratified with the Sender department and frozen into §7 once agreed:

1. Default failure policy: **abort** (recommended) vs proceed-on-error.
2. `x-routing-error` consumer: APISIX rule (dead-letter / default route) vs caller aborts.
3. `x-routing-error` header name + value format.

## 11. Conformance

Every rule here is asserted by the kit. The mapping rule → test, the testable invariants, and
the design rationale are in [`CONTEXT.md`](CONTEXT.md); the end-to-end run procedure is the
copy-paste session in [`DEMO.md`](DEMO.md).
