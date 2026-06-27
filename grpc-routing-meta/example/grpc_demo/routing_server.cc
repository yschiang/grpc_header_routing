// =============================================================================
// routing_server — ONE real gRPC backend for all three systems.
//
// Every RPC (sys1 / sys2 / sys3) verifies the same way: read the projected metadata
// off the wire (ServerContext::client_metadata) and recompute the sha256 digest over
// the x-process-context headers — the receiver_verify check, on bytes that actually
// crossed HTTP/2. x-routing-error (missing-required) is rejected; digest drift is
// rejected; count=0 / scalar-only carry no digest and just pass. No per-system branch.
// =============================================================================
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/process_context_parser.h"
#include "sys1.grpc.pb.h"
#include "sys2.grpc.pb.h"
#include "sys3.grpc.pb.h"

static std::string str(const grpc::string_ref& r) { return {r.data(), r.size()}; }

// The whole backend contract, uniform across all 16 transactions.
static grpc::Status VerifyWire(grpc::ServerContext* ctx, const char* rpc) {
  std::vector<std::string> contexts;
  std::string digest, route_err, mask, count = "0";
  for (const auto& kv : ctx->client_metadata()) {
    const std::string k = str(kv.first);
    if (k == "x-process-context") contexts.push_back(str(kv.second));
    else if (k == "x-process-context-digest") digest = str(kv.second);
    else if (k == "x-process-context-count") count = str(kv.second);
    else if (k == "x-routing-error") route_err = str(kv.second);
    else if (k == "x-mask-id") mask = str(kv.second);
  }
  std::printf("\n[server] %-16s count=%s%s%s\n", rpc, count.c_str(),
              mask.empty() ? "" : "  x-mask-id=", mask.c_str());

  if (!route_err.empty()) {
    std::printf("[server]   REJECT x-routing-error=%s\n", route_err.c_str());
    return {grpc::StatusCode::INVALID_ARGUMENT, "routing-error: " + route_err};
  }
  if (contexts.empty()) {                       // count=0 / scalar-only: nothing to digest
    std::printf("[server]   OK (no process-context to verify)\n");
    return grpc::Status::OK;
  }
  auto vr = routingmeta::VerifyDigest(contexts, digest);
  std::printf("[server]   digest %s\n", vr.ok ? "OK (no drift in transit)" : "MISMATCH (drift!)");
  if (!vr.ok) return {grpc::StatusCode::FAILED_PRECONDITION, "digest drift: " + vr.error};
  return grpc::Status::OK;
}

struct Sys1 final : sys1::v1::Sys1Service::Service {
  grpc::Status Calculate(grpc::ServerContext* c, const sys1::v1::CalculateRequest*,
                         common::v1::Ack*) override { return VerifyWire(c, "sys1.Calculate"); }
};
struct Sys2 final : sys2::v1::Sys2Service::Service {
  grpc::Status Verify(grpc::ServerContext* c, const sys2::v1::VerifyRequest*,
                      common::v1::Ack*) override { return VerifyWire(c, "sys2.Verify"); }
  grpc::Status List(grpc::ServerContext* c, const sys2::v1::ListRequest*,
                    common::v1::Ack*) override { return VerifyWire(c, "sys2.List"); }
};
struct Sys3 final : sys3::v1::Sys3Service::Service {
  grpc::Status Submit05(grpc::ServerContext* c, const sys3::v1::Submit05Request*,
                        common::v1::Ack*) override { return VerifyWire(c, "sys3.Submit05"); }
};

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffer so server logs survive piping/kill
  const std::string addr = argc > 1 ? argv[1] : "127.0.0.1:50251";
  Sys1 s1; Sys2 s2; Sys3 s3;
  grpc::ServerBuilder b;
  b.AddListeningPort(addr, grpc::InsecureServerCredentials());
  b.RegisterService(&s1); b.RegisterService(&s2); b.RegisterService(&s3);
  std::printf("[server] Sys1+Sys2+Sys3 listening on %s\n", addr.c_str());
  b.BuildAndStart()->Wait();
  return 0;
}
