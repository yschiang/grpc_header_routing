# 03 Codegen Walkthrough — how `ProjectMeta()` is generated

> Companion to `02_grpc_routing_metadata_spec.md`. The spec defines *what* headers
> appear on the wire; this doc explains *how* the kit generates the code that
> produces the body-derived ones, so you never hand-write projection logic. This is
> the implementation tooling called "reviewed separately / out of scope" in
> `EA_DESIGN_SUMMARY.md` §1.

## TL;DR

You **tag a `.proto` field** with "this becomes header X"; at build time the
`protoc-gen-meta` plugin reads the tag and **writes the C++** that pulls the value
out of the body, URL-encodes it, sorts it, digests it, and size-guards it. The body
stays the single source of truth; the header can never silently drift from it.

```
                         ┌──────────────────────────────────────────────┐
  ① define the "tag"      │ proto/metadata_options.proto                  │
                         │   project / pctx  = FieldOptions ext 50005/6  │
                         └────────────────┬─────────────────────────────┘
                          ┌────────────────▼─────────────────────────────┐
  ② shared contract       │ proto/process_context.proto                   │
                         │   ProcessContext { 7 pctx-tagged fields }     │
                         └────────────────┬─────────────────────────────┘
                          ┌────────────────▼─────────────────────────────┐
  ③ tag the data          │ examples/proto/{sys1,sys2,sys3}.proto           │
                         │   repeated ProcessContext  (+ mask: x-mask-id)│
                         └────────────────┬─────────────────────────────┘
        ┌─────────────────────────────────┼─────────────────────────────────┐
        │ protoc runs twice per system  (CMakeLists.txt / build.sh)            │
   ④ --cpp_out                       ⑤ --meta_out  (= our plugin)            │
        │                                 │                                   │
   {sys}.pb.{h,cc}                  protoc-gen-meta.cc reads tags, prints code │
   (message classes)                     ▼                                   │
        │                          {sys}.proj.{h,cc} = generated ProjectMeta() │
        └─────────────────┬───────────────┴─────────────────┬─────────────────┘
                          ▼                                 ▼
                   ⑥ unified_sender / receiver / test compiled & linked
```

**Plugin mechanism:** `protoc` sees the unknown flag `--meta_out`, runs an
executable named `protoc-gen-meta`, hands it the parsed proto over stdin, and takes
its stdout as generated files. A plugin = a program that *reads proto structure and
prints strings*.

---

## ① Define what a "tag" looks like — `proto/metadata_options.proto`

```proto
// proto/metadata_options.proto:9-22
message ProjectField { string key = 1; bool required = 2; }  // scalar -> single header
message CtxField     { string key = 1; }                     // one field of a context
extend google.protobuf.FieldOptions {
  ProjectField project = 50005;   // "project me into header `key`"
  CtxField     pctx    = 50006;   // "I am one cell of a process-context"
}
```

## ② Tag the shared process-context once — `proto/process_context.proto`

`ProcessContext` is defined once and imported by every system, so the schema and
header keys can't drift between sys1 / sys2 / sys3.

```proto
// proto/process_context.proto
message ProcessContext {
  string lot_id     = 1 [(routing.pctx) = {key: "LotID"}];
  string chamber_id = 7 [(routing.pctx) = {key: "ChamberId"}];
  // ... recipe_id/tech/part_id/stage_id/operation_no
}
```

## ③ Tag the data per system — `examples/proto/sys1.proto`, `sys3.proto`

```proto
// sys1.proto — only the universal process-context
message CalculateRequest {
  string tool_id = 1;                                 // NOT tagged -> sender fills x-tool-id
  repeated common.v1.ProcessContext contexts = 2;     // -> x-process-context
}

// sys3.proto — a domain scalar buried at a nested path, + the universal context
message Submit05Request {
  message Mask { string mask_id = 1 [(routing.project) = {key: "x-mask-id", required: true}]; }
  message Job  { Mask mask = 1; }
  Job job = 1;                                         // mask id lives at job.mask.mask_id
  repeated common.v1.ProcessContext contexts = 2;
}
```

> **Untagged fields are never touched.** `tool_id` is sender-known (spec §7); the
> sender fills `x-tool-id` directly. The kit only projects what is tagged.

---

## ④⑤ How the plugin reads tags and prints code — `src/plugin/protoc-gen-meta.cc`

**(a) Scalar tags, auto-walking nested paths — `walkProj()`**

```cpp
// src/plugin/protoc-gen-meta.cc:40-51
void walkProj(const Descriptor* d, const std::string& prefix, std::vector<Proj>* out) {
  for (each field f) {
    if (f is a sub-message)                 // nested: recurse, prefix += "job()."
      walkProj(f->message_type(), prefix + f->name() + "().", out);
    else if (f has (routing.project))       // tag found
      out->push_back({key, required, prefix + f->name() + "()"});   // record how to reach it
  }
}
```
For `Submit05Request` this yields the getter `job().mask().mask_id()` — the plugin
assembles the **nested access path for you**.

**(b) Batch-context tags, key-sorted — `FindCtx()` + `std::sort`**

```cpp
// src/plugin/protoc-gen-meta.cc:120-127
for (each field sf in the repeated ProcessContext message)
  if (sf has (routing.pctx)) cf.push_back({key, sf->name() + "()"});
std::sort(cf.begin(), cf.end());            // key-sort -> canonical, so the digest is stable
```

**(c) Emit C++: build the context strings, then defer policy to a shared helper.**
The field-extraction loop is generated (fields differ per message); count / format /
digest / overflow / the **7 KB size guard** all live in one hand-written helper, so
the policy isn't baked into every generated file.

```cpp
// src/plugin/protoc-gen-meta.cc — scalar (one Add) then a single helper call
p.Print("  if ($v$.empty()) throw ...(\"$k$ required\");\n", ...);
p.Print("  if (!$v$.empty()) sink.Add(\"$k$\", UrlEncode($v$));\n", ...);
// ... generate the per-field `s += \"Key=\"; s += UrlEncode(e.field());` loop ...
p.Print("    routingmeta::EmitProcessContexts(sink, ctxs);\n");
```

---

## ⑥ What gets generated — `sys1.proj.cc` (real output)

```cpp
// build/generated/sys1.proj.cc  (AUTO-GENERATED)
void ProjectMeta(const sys1::v1::CalculateRequest& req, routingmeta::MetadataSink& sink) {
  {
    std::vector<std::string> ctxs;
    for (const auto& e : req.contexts()) {            // count is dynamic = body
      std::string s;
      s += "ChamberId=";    s += UrlEncode(e.chamber_id());   // already key-sorted:
      s += "&LotID=";       s += UrlEncode(e.lot_id());       //   ChamberId < LotID < ...
      s += "&OperationNO="; /* PartID, RecipeID, StageID, Tech ... */
      ctxs.push_back(std::move(s));
    }
    routingmeta::EmitProcessContexts(sink, ctxs);     // count/format/digest/overflow + 7KB guard
  }
}
```

The sys3 overload additionally projects the scalar, with the nested path assembled
for it:

```cpp
// build/generated/sys3.proj.cc
void ProjectMeta(const sys3::v1::Submit05Request& req, routingmeta::MetadataSink& sink) {
  if (req.job().mask().mask_id().empty()) throw std::runtime_error("x-mask-id required");
  if (!req.job().mask().mask_id().empty()) sink.Add("x-mask-id", UrlEncode(req.job().mask().mask_id()));
  { /* same context loop -> EmitProcessContexts(sink, ctxs); */ }
}
```

### The policy that lives in the helper — `src/common/process_context_emit.h`

`EmitProcessContexts` always emits `count` + `format`; suppresses everything when
`count == 0`; and falls back to an explicit `x-process-context-overflow: true` (no
lines, no digest) when `count > 25`, any single context `> 512 B`, **or** the whole
running metadata `> 7 KB` (measured via the byte-tracking `MetadataSink`). The byte
guard is independent of the count cap — 25 wide contexts can still exceed the gRPC
limit, which would otherwise make APISIX silently reset/truncate the stream.

### Tag → generated line

| Tag in the proto | Plugin logic | Generated code |
|---|---|---|
| `mask_id [(routing.project)={x-mask-id, required}]` under `job.mask` | `walkProj` recurses, builds path | `req.job().mask().mask_id()` → `sink.Add("x-mask-id", ...)` |
| `repeated common.v1.ProcessContext` (7 pctx fields) | `FindCtx` + `sort` | `s += "...&LotID="; ...` then `EmitProcessContexts(sink, ctxs)` |
| `tool_id` (untagged) | skipped | (nothing emitted) |

---

## Why this codegen earns its keep — `examples/proto/sys3.proto`

The sys3 system's 10 methods carry `x-mask-id` at a **different body path each
time** (top-level / renamed / nested / sub-message):

```proto
// examples/proto/sys3.proto
message Submit01Request { string mask_id = 1 ... }                                  // top-level
message Submit02Request { string reticle_id = 1 ... }                               // renamed
message Submit05Request { ... Job job = 1; ... }                                    // nested 2 levels
message Submit10Request { ... Exposure exposure = 1; ... }                          // another nested path
```

One tag line per method, and `walkProj` generates the matching getter
(`req.mask_id()` / `req.reticle_id()` / `req.job().mask().mask_id()` /
`req.exposure().mask().id()` …). Nobody tracks how deep the value is or what it was
renamed to — the error-prone "where does the value live" problem is solved once, at
build time, for all 16 transactions across the three systems.

## Adding a transaction / system

1. Add the request to your system proto: `import "process_context.proto"` and a
   `repeated common.v1.ProcessContext contexts`. For a domain scalar, tag it with
   `(routing.project)` and `import "metadata_options.proto"`.
2. Add the proto stem to the codegen list in `CMakeLists.txt` / `build.sh`.
3. Rebuild. `ProjectMeta(const YourRequest&, MetadataSink&)` appears; call it from
   the same `Send<>()`.

## Build / verify

`cmake -S . -B build && cmake --build build` (or `./build.sh` when cmake is absent
— a direct protoc + clang build mirroring the same steps). Verified end-to-end on
protobuf 3.20.3 and 3.21.12: plugin builds, three systems codegen, `unified_sender`
projects all 16 transactions, `receiver_verify` re-checks the digest, and
`test_projection` covers projection, round-trip, tamper, count/byte overflow,
`count=0`, and nested scalar projection.
