# Reconciliation Review — ARCHITECTURE-SPINE vs SPEC + CONTEXT + live code

**Scope:** Does the spine accurately *ratify* the real code and the wire contract — not invent or misstate them?
**Reviewer role:** Independent reconciliation check (bytes win = SPEC.md).
**Date:** 2026-06-27

**Verdict: PASS-WITH-FINDINGS.** Every load-bearing claim the spine makes about the
code and the wire is technically correct. AD-2/AD-3 (namespace + ADL), AD-5
(throw → failure-as-data), AD-7 (duplicated `32`), and AD-9/CR1 (wire frozen)
all check out against the actual source. The Structural Seed matches the real
tree with `NEW` files correctly flagged. Findings are limited to one expected
doc-skew tracking item (already routed to criterion I) plus low-severity
completeness/wording nits. No invented or misstated behavior; no undeclared
wire-byte change.

---

## 1. AD-2 / AD-3 — namespace + ADL on the sink  ✅ technically correct

**Claim:** generated `ProjectMeta` is *currently* in the GLOBAL namespace and
resolves only because the demo includes all headers; the fix is to wrap it in
`namespace routingmeta`, resolved by ADL on the `routingmeta::MetadataSink` arg.

**Verified against `src/plugin/protoc-gen-meta.cc`:**

- The generated `.proj.h` (lines 175–187) prints `#pragma once`, two `#include`s,
  then `void ProjectMeta(const $ns$::$m$& req, routingmeta::MetadataSink& sink);`
  with **no `namespace` wrapper**. The `.proj.cc` definition (lines 208–209) is
  likewise emitted at file scope. So the function is declared in the **global
  namespace** today. The argument types are `sysN::v1::<Req>` (in `sysN::v1`) and
  `routingmeta::MetadataSink&`. → **Claim CONFIRMED: current `ProjectMeta` is global.**
- `sender/unified_sender.cc`: `template<class Req> void Send(...)` lives in the
  global namespace and calls **unqualified** `ProjectMeta(req, sink)` (line 35).
  Because `ProjectMeta` is global and `Send` is global, **ordinary unqualified
  lookup** finds it — provided the declaration is visible. The sender includes all
  three (`sys1/2/3.proj.h`, lines 26–28). → **Claim CONFIRMED: "resolves only
  because the demo includes all headers."**
- **Fix is correct C++.** Put `ProjectMeta` inside `namespace routingmeta` and the
  unqualified call resolves by **ADL**: an argument of type `routingmeta::MetadataSink`
  has `routingmeta` among its associated namespaces, so `routingmeta::ProjectMeta`
  is found via the *sink* argument — independent of the request's package
  (`sysN.v1`). The `.proj.h` already `#include`s `common/metadata_sink.h`
  (line 179), so `routingmeta::MetadataSink` is a known type at the declaration.
  → **ADL on the sink does reach `routingmeta`. Correct.**
- Note (complementary, not a conflict): CONTEXT inv. 10 says "the `ProjectMeta`
  overload is chosen by request type." That is **overload resolution** among the
  found candidates (discriminated by the first arg). AD-3's ADL governs **name
  lookup** (which namespace's candidates are found). Two different phases — they
  compose correctly. The spine does not contradict inv. 10.

**No error.** See also Finding L-1 below (FillCommon/Runtime are the *other*
currently-global kit symbols the AD-2 inventory understates).

## 2. AD-7 — single HPACK constant (`32`)  ✅ confirmed exactly

**Claim:** `32` is duplicated — `kHpackEntryOverhead` in `process_context_emit.h`
AND a literal `+32` in `metadata_sink.h` `Add()`.

**Verified:**
- `process_context_emit.h` line 33: `constexpr size_t kHpackEntryOverhead = 32;`
  with the comment "must match the +32 in metadata_sink.h Add()".
- `metadata_sink.h` line 23: `bytes_ += key.size() + value.size() + 32;` with the
  comment "+32 == kHpackEntryOverhead in process_context_emit.h".

→ **Exactly as the spine states.** The two refer to each other by comment but the
value is physically duplicated. The three policy limits (`7168/25/512`) are *not*
duplicated — they live only in `process_context_emit.h` (lines 29–31), matching
AD-7's "live ONLY in `process_context_emit.h`." AD-7 is precise. See Finding L-2
for the include-direction caveat on *where* the single definition should live.

## 3. AD-9 / CR1 — wire frozen  ✅ no undeclared byte change; encoding/digest match SPEC §5–6 exactly

**Does any AD or convention imply a wire-byte change vs SPEC?** No, beyond the one
allowed new header `x-routing-error`:
- AD-5 changes *behavior* (throw → emit `x-routing-error` + return `ProjResult`).
  `ProjResult` is a **return value, not a header** — zero wire impact. The only new
  header is `x-routing-error`, already listed PROVISIONAL in SPEC §2/§7. The empty
  scalar header is absent in both old (throw aborts before emit) and new (skip)
  behavior. Consistent with SPEC §4/§7.
- AD-10 (`void → ProjResult`) is a return-type change — not wire.
- Count/format/digest/overflow semantics: unchanged, see below.

**Canonical-encoding convention vs SPEC §5.1 / §6:**
- Key order: spine `ChamberId, LotID, OperationNO, PartID, RecipeID, StageID, Tech`
  = SPEC §5.1 **exactly**, and matches the runtime sort in
  `protoc-gen-meta.cc` (line 234 `std::sort(cf...)`) over the `(routing.pctx)` keys
  in `process_context.proto`.
- `&`-joined, `Key=value`, empty field → `Key=`: matches SPEC §5.1 and the generated
  loop (lines 239–241; `sep` is `""` for the first field, `"&"` after).
- URL-encoding: spine "RFC 3986 unreserved verbatim, else `%XX` uppercase, space
  `%20`" = SPEC §6 **exactly**; matches `url_encode.h` (unreserved set + uppercase
  `kHex` + per-byte `%XX`).

**Digest vs SPEC §5.3:**
- spine: `sha256:` + hex over emitted contexts joined by `\n`, in emission order;
  receiver recomputes and compares. = SPEC §5.3 **exactly**. Matches
  `process_context_emit.h` lines 65–70 (`canon` = ctxs `\n`-joined → `"sha256:" +
  Sha256Hex(canon)`) and `process_context_parser.h` `VerifyDigest` (same canon rule).

**Error shape:** spine `x-routing-error` value = `missing:<key>` = SPEC §7 exactly.

→ **AD-9 holds. No invented wire change.** (Low nit L-3 on the "each URL-encoded"
wording.)

## 4. AD-5 — failure-as-data  ✅ pivot consistent with SPEC §7 + CONTEXT inv. 9 flagged

**Claim:** the current plugin emits `throw std::runtime_error` for a
required-but-empty scalar; the spine pivots to no-throw → `ProjResult` +
`x-routing-error`.

**Verified `protoc-gen-meta.cc` lines 213–218** (the `pj.required` branch):
```cpp
if ($v$.empty()) throw std::runtime_error("$k$ required");
sink.Add("$k$", routingmeta::UrlEncode($v$));
```
→ **Current throw behavior CONFIRMED.** Also asserted by
`tests/test_projection.cc` lines 116–124 (`assert(threw)`).

**Spine AD-5** — Missing required scalar → `Issue{MissingRequired, key}` + `ok=false`
+ emit `x-routing-error: missing:<key>` + NOT the empty header; Overflow →
`Issue{Overflow}`, `ok` stays `true`, alongside `x-process-context-overflow: true`.
This matches **SPEC §7** point-for-point (record `MissingRequired`/`ok=false`; emit
`x-routing-error: missing:<key>`; suppress the empty scalar; overflow is
non-blocking). **Consistent.**

**CONTEXT inv. 9 still says "throws" — flagged (criterion I), see Finding M-1.**

## 5. Invariant contradictions + Structural Seed vs real tree  ✅

**Structural Seed (spine lines 163–175) vs the real tree** — accurate:
| Seed entry | Real file(s) | Status |
|---|---|---|
| `proto/` metadata_options, process_context, sys1/2/3 | all present | ✅ |
| `src/plugin/` protoc-gen-meta | `protoc-gen-meta.cc` | ✅ |
| `src/common/` metadata_sink, process_context_emit, url_encode, sha256, process_context_parser, common_headers | all present | ✅ |
| `src/common/proj_result.h` `<- NEW` | not present | ✅ correctly flagged NEW |
| `sender/unified_sender.cc` | present | ✅ |
| `receiver/receiver_verify.cc` | present | ✅ |
| `tests/` test_projection.cc + negative/*.proto | present (3 negative fixtures) | ✅ |
| `tests/bench_projection` `(NEW)` | not present | ✅ correctly flagged NEW |
| `.github/workflows/` `(NEW)` | not present | ✅ correctly flagged NEW |

No misnamed paths; no phantom files; every `NEW` marker is justified.

**10 CONTEXT invariants** — the spine contradicts **none** of them as written,
except the deliberate, SPEC-backed supersession of inv. 9's "throws" clause
(Finding M-1). Spot checks: inv. 1 (empty→`Key=`)=AD-1; inv. 2 (common=6 uniform)
matches `FillCommon`; inv. 3/4 (7-field schema, sorted key order)=conventions;
inv. 5/7 (always count+format; count=0 → no digest/lines)=AD-9/emit.h; inv. 6
(digest)=conventions; inv. 8 (`>25`/`>512`/`>7168`, `name+value+32`)=AD-7+emit.h
lines 55–57; inv. 10 (one Send, no branching)=AD-3/AD-4 and matches the actual
`Send` already living in `unified_sender.cc` (so AD-4's "no `Send` symbol in the
kit" is consistent with both inv. 10 and the live code; the conflict AD-4 names is
with PRD FR6, out of this review's SPEC/CONTEXT scope).

---

## Findings

### Critical — none
### High — none

### Medium

**M-1 — CONTEXT inv. 9 + checklist still say "throws"; live doc copies must be
updated under criterion I.**
*Location:* `refs/CONTEXT.md` line 59 ("`required` — `ProjectMeta` throws if the
source field is empty") and line 88 checklist ("Domain scalar (9): … empty ⇒
throws"). *What:* contradicts AD-5 and SPEC §7 ("MUST NOT throw on a data
condition"). *Assessment:* **not a spine defect** — SPEC wins over CONTEXT by both
documents' own precedence rules, so the spine is *correct* to follow no-throw, and
it already routes the doc fix to criterion I ("Docs match code … governed by AD-5,
AD-9"). *Fix:* when the live doc copies are produced, rewrite inv. 9 and the
checklist line from "throws" to "records `MissingRequired` in `ProjResult`
(`ok=false`) and emits `x-routing-error: missing:<key>`, suppressing the empty
scalar." `refs/CONTEXT.md` is read-only; the update lands in the live working copy.
*Side note (not blocking):* criterion I names live `CONTEXT.md / OVERVIEW.zh.md /
README.md` (and SPEC §11 cites `example/TESTING.md`), none of which currently exist
under `example/`. If these are expected deliverables, that is a doc-creation gap
criterion I should own explicitly.

### Low

**L-1 — AD-2 current-state inventory understates which kit symbols are global.**
The spine's AD-2/AD-3 narrative flags only the generated `ProjectMeta` as
currently global. But `src/common/common_headers.h` declares `struct Runtime` and
`inline void FillCommon(...)` at **file scope (global namespace)** too (lines
14–27), and AD-4 explicitly counts `FillCommon` as part of the lib's populate
guarantee — so under AD-2 ("every kit symbol MUST be declared in `namespace
routingmeta`") they must also move. The namespace map (line 51) already places
`common_headers (FillCommon)` under the `routingmeta` kit, so the *intent* is
right; only the current-state inventory is incomplete. Moving them is consistent
and helps: `Send` calls `FillCommon(rt, sink)` unqualified (line 34), which then
also resolves by the same ADL-on-sink mechanism. *Fix:* add FillCommon/Runtime to
the "currently global" list so the namespace move is done completely.

**L-2 — AD-7 single-source location should respect include direction.**
`process_context_emit.h` `#include`s `metadata_sink.h` (line 21), not the reverse.
So the single `32` definition cannot live in `process_context_emit.h` and be used
by `metadata_sink.h` without a circular include. The natural home is the leaf
`metadata_sink.h` (e.g. a `routingmeta::kHpackEntryOverhead` there), referenced by
`process_context_emit.h`. AD-7 says "shared by `metadata_sink.h` and the policy
header" without naming the home — fine, but note the direction so the implementer
puts it in the leaf header.

**L-3 — "each URL-encoded" convention wording vs key encoding.**
The canonical-encoding convention says contexts are "each URL-encoded." The code
URL-encodes the **value** only and emits the **key** as a literal
(`protoc-gen-meta.cc` line 240: `s += "$sep$$k$="; s += UrlEncode(e.$g$);`). No wire
difference — the schema keys (`ChamberId`, `LotID`, …) are all RFC-3986-unreserved,
so encoding them is a no-op, and this matches SPEC §5.1's `Key=UrlEncode(Value)`
form. (SPEC §6 says encode key *and* value, but with unreserved keys the bytes are
identical.) *Fix (optional):* phrase the convention as `Key=UrlEncode(Value)` with
keys being unreserved-by-schema, to match both §5.1 and the code precisely.

**L-4 — AD-3 rationale wording is loose ("ADL being defeated by a proto's package
declaration").** The *rule* (anchor resolution on the always-`routingmeta` sink,
independent of the request package) is technically correct and is what makes the
design robust; the prose rationale is hand-wavy but harmless. No change required.

---

## Bottom line

The spine **ratifies** the real code and the SPEC accurately. The four targeted
claims (AD-2/AD-3 namespace+ADL, AD-7 duplicated `32`, AD-9 wire-frozen with
encoding/digest matching SPEC §5–6 exactly, AD-5 throw→failure-as-data matching
SPEC §7) are all correct against the source. The only behavioral pivot that
diverges from a written invariant — CONTEXT inv. 9 "throws" — is a deliberate,
SPEC-mandated change the spine already tracks via criterion I. Remaining findings
are low-severity completeness/wording items. **PASS-WITH-FINDINGS.**
