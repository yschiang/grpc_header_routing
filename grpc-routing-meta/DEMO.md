# DEMO — an end-to-end session

A copy-paste walkthrough of the kit, from a clean build to every behavior the
BRIEF claims. Each step shows the command, the output that matters, and what it
proves. Criterion letters (A–I) refer to `refs/BRIEF.md`; invariant numbers to
`CONTEXT.md`.

```sh
cd grpc-routing-meta/example
```

---

## 0. Build — `./build.sh`

```
== protoc libprotoc 3.20.3 ==
[plug] protoc-gen-meta
[neg ] codegen must reject malformed (routing.project)
       ok (rejected): bad_dup_key.proto
       ok (rejected): bad_message_project.proto
       ok (rejected): bad_project_under_repeated.proto
       ok (rejected): bad_repeated_scalar.proto
[gen ] sys1.proto (cpp + meta)   sys2.proto   sys3.proto
[app ] unified_sender   receiver_verify
[test] test_projection   [bench] bench_projection
OK -> binaries in .../example/build/
```

**Proves:** portable build, no hardcoded toolchain path (**A**). The `[neg ]`
gate compiles 4 malformed `(routing.project)` fixtures and the build *fails* if
any is accepted — a bad annotation never reaches the wire (**C**/**G**,
fail-loud at codegen). The same steps run in CI on gcc/clang × protobuf
3.20/3.21 (**B**, see `.github/workflows/ci.yml`).

> No cmake locally? `./build.sh` is the direct-`protoc` equivalent; CI runs both.

---

## 1. The sender — `./build/unified_sender`

One `Send<>()` drives all 16 transaction types (sys1×1, sys2×5, sys3×10). Each
block prints its metadata plus `(bytes, ok, duration)`.

### 1a. sys1 — batch process-context

```
=== sys1  Calculate (2 contexts)   (875 bytes, ok=true, 29.38 us) ===
  x-request-id / x-correlation-id / x-contract-version / x-source-system / x-site-id / x-tool-id
  x-process-context-count:   2
  x-process-context-format:  urlencoded-query-string-v1
  x-process-context-digest:  sha256:efafba166aab...
  x-process-context:  ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=PART-A&RecipeID=RCP_ETCH_V3&StageID=ETCH&Tech=N5
  x-process-context:  ChamberId=CH-B&LotID=LOT02&...
```

**Proves:** 6 common headers, uniform (inv. 2). Contexts are **key-sorted**
(`ChamberId` before `LotID`), `&`-joined, values URL-encoded; a digest is
emitted; `count` matches the body's `repeated` size (inv. 4,5,6 — exact
projection, **D**). Per-call `duration` (**H**).

### 1b. sys2 — sparse + count=0

```
=== sys2  Verify (1 sparse context) ===
  x-process-context:  ChamberId=&LotID=&...&RecipeID=RCP_ETCH_V3&...   # empty fields kept as Key=

=== sys2  List (count=0) ===
  x-process-context-count:   0                                        # count + format only, no digest/lines
```

**Proves:** empty body fields project as present-but-empty `Key=`, not dropped
(inv. 1, body-authoritative); `count=0` emits structure only (inv. 7).

### 1c. sys3 — domain scalar, and the no-silent-failure path

```
=== sys3 Submit05 (nested mask id) ===
  x-mask-id:  RET-9981                          # reached via nested job→mask walk

=== sys3 Submit05 (EMPTY mask) ===   ok=false
  x-routing-error:  missing:x-mask-id           # explicit, in-band; NO x-mask-id header; NO throw
  [issue] missing-required x-mask-id            # ProjResult.issues
```

**Proves:** required scalar reached by nested path; when empty, `ProjectMeta`
reports `ok=false` + a `MissingRequired` issue + `x-routing-error`, and does
**not** throw or emit an empty header (inv. 9, **C**/**I** — report, don't throw).

### 1d. Overflow

```
=== sys1  Calculate (60 contexts -> overflow)   ok=true ===
  x-process-context-count:    60
  x-process-context-overflow: true              # lines + digest suppressed; count still emitted
  [issue] overflow
```

**Proves:** size guard fires explicitly instead of letting HTTP/2 reset the
stream; overflow is **non-blocking** (`ok` stays true) (inv. 8, **C**, **F** —
the 7168/25/512 policy lives in one place).

---

## 2. The receiver — `./build/receiver_verify`

```
=== received 2 process-context header(s) ===
  LotID=LOT01 ChamberId=CH-A RecipeID=RCP_ETCH_V3
  LotID=LOT02 ChamberId=CH-B RecipeID=RCP_ETCH_V3

digest check: OK (header matches body)
  expected: sha256:efafba166aab...
  actual:   sha256:efafba166aab...
```

**Proves:** the receiver recomputes the digest over the received contexts and it
matches the sender's byte-for-byte — drift would be caught (inv. 6,
integrity-only, no key/signature; **D**/**I**).

---

## 3. The tests — `./build/test_projection`

```
ALL TESTS PASSED
```

12 assert blocks covering every invariant: url/sha256 primitives, key-sort,
digest round-trip **and tamper-detect**, count=0, overflow by count/bytes/line,
sys3 scalar + missing-required, empty-field faithfulness, the 6 uniform common
headers, the `FillCommon`+`ProjectMeta` compose, and an 8-thread re-entrancy
check (**G**; full table in the test file header). Asserts compile in even under
`-DNDEBUG` (`#undef NDEBUG`, line 1).

---

## 4. The bench — `./build/bench_projection`

```
  1 contexts:   2.053 us/call
  2 contexts:   3.674 us/call
 25 contexts:  28.064 us/call
 60 contexts:  27.471 us/call
BENCH OK (sub-ms)
```

**Proves:** projection cost is observed per call and sub-millisecond across
1/2/25/60 contexts (**H**). The number is `ProjResult::duration`, the same
self-timed value the sender prints.

---

## What the session demonstrates (BRIEF A–I)

| | Where in this session |
|---|---|
| **A** portable build | step 0 — no hardcoded path |
| **B** CI matrix | step 0 steps run in `ci.yml` on gcc/clang × pb 3.20/3.21 |
| **C** no silent failure | 0 (codegen gate) · 1c (`x-routing-error`) · 1d (overflow flag) |
| **D** exact projection | 1a (key-sort/encode/digest) · 2 (round-trip) |
| **E** one sender path | step 1 — one `Send<>()`, all 16 types, zero `if(system==…)` |
| **F** policy centralized | 1d — 7168/25/512 in `process_context_emit.h` |
| **G** testable invariants | step 3 (asserts) · step 0 (negative codegen) |
| **H** perf observed | per-block `us` in step 1 · step 4 bench |
| **I** docs match code | digest=integrity-only, error=`ProjResult` — matches 1c/2 |

The lib's job is the two building blocks (`FillCommon` + generated
`ProjectMeta` → `ProjResult`); composing them into `Send` and deciding
abort-vs-proceed is the **Sender's** (inv. 10). That boundary is what `Send<>()`
in `unified_sender.cc` shows.

---

## 5. Real wire — `grpc_demo/run.sh` (optional)

Steps 1–4 use an in-memory `VectorSink`. This step sends the projected metadata
over a **real HTTP/2 gRPC channel** to a live server, for all three systems.
Needs a local `grpc++` + `grpc_cpp_plugin` (set `GRPC_PREFIX`, default
`~/anaconda3`); run `./build.sh` first.

```sh
./grpc_demo/run.sh
```

Client side = the Sender: one `GrpcSink` + `ProjectMeta(req, sink)` per call, no
per-system branch. Server side = one `VerifyWire()` reading
`ServerContext::client_metadata()`, the same digest recompute as
`receiver_verify`, for all 16 RPCs.

```
[server] sys1.Calculate   count=2     digest OK (no drift in transit)
[server] sys2.Verify      count=1     digest OK
[server] sys2.List        count=0     OK (no process-context to verify)
[server] sys3.Submit05    count=0  x-mask-id=RET-9981   OK
[server] sys3.Submit05    count=0     REJECT x-routing-error=missing:x-mask-id
[server] sys1.Calculate   count=2     digest MISMATCH (drift!)
[client] sys3.empty     sent ok=0 -> ERR routing-error: missing:x-mask-id
[client] sys1.tampered  sent (tampered) -> ERR digest drift
```

**Proves on a real wire:** the 3 Layer-3 shapes (batch / sparse+count=0 /
domain scalar), `GrpcSink` exercised for real (not CI compile-smoke), the
missing-required path rejected with a non-OK gRPC status (**C**), and digest
drift introduced *in transit* caught server-side (inv. 6, **D**). This is
demo-only and **not** wired into `build.sh`/CI — the portable kit keeps gRPC
optional.
