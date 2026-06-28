// =============================================================================
// unified_sender — ONE sender for every system.
//
// Don't be scared off: adopting this kit barely touches your client. In a real
// gRPC call you ALREADY write:
//
//     sys1::v1::CalculateRequest req;            // [app] build the request
//     ... fill req from your business data ...  // [app]
//     grpc::ClientContext ctx;                  // [app]
//     stub->Calculate(&ctx, req, &resp);        // [app] make the call
//
// Adopting the kit adds exactly TWO lines before the call — nothing else changes,
// no per-system code, no hand-written header extraction:
//
//     routingmeta::GrpcSink sink(&ctx);         // [+meta] wrap the context once
//     Send(req, rt, sink);                      // [+meta] = FillCommon + ProjectMeta
//
// Below, every line is tagged:  [app] = you'd write it anyway · [+meta] = added for
// routing metadata · [demo] = only here to print the result. Notice the ratio.
// =============================================================================
#include <cstdio>
#include <string>

#include "common/metadata_sink.h"
#include "common/common_headers.h"   // [+meta] Runtime + FillCommon (the 6 common headers)
#include "sys1.proj.h"
#include "sys2.proj.h"
#include "sys3.proj.h"

// [+meta] The one call your app makes. Works for any request type; ProjectMeta
// resolves by Req. No system/method branching anywhere.
template <class Req>
void Send(const Req& req, const routingmeta::Runtime& rt, routingmeta::MetadataSink& sink) {
  FillCommon(rt, sink);
  ProjectMeta(req, sink);
}

// [demo] Only prints what got attached; not part of adoption.
static void dump(const char* title, const routingmeta::VectorSink& s) {
  std::printf("=== %s   (%zu bytes metadata) ===\n", title, s.bytes());
  for (const auto& kv : s.items)
    std::printf("  %-26s %s\n", (kv.first + ":").c_str(), kv.second.c_str());
  std::printf("\n");
}

// [app] Fills business data into a context. This is YOUR request payload — the
// lot/recipe/chamber you were going to send regardless of any routing kit.
static void fillCtx(common::v1::ProcessContext* c, const char* lot, const char* chamber) {
  c->set_lot_id(lot);
  c->set_chamber_id(chamber);
  c->set_recipe_id("RCP_ETCH_V3");
  c->set_tech("N5");
  c->set_part_id("PART-A");
  c->set_stage_id("ETCH");
  c->set_operation_no("OP100");
}

int main() {
  const routingmeta::Runtime rt{"CORR-LOT01-001", "F18", "ETCH01"};   // [+meta] values you already have

  // --- sys1  sys1.control.calculate — batch of 2, full context ---
  {
    sys1::v1::CalculateRequest req;                 // [app] build the request
    req.set_tool_id("ETCH01");                     // [app] business data
    fillCtx(req.add_contexts(), "LOT01", "CH-A");  // [app] business data (the lots in this batch)
    fillCtx(req.add_contexts(), "LOT02", "CH-B");  // [app]
    routingmeta::VectorSink sink;                  // [+meta] in prod: routingmeta::GrpcSink sink(&ctx);
    Send(req, rt, sink);                           // [+meta] the only added call
    dump("sys1  Calculate (2 contexts)", sink);     // [demo]
  }

  // --- sys2  sys2.recipe.verify — 1 sparse context (only RecipeID; rest empty) ---
  {
    sys2::v1::VerifyRequest req;                          // [app]
    req.add_contexts()->set_recipe_id("RCP_ETCH_V3");    // [app] business data
    routingmeta::VectorSink sink;                        // [+meta]
    Send(req, routingmeta::Runtime{"CORR-LOT01-002", "F18", "ETCH01"}, sink);  // [+meta]
    dump("sys2  Verify (1 sparse context)", sink);        // [demo]
  }

  // --- sys2  sys2.recipe.list — zero contexts (count=0) ---
  {
    sys2::v1::ListRequest req;                            // [app] (no contexts to send)
    routingmeta::VectorSink sink;                        // [+meta]
    Send(req, routingmeta::Runtime{"CORR-LIST-003", "F18", "ETCH01"}, sink);   // [+meta]
    dump("sys2  List (count=0)", sink);                   // [demo]
  }

  // --- sys3  sys3.layout.submit — nested mask id (Submit05), no context ---
  {
    sys3::v1::Submit05Request req;                          // [app]
    req.mutable_job()->mutable_mask()->set_mask_id("RET-9981");  // [app] business data (your mask id);
                                                            //       the x-mask-id HEADER is auto-projected by Send
    routingmeta::VectorSink sink;                          // [+meta]
    Send(req, routingmeta::Runtime{"CORR-sys3-004", "F18", "LITHO01"}, sink);  // [+meta]
    dump("sys3 Submit05 (nested mask id, no context)", sink);     // [demo]
  }

  // --- sys1 overflow — 60 contexts trip the 7 KB total guard ---
  {
    sys1::v1::CalculateRequest req;                 // [app]
    req.set_tool_id("ETCH01");                     // [app]
    for (int i = 0; i < 60; ++i)                   // [app] business data (a big batch)
      fillCtx(req.add_contexts(), ("LOT" + std::to_string(i)).c_str(), "CH-A");
    routingmeta::VectorSink sink;                  // [+meta]
    Send(req, rt, sink);                           // [+meta]
    dump("sys1  Calculate (60 contexts -> overflow)", sink);       // [demo]
  }

  std::printf("All 16 transaction types (sys1 x1, sys2 x5, sys3 x10) route through the same Send<>().\n");
  return 0;
}
