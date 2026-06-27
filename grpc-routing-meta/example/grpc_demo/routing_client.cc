// =============================================================================
// routing_client — the Sender, over a REAL gRPC channel, for all three systems.
//
// Each call projects the body into the grpc::ClientContext via GrpcSink (the
// production path) and makes the RPC. Demonstrates the 3 Layer-3 shapes + the error
// path + wire-tamper detection, all uniform — no per-system branch on the send side.
//   sys1.Calculate  : 2 contexts (batch)        -> server verifies digest
//   sys2.Verify     : 1 sparse context          -> server verifies digest
//   sys2.List       : 0 contexts (count=0)       -> nothing to verify
//   sys3.Submit05   : nested mask id (scalar)    -> x-mask-id on the wire
//   sys3.Submit05   : EMPTY mask (missing-req)   -> x-routing-error -> server REJECTs
//   sys1.Calculate  : tampered in transit        -> server REJECTs (digest drift)
// =============================================================================
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <string>

#include "common/common_headers.h"
#include "common/metadata_sink.h"   // GrpcSink, under -DROUTINGMETA_WITH_GRPC
#include "sys1.grpc.pb.h"
#include "sys2.grpc.pb.h"
#include "sys3.grpc.pb.h"
#include "sys1.proj.h"
#include "sys2.proj.h"
#include "sys3.proj.h"

static common::v1::ProcessContext* fill(common::v1::ProcessContext* c, const char* lot, const char* ch) {
  c->set_lot_id(lot); c->set_chamber_id(ch); c->set_recipe_id("RCP_ETCH_V3");
  c->set_tech("N5"); c->set_part_id("PART-A"); c->set_stage_id("ETCH"); c->set_operation_no("OP100");
  return c;
}

// Project `req` via GrpcSink into a fresh context, run `rpc`, print the round-trip.
template <class Req, class Rpc>
static void call(const char* tag, const Req& req, Rpc rpc) {
  grpc::ClientContext ctx;
  ctx.set_wait_for_ready(true);
  routingmeta::GrpcSink sink(&ctx);
  FillCommon(Runtime{std::string("CORR-") + tag, "F18", "ETCH01"}, sink);
  routingmeta::ProjResult r = ProjectMeta(req, sink);   // ADL-resolved per request type
  common::v1::Ack ack;
  grpc::Status s = rpc(&ctx, &ack);
  std::printf("[client] %-14s sent ok=%d -> %s %s\n", tag, r.ok,
              s.ok() ? "OK" : "ERR", s.error_message().c_str());
}

int main(int argc, char** argv) {
  const std::string addr = argc > 1 ? argv[1] : "127.0.0.1:50251";
  auto chan = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  auto s1 = sys1::v1::Sys1Service::NewStub(chan);
  auto s2 = sys2::v1::Sys2Service::NewStub(chan);
  auto s3 = sys3::v1::Sys3Service::NewStub(chan);

  sys1::v1::CalculateRequest c1;                    // sys1: batch of 2
  c1.set_tool_id("ETCH01"); fill(c1.add_contexts(), "LOT01", "CH-A"); fill(c1.add_contexts(), "LOT02", "CH-B");
  call("sys1.Calculate", c1, [&](grpc::ClientContext* c, common::v1::Ack* a) { return s1->Calculate(c, c1, a); });

  sys2::v1::VerifyRequest v2;                        // sys2: one sparse context (RecipeID only)
  v2.add_contexts()->set_recipe_id("RCP_ETCH_V3");
  call("sys2.Verify", v2, [&](grpc::ClientContext* c, common::v1::Ack* a) { return s2->Verify(c, v2, a); });

  sys2::v1::ListRequest l2;                          // sys2: count=0
  call("sys2.List", l2, [&](grpc::ClientContext* c, common::v1::Ack* a) { return s2->List(c, l2, a); });

  sys3::v1::Submit05Request m3;                      // sys3: nested mask id (scalar)
  m3.mutable_job()->mutable_mask()->set_mask_id("RET-9981");
  call("sys3.Submit05", m3, [&](grpc::ClientContext* c, common::v1::Ack* a) { return s3->Submit05(c, m3, a); });

  sys3::v1::Submit05Request e3;                      // sys3: EMPTY mask -> missing-required
  call("sys3.empty", e3, [&](grpc::ClientContext* c, common::v1::Ack* a) { return s3->Submit05(c, e3, a); });

  // bonus: mangle one context header in transit -> server's digest recompute rejects it
  {
    routingmeta::VectorSink vs; ProjectMeta(c1, vs);
    grpc::ClientContext ctx; ctx.set_wait_for_ready(true);
    bool t = false;
    for (const auto& kv : vs.items) {
      std::string v = kv.second;
      if (!t && kv.first == "x-process-context") { v += "&X=1"; t = true; }
      ctx.AddMetadata(kv.first, v);
    }
    common::v1::Ack ack;
    grpc::Status s = s1->Calculate(&ctx, c1, &ack);
    std::printf("[client] %-14s sent (tampered) -> %s %s\n", "sys1.tampered",
                s.ok() ? "OK" : "ERR", s.error_message().c_str());
  }
  return 0;
}
