// =============================================================================
// demo/grpc_client.cc — LIVE sender for the real-wire demo (Story 2.1).
//
// The real adoption path: build the request you'd send anyway, then the only two
// added lines — GrpcSink sink(&ctx); Send(req, rt, sink); — attach the projected
// routing-meta to the actual gRPC ClientContext. Calls real RPCs over a localhost
// channel and prints the ProjResult + the server's verdict.
//
// Case 4 is a DELIBERATE tamper: it projects into a VectorSink, then writes the
// headers to the wire but corrupts one context value while leaving the digest
// header intact — proving the server's digest gate rejects header<->body drift.
// =============================================================================
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <string>

#include "common/common_headers.h"  // routingmeta::Runtime / FillCommon
#include "common/metadata_sink.h"   // routingmeta::GrpcSink (ROUTINGMETA_WITH_GRPC) + VectorSink
#include "common/send.h"            // routingmeta::Send
#include "sys1.grpc.pb.h"
#include "sys1.proj.h"  // ProjectMeta(sys1...) — ADL target for Send / tamper case
#include "sys3.grpc.pb.h"
#include "sys3.proj.h"  // ProjectMeta(sys3...)

namespace {

void fillCtx(common::v1::ProcessContext* c, const char* lot, const char* chamber) {
  c->set_lot_id(lot);
  c->set_chamber_id(chamber);
  c->set_recipe_id("RCP_ETCH_V3");
  c->set_tech("N5");
  c->set_part_id("PART-A");
  c->set_stage_id("ETCH");
  c->set_operation_no("OP100");
}

void report(const char* label, const routingmeta::ProjResult& r, const grpc::Status& st) {
  const std::string rpc = st.ok() ? "OK" : ("REJECTED: " + st.error_message());
  std::printf("[client] %-30s ok=%-5s issues=%zu duration=%lldns  rpc=%s\n", label,
              r.ok ? "true" : "false", r.issues.size(), (long long)r.duration.count(), rpc.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string target = (argc > 1) ? argv[1] : "127.0.0.1:50551";
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  auto sys1 = sys1::v1::Sys1Service::NewStub(channel);
  auto sys3 = sys3::v1::Sys3Service::NewStub(channel);
  const routingmeta::Runtime rt{"CORR-LOT01-001", "F18", "ETCH01"};

  // 1) GOOD — the real adoption path: GrpcSink + Send attach the headers; server OK.
  {
    sys1::v1::CalculateRequest req;
    req.set_tool_id("ETCH01");
    fillCtx(req.add_contexts(), "LOT01", "CH-A");
    fillCtx(req.add_contexts(), "LOT02", "CH-B");
    grpc::ClientContext ctx;
    routingmeta::GrpcSink sink(&ctx);
    auto r = routingmeta::Send(req, rt, sink);
    common::v1::Ack ack;
    auto st = sys1->Calculate(&ctx, req, &ack);
    report("sys1 good (2 ctx)", r, st);
  }

  // 2) OVERFLOW — 60 contexts trip the 7 KB guard: x-process-context-overflow, no digest.
  {
    sys1::v1::CalculateRequest req;
    req.set_tool_id("ETCH01");
    for (int i = 0; i < 60; ++i)
      fillCtx(req.add_contexts(), ("LOT" + std::to_string(i)).c_str(), "CH-A");
    grpc::ClientContext ctx;
    routingmeta::GrpcSink sink(&ctx);
    auto r = routingmeta::Send(req, rt, sink);
    common::v1::Ack ack;
    auto st = sys1->Calculate(&ctx, req, &ack);
    report("sys1 overflow (60 ctx)", r, st);
  }

  // 3) MISSING REQUIRED — empty mask: x-routing-error, ok=false, still delivered.
  {
    sys3::v1::Submit05Request req;  // mask_id left empty (the bug)
    grpc::ClientContext ctx;
    routingmeta::GrpcSink sink(&ctx);
    auto r = routingmeta::Send(req, routingmeta::Runtime{"CORR-sys3-005", "F18", "LITHO01"}, sink);
    common::v1::Ack ack;
    auto st = sys3->Submit05(&ctx, req, &ack);
    report("sys3 empty-mask (missing req)", r, st);
  }

  // 4) TAMPER — project into a VectorSink, copy headers to the wire but corrupt ONE
  //    context value; the digest header is unchanged -> server recompute mismatches.
  {
    sys1::v1::CalculateRequest req;
    req.set_tool_id("ETCH01");
    fillCtx(req.add_contexts(), "LOT01", "CH-A");
    fillCtx(req.add_contexts(), "LOT02", "CH-B");
    routingmeta::VectorSink probe;
    routingmeta::FillCommon(rt, probe);
    auto r = routingmeta::ProjectMeta(req, probe);  // same projection, captured for tampering
    grpc::ClientContext ctx;
    bool tampered = false;
    for (const auto& kv : probe.items) {
      if (kv.first == "x-process-context" && !tampered) {
        ctx.AddMetadata(kv.first, kv.second + "&INJECTED=1");  // drift body, keep digest
        tampered = true;
      } else {
        ctx.AddMetadata(kv.first, kv.second);
      }
    }
    common::v1::Ack ack;
    auto st = sys1->Calculate(&ctx, req, &ack);
    report("sys1 TAMPERED (expect reject)", r, st);
  }
  return 0;
}
