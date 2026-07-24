# sys4 UpdateMaterialRequest Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the sys4 `UpdateMaterialRequest` adoption example (proto + producer-side `FillContexts` + three test cases + build integration) so every claim in `grpc-routing-meta/docs/sys4-update-material.zh.md` is executable.

**Architecture:** sys4 follows the sys1 `[+meta]` pattern — three unannotated operation messages, one added `repeated common.v1.ProcessContext contexts` field, producer-side normalization (`FillContexts`) that maps `new_mat_lot_id` → `LotID` and fail-louds on exclusive-source violation. Kit and `Send<>()` are untouched (ADR 0001, CONTEXT.md principle #10).

**Tech Stack:** proto3 + protoc-gen-meta codegen (existing), C++17, plain-assert tests (`test_projection.cc` style, zero test deps).

## Global Constraints

- Working dir for all commands: `grpc-routing-meta/example/` (repo: `/Users/johnson.chiang/workspace/ab-superpowers`).
- Do NOT modify anything under `src/common/` or `src/plugin/` — kit stays untouched.
- Do NOT annotate `IdChange`/`LotAdd`/`LotChange` with `(routing.pctx)` — that fails codegen (SPEC §9, `bad_multiple_ctx_fields.proto`).
- Contexts carry **LotID only**; the other 6 keys project as `Key=` (doc final decision).
- Overflow policy constants (25 / 512 / 7168) are NOT touched (DoD F).
- Commits: concise messages, NO `Co-Authored-By` trailer, local only — never push.
- Branch: `sys4-update-material-example` (already checked out).
- Build command is `./build.sh` (runs codegen + negative gate + tests at the end).

---

### Task 1: sys4.proto + build integration

**Files:**
- Create: `grpc-routing-meta/example/proto/sys4.proto`
- Modify: `grpc-routing-meta/example/build.sh:39` (`SYSTEMS=(sys1 sys2 sys3)`)
- Modify: `grpc-routing-meta/example/CMakeLists.txt:56` (`foreach(name sys1 sys2 sys3)`)

**Interfaces:**
- Produces: generated `sys4.pb.h` / `sys4.proj.h` with `sys4::v1::UpdateMaterialRequest` (fields: `msg_hdr`, `ctrl_job`, `ope_no`, `eqp_id`, `stage_id`, `route_id`, `tech`, repeated `id_change`/`lot_add`/`lot_change`, repeated `contexts`), `sys4::v1::IdChange{orig_mat_id,new_mat_id,new_mat_lot_id}`, `sys4::v1::LotAdd{mat_id_list,new_mat_lot_id}`, `sys4::v1::LotChange{orig_mat_lot_id,new_mat_lot_id}`, and free function `ProjectMeta(const sys4::v1::UpdateMaterialRequest&, routingmeta::MetadataSink&, bool emit_digest = true)` (same shape as sys1–3). Tasks 2–3 rely on all of these names.

- [ ] **Step 1: Write `proto/sys4.proto`**

```proto
// sys4 system — material/MES domain. 1 transaction, THREE exclusive batch shapes.
//
// One method serves three batch operations (id-change / lot-add / lot-change),
// each carrying lot ids in its own message under different field names. The
// operation messages are NOT annotated: three repeated pctx sources per message
// is rejected at codegen (SPEC §9), and three different keys would defeat the
// shared 7-field schema. Instead the PRODUCER normalizes new_mat_lot_id ->
// contexts (LotID only) before Send; see docs/sys4-update-material.zh.md.
syntax = "proto3";

package sys4.v1;

import "metadata_options.proto";   // (routing.project) for the scalar route keys
import "process_context.proto";

message IdChange {
  string orig_mat_id    = 1;
  string new_mat_id     = 2;
  string new_mat_lot_id = 3;
}

message LotAdd {
  repeated string mat_id_list = 1;
  string new_mat_lot_id       = 2;
}

message LotChange {
  string orig_mat_lot_id = 1;
  string new_mat_lot_id  = 2;
}

message UpdateMaterialRequest {
  string msg_hdr  = 1;   // [app] stand-in for the real system's header block
  string ctrl_job = 2;   // [app]
  string ope_no   = 3 [(routing.project)={key:"x-ope-no"}];   // [+meta] whole-request route key
  string eqp_id   = 4 [(routing.project)={key:"x-eqp-id"}];   // [+meta] whole-request route key
  string stage_id = 5;   // [app] NOT projected (decision: contexts carry LotID only)
  string route_id = 6;   // [app]
  string tech     = 7;   // [app] NOT projected
  repeated IdChange  id_change  = 8;   // [app] exclusive source 1
  repeated LotAdd    lot_add    = 9;   // [app] exclusive source 2
  repeated LotChange lot_change = 10;  // [app] exclusive source 3

  repeated common.v1.ProcessContext contexts = 11;  // [+meta] the ONLY pctx source
}

service Sys4Service {
  // sys4.material.update
  rpc UpdateMaterial(UpdateMaterialRequest) returns (common.v1.Ack);
}
```

- [ ] **Step 2: Add sys4 to both build lists**

In `build.sh`, change:

```bash
SYSTEMS=(sys1 sys2 sys3)
```
to
```bash
SYSTEMS=(sys1 sys2 sys3 sys4)
```

In `CMakeLists.txt`, change:

```cmake
foreach(name sys1 sys2 sys3)
```
to
```cmake
foreach(name sys1 sys2 sys3 sys4)
```

- [ ] **Step 3: Run the build to verify codegen accepts sys4**

Run: `cd grpc-routing-meta/example && ./build.sh`
Expected: green (`OK -> binaries in .../build/`), and the log contains `[gen ] sys4.proto (cpp + meta)`. Verify `build/generated/sys4.proj.h` exists.

- [ ] **Step 4: Commit**

```bash
git add grpc-routing-meta/example/proto/sys4.proto grpc-routing-meta/example/build.sh grpc-routing-meta/example/CMakeLists.txt
git commit -m "feat: sys4 proto — UpdateMaterialRequest with three exclusive batch shapes"
```

---

### Task 2: FillContexts via TDD (all three doc-§6 test cases)

**Files:**
- Create: `grpc-routing-meta/example/sender/sys4_fill_contexts.h`
- Test: `grpc-routing-meta/example/tests/test_projection.cc` (append cases before the final `printf`/`return` in `main`)

**Interfaces:**
- Consumes: `sys4::v1::UpdateMaterialRequest` and `ProjectMeta` from Task 1; existing `routingmeta::VectorSink` (`.Get(key)`, `.Count(key)`), `routingmeta::ProjResult` (`.ok`, `.issues`), `routingmeta::Issue::Overflow`.
- Produces: `bool sys4demo::FillContexts(sys4::v1::UpdateMaterialRequest&)` — `true` = contexts filled (one per item, LotID only), `false` = exclusive-source violation (>1 source non-empty), contexts untouched. Task 3 relies on this exact signature.

- [ ] **Step 1: Write the failing tests**

In `tests/test_projection.cc`, add the includes (after `#include "sys3.proj.h"`):

```cpp
#include "sys4.proj.h"
#include "../sender/sys4_fill_contexts.h"
```

Add these cases at the end of `main` (before the final success `printf`, matching the file's block style):

```cpp
  // --- sys4: single-source expansion — each exclusive source maps new_mat_lot_id
  //     -> LotID; the other 6 keys project as `Key=` (docs/sys4-update-material.zh.md §3) ---
  {
    sys4::v1::UpdateMaterialRequest req;
    req.set_ope_no("OP123");
    req.set_eqp_id("EQP-A");
    req.add_lot_add()->set_new_mat_lot_id("LOT001");
    req.add_lot_add()->set_new_mat_lot_id("LOT002");
    assert(sys4demo::FillContexts(req));
    assert(req.contexts_size() == 2);
    routingmeta::VectorSink sink;
    routingmeta::ProjResult r = ProjectMeta(req, sink);
    assert(r.ok);
    assert(sink.Get("x-ope-no") == "OP123");                        // scalar route keys ride along
    assert(sink.Get("x-eqp-id") == "EQP-A");
    assert(sink.Get("x-process-context-count") == "2");
    // Pin the EXACT canonical line: LotID filled, all other keys present-but-empty.
    assert(sink.Get("x-process-context") ==
           "ChamberId=&LotID=LOT001&OperationNO=&PartID=&RecipeID=&StageID=&Tech=");
  }
  {
    sys4::v1::UpdateMaterialRequest req;                            // source: id_change
    auto* e = req.add_id_change();
    e->set_orig_mat_id("M-OLD"); e->set_new_mat_id("M-NEW"); e->set_new_mat_lot_id("LOT-IC");
    assert(sys4demo::FillContexts(req));
    assert(req.contexts_size() == 1 && req.contexts(0).lot_id() == "LOT-IC");
  }
  {
    sys4::v1::UpdateMaterialRequest req;                            // source: lot_change
    auto* e = req.add_lot_change();
    e->set_orig_mat_lot_id("LOT-OLD"); e->set_new_mat_lot_id("LOT-NEW");
    assert(sys4demo::FillContexts(req));
    assert(req.contexts_size() == 1 && req.contexts(0).lot_id() == "LOT-NEW");  // NEW id is canonical
  }

  // --- sys4: exclusive-source violation -> FillContexts refuses, contexts untouched
  //     (fail loud, never a silent first-pick) ---
  {
    sys4::v1::UpdateMaterialRequest req;
    req.add_lot_add()->set_new_mat_lot_id("LOT001");
    req.add_lot_change()->set_new_mat_lot_id("LOT002");
    assert(!sys4demo::FillContexts(req));
    assert(req.contexts_size() == 0);
  }

  // --- sys4: 26 items -> overflow flag, no context lines, non-blocking (SPEC §5.4,
  //     centralized policy — no sys4 exception) ---
  {
    sys4::v1::UpdateMaterialRequest req;
    for (int i = 0; i < 26; ++i)
      req.add_lot_add()->set_new_mat_lot_id("LOT" + std::to_string(i));
    assert(sys4demo::FillContexts(req));
    routingmeta::VectorSink sink;
    routingmeta::ProjResult r = ProjectMeta(req, sink);
    assert(r.ok);                                                   // overflow is non-blocking
    assert(r.issues.size() == 1 && r.issues[0].kind == routingmeta::Issue::Overflow);
    assert(sink.Get("x-process-context-count") == "26");
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0);
    assert(sink.Get("x-process-context-digest").empty());
  }
```

- [ ] **Step 2: Run build to verify the tests fail**

Run: `cd grpc-routing-meta/example && ./build.sh`
Expected: FAIL at `[test] test_projection` compile step — `'sys4_fill_contexts.h' file not found`.

- [ ] **Step 3: Write minimal implementation `sender/sys4_fill_contexts.h`**

```cpp
#pragma once
// =============================================================================
// sys4 producer-side normalization — NOT part of the kit (ADR 0001: filling the
// body is the producer's job). Three exclusive batch shapes carry lot ids under
// different field names; this maps new_mat_lot_id -> contexts (LotID only).
// Design + wire examples: docs/sys4-update-material.zh.md.
// =============================================================================
#include <string>

#include "sys4.pb.h"

namespace sys4demo {

// false = exclusive-source violation (>1 source non-empty): producer MUST refuse
// to send — never a silent first-pick. contexts is left untouched in that case.
inline bool FillContexts(sys4::v1::UpdateMaterialRequest& req) {
  const int sources = (req.id_change_size() > 0) + (req.lot_add_size() > 0) +
                      (req.lot_change_size() > 0);
  if (sources > 1) return false;
  // Only LotID; the other 6 fields stay empty and project as `Key=` (faithful).
  auto add = [&req](const std::string& lot) { req.add_contexts()->set_lot_id(lot); };
  for (const auto& e : req.id_change())  add(e.new_mat_lot_id());
  for (const auto& e : req.lot_add())    add(e.new_mat_lot_id());
  for (const auto& e : req.lot_change()) add(e.new_mat_lot_id());
  return true;
}

}  // namespace sys4demo
```

- [ ] **Step 4: Run build to verify all tests pass**

Run: `cd grpc-routing-meta/example && ./build.sh`
Expected: green — `[test] run test_projection` passes, `OK -> binaries ...`.

- [ ] **Step 5: Commit**

```bash
git add grpc-routing-meta/example/sender/sys4_fill_contexts.h grpc-routing-meta/example/tests/test_projection.cc
git commit -m "feat: sys4 FillContexts — producer-side LotID normalization, fail-loud exclusivity; 3 doc cases tested"
```

---

### Task 3: unified_sender sys4 demo block

**Files:**
- Modify: `grpc-routing-meta/example/sender/unified_sender.cc` (add include after `#include "sys3.proj.h"`; add demo block at the end of `main`, after the last existing demo block)

**Interfaces:**
- Consumes: `sys4demo::FillContexts` (Task 2), `sys4::v1::UpdateMaterialRequest` (Task 1), existing local `Send<>()`, `Runtime`, `routingmeta::VectorSink`, `dump(...)`.

- [ ] **Step 1: Add include and demo block**

Include:

```cpp
#include "sys4.proj.h"
#include "sys4_fill_contexts.h"
```

Demo block (end of `main`, style-matched to the existing blocks):

```cpp
  // --- sys4  sys4.material.update — ONE method, THREE exclusive batch shapes.
  // The producer normalizes new_mat_lot_id -> contexts (LotID only) BEFORE Send;
  // kit and Send<> are untouched. See docs/sys4-update-material.zh.md.
  {
    sys4::v1::UpdateMaterialRequest req;                 // [app] build the request
    req.set_ope_no("OP123");                             // [app] -> x-ope-no (whole-request route key)
    req.set_eqp_id("EQP-A");                             // [app] -> x-eqp-id
    req.add_lot_add()->set_new_mat_lot_id("LOT001");     // [app] this call's shape: lot_add
    req.add_lot_add()->set_new_mat_lot_id("LOT002");     // [app]
    if (sys4demo::FillContexts(req)) {                   // [+meta] producer-side normalization
      routingmeta::VectorSink sink;                      // [+meta]
      routingmeta::ProjResult r = Send(req, Runtime{"CORR-LOT01-005", "F18", "ETCH01", "REQ-0005", "eap"}, sink);  // [+meta]
      dump("sys4  UpdateMaterial (lot_add x2 -> LotID)", sink, r);  // [demo]
    }
  }

  // --- sys4 exclusive-source violation: FillContexts refuses -> producer does NOT send ---
  {
    sys4::v1::UpdateMaterialRequest bad;                          // [demo]
    bad.add_lot_add()->set_new_mat_lot_id("LOT001");              // [demo] two sources at once
    bad.add_lot_change()->set_new_mat_lot_id("LOT002");           // [demo]
    if (!sys4demo::FillContexts(bad))                             // [+meta] fail loud, no send
      std::printf("=== sys4  UpdateMaterial REFUSED (exclusive-source violation: lot_add + lot_change) ===\n\n");
  }
```

- [ ] **Step 2: Build and eyeball the demo output**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/unified_sender`
Expected: green build; output contains the sys4 block with `x-ope-no: OP123`, `x-eqp-id: EQP-A`, `x-process-context-count: 2`, two `x-process-context` lines whose only filled key is `LotID=`, followed by the `REFUSED (exclusive-source violation...)` line.

- [ ] **Step 3: Commit**

```bash
git add grpc-routing-meta/example/sender/unified_sender.cc
git commit -m "feat: sys4 demo in unified_sender — normalize + send, and fail-loud refusal"
```

---

### Task 4: docs sync

**Files:**
- Modify: `grpc-routing-meta/CONTEXT.md:30-38` (systems table)
- Modify: `grpc-routing-meta/docs/sys4-update-material.zh.md` (§6 checklist → point at real files)

- [ ] **Step 1: Update CONTEXT.md systems table**

Change the heading `## Systems (3) and methods (16)` to `## Systems (4) and methods (17)`, add this row after the sys3 row:

```markdown
| **sys4** | `sys4.proto` / `sys4.v1` | 1 (`UpdateMaterial`) | process-context **LotID-only**, producer-normalized from 3 exclusive batch shapes (`sender/sys4_fill_contexts.h`) |
```

and change the sentence `All three import the shared` to `All four import the shared`.

- [ ] **Step 2: Point the doc's checklist at the real files**

In `docs/sys4-update-material.zh.md`, replace §6 entirely with:

```markdown
## 6. 對照實作（example/ 內可跑）

| 本文段落 | 實作 |
|---|---|
| §1 proto | `example/proto/sys4.proto` |
| §3 `FillContexts` | `example/sender/sys4_fill_contexts.h`（producer 端，非 kit） |
| §4 wire demo | `example/sender/unified_sender.cc` 的 sys4 區塊（`./build/unified_sender`） |
| §5 三個驗證 case | `example/tests/test_projection.cc` 的 sys4 區塊：單來源展開（含 exact 行 pin）、互斥破裂拒送、26 筆 overflow |

`cd example && ./build.sh` 一次跑完 codegen + 全部 assert。
```

- [ ] **Step 3: Commit**

```bash
git add grpc-routing-meta/CONTEXT.md grpc-routing-meta/docs/sys4-update-material.zh.md
git commit -m "docs: register sys4 in CONTEXT.md systems table; doc checklist points at real files"
```
