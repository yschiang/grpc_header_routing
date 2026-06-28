# Optional process-context digest (send-time) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `x-process-context-digest` optional via a **send-time** argument `emit_digest` (default `true` = current behavior), and make the receiver verify-if-present (absent digest = skip + OK, not drift).

**Architecture:** `EmitProcessContexts` gains `bool emit_digest = true`; a runtime `if (emit_digest)` gates only the digest `sink.Add`. The sender reaches it through the generated `ProjectMeta`, so the codegen plugin (`protoc-gen-meta.cc`) threads the bool through into a uniform `ProjectMeta(req, sink, bool emit_digest = true)` for every system. The receiver side is runtime regardless: `VerifyDigest` treats an absent digest as "skip → OK". Both digest paths are exercised in one build (no `-D`, no second compile) — `ProjectMeta(req, sink, false)` in the test.

**Tech Stack:** C++17, protoc plugin codegen, plain-assert tests (no framework), bash `build.sh`.

## Global Constraints

- **CLAUDE.md:** no `Co-Authored-By` trailer; concise concrete commit messages; commit locally as you go; **do not push**; stay in this workspace; do not read `refs/` superseded docs / `archive/`.
- **TDD:** failing test first for every behavior change; pure refactors stay green.
- **Default = current behavior:** `emit_digest` defaults to `true`. Every existing 2-arg call site (`ProjectMeta(req, sink)`) keeps emitting the digest, byte-for-byte unchanged.
- **Do not move the byte budget:** count / format / context lines / overflow must be identical with `emit_digest` true or false. The digest header is **pre-charged** against the 7 KB guard unconditionally.
- **Uniform signature:** every generated `ProjectMeta` takes the `emit_digest` param (even scalar-only systems like sys3, where it is unused) so a generic sender can pass it without per-system branching. `-Wall` (no `-Wextra`) does not warn on the unused param.
- **One build, both paths green:** `cd grpc-routing-meta/example && ./build.sh` compiles once and runs `test_projection` (which exercises digest on AND off) + `receiver_verify`.
- **Deviation from `refs/`:** making the digest MUST→MAY deviates from `refs/SPEC.md` §5.3 — record it as ADR 0002 (Task 5). Do not edit `refs/` (read-only); edit only the kit's own `grpc-routing-meta/SPEC.md`.

All paths below are relative to `grpc-routing-meta/`. `receiver/receiver_verify.cc` and `sender/unified_sender.cc` are **unchanged**: they call the 2-arg `ProjectMeta` (default `true`), and verify-if-present is inherited from `VerifyDigest` (Task 1).

---

### Task 1: `VerifyDigest` — verify-if-present (absent digest → OK, not error)

**Files:**
- Modify: `example/src/common/process_context_parser.h:74-95`
- Test: `example/tests/test_projection.cc:281`

**Interfaces:**
- Consumes: `VerifyResult VerifyDigest(const std::vector<std::string>& contexts, const std::string& received_digest)` (existing signature, unchanged).
- Produces: new semantics — `received_digest.empty()` ⇒ `VerifyResult{ ok = true }` (skip, no error). A present-but-mismatched digest still ⇒ `ok = false, error = "digest mismatch: header/body projection drift"`.

- [ ] **Step 1: Flip the existing empty-digest assertion to the new policy**

In `example/tests/test_projection.cc`, the parser-robustness block ends with (line 281):

```cpp
    assert(!VerifyDigest({"ChamberId=CH-A"}, "").ok);  // empty digest -> not-ok, no crash
```

Change it to:

```cpp
    assert(VerifyDigest({"ChamberId=CH-A"}, "").ok);   // absent digest -> OK (verify-if-present), no crash
```

(Leave the line above it — `VerifyDigest({"ChamberId=CH-A"}, "sha256:not-a-real-digest")` → `!ok` — unchanged: a present-but-bogus digest must still reject.)

- [ ] **Step 2: Build and run the test to verify it FAILS**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh && ./build/test_projection
```
Expected: compiles, then `./build/test_projection` aborts on the flipped assertion (`VerifyDigest(…, "").ok` is currently `false`) — exits non-zero, no `ALL TESTS PASSED`.

- [ ] **Step 3: Change the parser empty-digest branch to skip → OK**

In `example/src/common/process_context_parser.h`, replace the empty-digest branch (lines 87-93):

```cpp
  if (received_digest.empty()) {
    r.error = "no digest provided (overflow or sender omitted)";
    r.ok = false;
  } else {
    r.ok = (r.actual_digest == received_digest);
    if (!r.ok) r.error = "digest mismatch: header/body projection drift";
  }
  return r;
```

with:

```cpp
  if (received_digest.empty()) {
    // Verify-if-present: an ABSENT digest is not drift. The sender may have omitted it
    // (emit_digest=false), or overflow suppressed it — there is nothing to recompute
    // against, so accept. A present-but-wrong digest still rejects below.
    r.ok = true;
    return r;
  }
  r.ok = (r.actual_digest == received_digest);
  if (!r.ok) r.error = "digest mismatch: header/body projection drift";
  return r;
```

Also update the function's doc comment (lines 74-76) — replace "and compare to the received x-process-context-digest. Detects drift between header and body." with "and compare to the received x-process-context-digest **if one is present** (verify-if-present); an absent digest is skipped, not treated as drift. Detects drift between header and body."

- [ ] **Step 4: Build and run the test to verify it PASSES**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh && ./build/test_projection
```
Expected: prints `ALL TESTS PASSED`, exits 0.

- [ ] **Step 5: Commit**

```bash
cd /Users/johnson.chiang/workspace/ab-superpowers
git add grpc-routing-meta/example/src/common/process_context_parser.h grpc-routing-meta/example/tests/test_projection.cc
git commit -m "feat(parser): VerifyDigest is verify-if-present (absent digest = OK, not drift)"
```

---

### Task 2: `EmitProcessContexts` — `emit_digest` argument

**Files:**
- Modify: `example/src/common/process_context_emit.h:35-69`
- Test: `example/tests/test_projection.cc:211-225`

**Interfaces:**
- Consumes: `MetadataSink`, `Sha256Hex` (unchanged).
- Produces: `bool EmitProcessContexts(MetadataSink& sink, const std::vector<std::string>& ctxs, bool emit_digest = true)`. When `emit_digest == false`, no `x-process-context-digest` is added; count / format / context lines / overflow are unchanged (digest pre-charge stays unconditional).

- [ ] **Step 1: Extend the EmitProcessContexts test block with digest-off sub-cases (failing test)**

In `example/tests/test_projection.cc`, replace the whole `EmitProcessContexts` block (lines 211-225):

```cpp
  // --- EmitProcessContexts returns overflow signal (Task 1) ---
  {
    std::vector<std::string> few(2, "ChamberId=CH-A");
    routingmeta::VectorSink sink;
    bool of = routingmeta::EmitProcessContexts(sink, few);
    assert(!of);                                                  // within budget
    assert(sink.Get("x-process-context-digest").rfind("sha256:", 0) == 0);

    std::vector<std::string> many(30, "ChamberId=CH-A");
    routingmeta::VectorSink sink2;
    bool of2 = routingmeta::EmitProcessContexts(sink2, many);
    assert(of2);                                                  // count > 25
    assert(sink2.Get("x-process-context-overflow") == "true");
    assert(sink2.Count("x-process-context") == 0);
  }
```

with:

```cpp
  // --- EmitProcessContexts: overflow signal + send-time digest toggle ---
  {
    std::vector<std::string> few(2, "ChamberId=CH-A");
    routingmeta::VectorSink sink;
    bool of = routingmeta::EmitProcessContexts(sink, few);         // default: digest ON
    assert(!of);                                                  // within budget
    assert(sink.Get("x-process-context-digest").rfind("sha256:", 0) == 0);

    routingmeta::VectorSink sink_off;                             // send-time: digest OFF
    bool of_off = routingmeta::EmitProcessContexts(sink_off, few, /*emit_digest=*/false);
    assert(!of_off);
    assert(sink_off.Get("x-process-context-digest").empty());     // no digest header
    assert(sink_off.Count("x-process-context") == 2);             // context lines unaffected
    assert(sink_off.Get("x-process-context-count") == "2");       // count unaffected
    assert(sink_off.Get("x-process-context-format") == "urlencoded-query-string-v1");

    std::vector<std::string> many(30, "ChamberId=CH-A");
    routingmeta::VectorSink sink2;
    bool of2 = routingmeta::EmitProcessContexts(sink2, many);
    assert(of2);                                                  // count > 25
    assert(sink2.Get("x-process-context-overflow") == "true");
    assert(sink2.Count("x-process-context") == 0);

    routingmeta::VectorSink sink2_off;                            // overflow identical with digest OFF
    bool of2_off = routingmeta::EmitProcessContexts(sink2_off, many, /*emit_digest=*/false);
    assert(of2_off);                                              // pre-charge unconditional -> same decision
    assert(sink2_off.Get("x-process-context-overflow") == "true");
    assert(sink2_off.Count("x-process-context") == 0);
  }
```

- [ ] **Step 2: Build to verify it FAILS (no 3-arg overload yet)**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh
```
Expected: `build.sh` fails at the `[test] test_projection` step with a compile error — `no matching function for call to 'EmitProcessContexts'` (called with 3 args; only 2-arg exists). Non-zero exit.

- [ ] **Step 3: Add the `emit_digest` parameter and gate the digest emit**

In `example/src/common/process_context_emit.h`, change the signature (line 38):

```cpp
inline bool EmitProcessContexts(MetadataSink& sink, const std::vector<std::string>& ctxs) {
```

to:

```cpp
inline bool EmitProcessContexts(MetadataSink& sink, const std::vector<std::string>& ctxs,
                                bool emit_digest = true) {
```

Update the block comment just above it (lines 35-37) — replace "Returns true iff context lines + digest were suppressed due to overflow." with "`emit_digest` (default true) chooses whether to emit the sha256 digest header — a send-time integrity toggle; the context lines, count, format, and overflow decision are unaffected by it. Returns true iff context lines were suppressed due to overflow."

Update the pre-charge comment (lines 44-46) — replace:

```cpp
  // Pre-charge the digest header we'll emit below: name "x-process-context-digest"(24)
  // + value "sha256:"(7) + 64 hex = 71, + one HPACK entry.
  size_t projected = 24 + 71 + kHpackEntryOverhead;
```

with:

```cpp
  // Pre-charge the digest header UNCONDITIONALLY (even when emit_digest=false): name
  // "x-process-context-digest"(24) + value "sha256:"(7) + 64 hex = 71, + one HPACK entry.
  // Charging it whether or not it's emitted keeps the overflow decision identical either
  // way — the digest toggle must not move the byte budget.
  size_t projected = 24 + 71 + kHpackEntryOverhead;
```

Gate the digest emit. Replace lines 61-67:

```cpp
  std::string canon;
  for (size_t i = 0; i < ctxs.size(); ++i) {
    if (i) canon.push_back('\n');
    canon += ctxs[i];
  }
  sink.Add("x-process-context-digest", "sha256:" + Sha256Hex(canon));
  for (const auto& c : ctxs) sink.Add(kKey, c);
  return false;
```

with:

```cpp
  if (emit_digest) {
    std::string canon;
    for (size_t i = 0; i < ctxs.size(); ++i) {
      if (i) canon.push_back('\n');
      canon += ctxs[i];
    }
    sink.Add("x-process-context-digest", "sha256:" + Sha256Hex(canon));
  }
  for (const auto& c : ctxs) sink.Add(kKey, c);
  return false;
```

- [ ] **Step 4: Build and run to verify it PASSES**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh && ./build/test_projection
```
Expected: prints `ALL TESTS PASSED`, exits 0. (The generated `ProjectMeta` still calls the 2-arg form via the default, so digest-on behavior elsewhere is unchanged.)

- [ ] **Step 5: Commit**

```bash
cd /Users/johnson.chiang/workspace/ab-superpowers
git add grpc-routing-meta/example/src/common/process_context_emit.h grpc-routing-meta/example/tests/test_projection.cc
git commit -m "feat(emit): EmitProcessContexts gains send-time emit_digest (default on); overflow budget unchanged"
```

---

### Task 3: Codegen — thread `emit_digest` through `ProjectMeta`

**Files:**
- Modify: `example/src/plugin/protoc-gen-meta.cc:187, 212, 252`
- Test: `example/tests/test_projection.cc` (add a send-time digest-off block after the sys1 projection block, ~line 113)

**Interfaces:**
- Consumes: `EmitProcessContexts(sink, ctxs, emit_digest)` (Task 2).
- Produces: every generated `ProjectMeta` is uniformly `ProjResult ProjectMeta(const <Sys>& req, MetadataSink& sink, bool emit_digest = true)`; passing `false` omits the digest while emitting count / format / context lines / overflow unchanged.

- [ ] **Step 1: Add the send-time ProjectMeta-off test block (failing test)**

In `example/tests/test_projection.cc`, immediately after the closing `}` of the sys1 projection block (the block that ends with the drift assert, around line 113) and before the `// --- count=0` block, insert:

```cpp
  // --- send-time digest OFF via ProjectMeta(req, sink, false): contexts emitted, no digest,
  //     count / format / overflow unaffected, receiver verify-if-present returns OK (inv. 6) ---
  {
    auto req = sys1Req(2);
    routingmeta::VectorSink sink;
    routingmeta::ProjResult r = ProjectMeta(req, sink, /*emit_digest=*/false);
    assert(r.ok);
    assert(sink.Get("x-process-context-digest").empty());           // no digest header
    assert(sink.Count("x-process-context") == 2);                   // context lines unaffected
    assert(sink.Get("x-process-context-count") == "2");             // count unaffected
    assert(sink.Get("x-process-context-format") == "urlencoded-query-string-v1");
    std::vector<std::string> cs;
    for (auto& kv : sink.items)
      if (kv.first == "x-process-context") cs.push_back(kv.second);
    assert(routingmeta::VerifyDigest(cs, sink.Get("x-process-context-digest")).ok);  // absent -> OK
  }
```

- [ ] **Step 2: Build to verify it FAILS (ProjectMeta is still 2-arg)**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh
```
Expected: `build.sh` fails at the `[test] test_projection` step with a compile error — `no matching function for call to 'ProjectMeta'` (called with 3 args; generated `ProjectMeta` takes 2). Non-zero exit.

- [ ] **Step 3: Thread `emit_digest` through the generated code (3 edits)**

In `example/src/plugin/protoc-gen-meta.cc`:

**(a)** `.proj.h` declaration (line 187) — add the defaulted param. Replace:

```cpp
        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink);\n",
                "ns", ns, "m", d->name());
```

with:

```cpp
        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink, bool emit_digest = true);\n",
                "ns", ns, "m", d->name());
```

**(b)** `.proj.cc` definition (line 212) — same param, **no** default (the `.h` carries it; repeating it is an error). Replace:

```cpp
        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink) {\n"
                "  ProjResult _r;\n"
                "  const auto _t0 = std::chrono::steady_clock::now();\n",
                "ns", ns, "m", d->name());
```

with:

```cpp
        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink, bool emit_digest) {\n"
                "  ProjResult _r;\n"
                "  const auto _t0 = std::chrono::steady_clock::now();\n",
                "ns", ns, "m", d->name());
```

**(c)** The `EmitProcessContexts` call (line 252) — pass the bool. Replace:

```cpp
          p.Print("    if (EmitProcessContexts(sink, ctxs)) _r.issues.push_back({Issue::Overflow, \"\"});\n  }\n");
```

with:

```cpp
          p.Print("    if (EmitProcessContexts(sink, ctxs, emit_digest)) _r.issues.push_back({Issue::Overflow, \"\"});\n  }\n");
```

> Note: scalar-only systems (sys3) generate `ProjectMeta(req, sink, bool emit_digest)` with `emit_digest` unused. `build.sh` uses `-Wall` (not `-Wextra`), so there is no unused-parameter warning; the uniform signature is what lets a generic sender pass the flag without per-system branching.

- [ ] **Step 4: Build and run to verify it PASSES**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh && ./build/test_projection
```
Expected: `build.sh` rebuilds the plugin, regenerates every `*.proj.{h,cc}` with the 3-arg signature, recompiles, then `./build/test_projection` prints `ALL TESTS PASSED`, exits 0.

- [ ] **Step 5: Commit**

```bash
cd /Users/johnson.chiang/workspace/ab-superpowers
git add grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc grpc-routing-meta/example/tests/test_projection.cc
git commit -m "feat(codegen): thread send-time emit_digest through generated ProjectMeta (default on)"
```

---

### Task 4: `build.sh` — run the test + receiver as the green gate

**Files:**
- Modify: `example/build.sh:110`

**Interfaces:**
- Consumes: `BIN` (already defined in `build.sh`).
- Produces: `./build.sh` compiles once, then runs `test_projection` (which covers digest on AND off) and `receiver_verify`. Exits non-zero if either fails. No second build / no `-D` pass — the send-time toggle is a runtime arg.

- [ ] **Step 1: Add the run lines to `build.sh`**

In `example/build.sh`, replace the final line (line 110):

```bash
echo "OK -> binaries in $BIN/"
```

with:

```bash
# 5. run the green gate. The send-time digest toggle is exercised in-process by
#    test_projection (ProjectMeta(req, sink, false)); no second build is needed.
echo "[test] run test_projection"
"$BIN/test_projection"
echo "[recv] run receiver_verify"
"$BIN/receiver_verify" >/dev/null

echo "OK -> binaries in $BIN/ (digest on + off both covered)"
```

- [ ] **Step 2: Run `./build.sh` to verify the gate is green**

Run:
```bash
cd grpc-routing-meta/example && ./build.sh
```
Expected: ends with `ALL TESTS PASSED` (from the `[test]` line) and `OK -> binaries in …/build/ (digest on + off both covered)`, exit 0. (`receiver_verify` exits 0 — its digest-on round-trip still catches tampering; output suppressed.)

- [ ] **Step 3: Commit**

```bash
cd /Users/johnson.chiang/workspace/ab-superpowers
git add grpc-routing-meta/example/build.sh
git commit -m "build: run test_projection + receiver_verify as the green gate"
```

---

### Task 5: Docs + ADR

**Files:**
- Modify: `SPEC.md:34` (header table) and `SPEC.md:84-89` (§5.3)
- Modify: `CONTEXT.md:56` (invariant 6)
- Modify: `OVERVIEW.zh.md:119` (Receiver drift row)
- Create: `docs/adr/0002-optional-process-context-digest.md`

**Interfaces:** none (docs only). No test cycle — reviewable as one unit.

- [ ] **Step 1: SPEC.md header table — digest presence condition**

In `SPEC.md`, replace line 34:

```markdown
| `x-process-context-digest` | pctx | `sha256:` + 64 hex (§5.3) | iff count>0 and not overflow |
```

with:

```markdown
| `x-process-context-digest` | pctx | `sha256:` + 64 hex (§5.3) | iff count>0, not overflow, and the sender requested it (`emit_digest`, default yes) |
```

- [ ] **Step 2: SPEC.md §5.3 — MUST emit → send-time choice; verifier verifies if present**

In `SPEC.md`, replace the first paragraph of §5.3 (lines 86-89):

```markdown
When context lines are emitted, the sender MUST emit
`x-process-context-digest = "sha256:" + SHA256_hex(C)`, where `C` is the emitted
`x-process-context` values joined by `\n` (newline) in emission order. The verifier MUST
recompute and compare.
```

with:

```markdown
When context lines are emitted, the sender emits
`x-process-context-digest = "sha256:" + SHA256_hex(C)` **by default**, where `C` is the emitted
`x-process-context` values joined by `\n` (newline) in emission order. Emission is a per-call
sender choice: `ProjectMeta(req, sink, emit_digest)` (default `true`) — passing `false` omits
the digest while still emitting the context lines. The verifier verifies **if present**: when
`x-process-context-digest` is present it MUST recompute and compare and reject on mismatch; when
it is **absent** the verifier MUST treat it as "not verified", **not** as drift, and accept.
(See ADR 0002.)
```

- [ ] **Step 3: CONTEXT.md invariant 6 — optionality + verify-if-present**

In `CONTEXT.md`, replace invariant 6 (line 56):

```markdown
6. **Digest = consistency.** When contexts are emitted, `x-process-context-digest = "sha256:" + sha256(contexts joined by '\n')`. The receiver recomputes and compares; mismatch ⇒ header/body drift (sender bug, sender↔verifier version skew, or transit mangling). Integrity check, not a security control — no key, no signature (SPEC §5.3).
```

with:

```markdown
6. **Digest = consistency (optional).** When contexts are emitted, `x-process-context-digest = "sha256:" + sha256(contexts joined by '\n')` — unless the sender passes `emit_digest=false` to `ProjectMeta` (default `true`), which omits it. The receiver verifies **if present**: a present digest is recomputed and compared (mismatch ⇒ header/body drift — sender bug, sender↔verifier version skew, or transit mangling); an absent digest is skipped, not treated as drift. The digest pre-charge against the 7 KB guard is unconditional, so the overflow decision is identical whether or not it is emitted. Integrity check, not a security control — no key, no signature (SPEC §5.3, ADR 0002).
```

- [ ] **Step 4: OVERVIEW.zh.md — Receiver drift row**

In `OVERVIEW.zh.md`, replace line 119:

```markdown
| **Receiver** | header / body 漂移 | 用 `x-process-context-digest` **重算比對**,不符即 reject |
```

with:

```markdown
| **Receiver** | header / body 漂移 | **有 digest 才驗**:`x-process-context-digest` 在就重算比對、不符即 reject;不在就跳過(不算漂移)。預設帶,`ProjectMeta(req, sink, false)` 可逐次關 |
```

- [ ] **Step 5: Create ADR 0002**

Create `docs/adr/0002-optional-process-context-digest.md`:

```markdown
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
```

- [ ] **Step 6: Commit**

```bash
cd /Users/johnson.chiang/workspace/ab-superpowers
git add grpc-routing-meta/SPEC.md grpc-routing-meta/CONTEXT.md grpc-routing-meta/OVERVIEW.zh.md grpc-routing-meta/docs/adr/0002-optional-process-context-digest.md
git commit -m "docs(digest): optional send-time digest + verify-if-present (SPEC §5.3, CONTEXT inv.6, OVERVIEW, ADR 0002)"
```

---

## Self-Review

**Spec coverage (against the send-time request):**
- *Emit toggle, default on, only the digest `Add` gated, rest unchanged* → Task 2 (`emit_digest` runtime `if`). Overflow held identical via unconditional pre-charge (asserted in Task 2 Step 1, `sink2_off`).
- *Sender controls it at send time* → Task 3 threads `emit_digest` through the generated `ProjectMeta`.
- *Verify-if-present (absent → OK, not drift)* → Task 1 (`VerifyDigest`). Inherited by `receiver_verify` and any receiver with no source change.
- *Tests: digest-on green; digest-off via `ProjectMeta(req, sink, false)` — no digest header, contexts/count/format normal, verify returns OK, overflow unaffected* → Task 2 (helper) + Task 3 (`ProjectMeta`) blocks, all in one build.
- *Docs: SPEC §5, CONTEXT invariant 6, OVERVIEW* → Task 5. Plus ADR 0002 for the `refs/` deviation (established pattern, ADR 0001), with the send-time-vs-compile-time rationale recorded.
- *`./build.sh` green* → Task 4 (one build runs both paths).
- *CLAUDE.md (no co-author, local commit, no push, stay in workspace)* → Global Constraints; every commit is local, no trailer.

**Placeholder scan:** none — every code/edit step shows exact old→new text and exact run commands.

**Type/name consistency:** `emit_digest` (bool, default `true`) is named identically in `EmitProcessContexts` (Task 2), the generated `ProjectMeta` declaration/definition/call (Task 3), and the docs (Task 5). The definition omits the default that the declaration carries (C++ one-default rule). `VerifyDigest` verify-if-present semantics (Task 1) are what make the Task 3 `assert(VerifyDigest(cs, "").ok)` pass.

**Edge cases handled:**
- *Conflicting assertion:* old `assert(!VerifyDigest({…}, "").ok)` (test:281) is flipped in Task 1 Step 1 — the RED that drives the parser change.
- *Unused param on scalar-only systems:* called out in Task 3 — `-Wall` without `-Wextra` does not warn; the uniform signature is intentional.
- *Default keeps wire identical:* every 2-arg call site (receiver, sender, existing tests) is unchanged; no `#if`/`-D` test surgery needed (unlike the compile-time alternative).
