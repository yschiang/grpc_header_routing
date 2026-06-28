// =============================================================================
// demo/grpc_server.cc — LIVE receiver for the real-wire demo (Story 2.1).
//
// Reads the projected routing-meta off real gRPC client metadata and re-runs the
// kit's digest gate (process_context_parser.h::VerifyDigest) — the same check
// receiver_verify.cc does in-process, now over an actual wire hop. It implements
// only the two RPCs the demo client calls; the rest stay default-UNIMPLEMENTED.
//
// Policy shown: the digest gate REJECTS only when the projected context headers no
// longer match their digest header (integrity drift, DATA_LOSS) — it is a header-vs-
// digest integrity check, not security and not a body re-derivation (see story 1.15).
// Missing-required and overflow are surfaced (logged) but non-blocking (ACCEPT) —
// the kit reports, the receiver decides (SPEC §7 / NFR7).
// =============================================================================
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/process_context_parser.h"  // routingmeta::VerifyDigest / ParseContext
#include "sys1.grpc.pb.h"
#include "sys3.grpc.pb.h"

namespace {

// data() may be null for a zero-length string_ref — std::string(nullptr, 0) is UB by
// precondition, so guard the empty case.
std::string Ref(const grpc::string_ref& r) {
  return r.size() ? std::string(r.data(), r.size()) : std::string();
}

// Pull the projected headers off the wire and re-run the digest gate. OK on a
// clean or absent-digest path; DATA_LOSS only on projected-header vs digest drift.
grpc::Status VerifyFromMetadata(grpc::ServerContext* ctx, const char* rpc) {
  std::vector<std::string> contexts;  // repeated x-process-context, in arrival order
  std::string digest, corr, count, overflow, routing_error;
  for (const auto& kv : ctx->client_metadata()) {
    const std::string k = Ref(kv.first);  // gRPC lowercases metadata keys
    if (k == "x-process-context")
      contexts.push_back(Ref(kv.second));
    else if (k == "x-process-context-digest")
      digest = Ref(kv.second);
    else if (k == "x-correlation-id")
      corr = Ref(kv.second);
    else if (k == "x-process-context-count")
      count = Ref(kv.second);
    else if (k == "x-process-context-overflow")
      overflow = Ref(kv.second);
    else if (k == "x-routing-error")
      routing_error = Ref(kv.second);
  }

  std::printf("[server] %-14s corr=%s count=%s contexts=%zu\n", rpc, corr.c_str(), count.c_str(),
              contexts.size());
  for (const auto& c : contexts) {
    auto kv = routingmeta::ParseContext(c);  // human-readable, like receiver_verify
    std::printf("           LotID=%s ChamberId=%s RecipeID=%s\n", kv["LotID"].c_str(),
                kv["ChamberId"].c_str(), kv["RecipeID"].c_str());
  }
  if (!routing_error.empty())
    std::printf("[server] %-14s x-routing-error: %s -> ACCEPT (surfaced, non-blocking)\n", rpc,
                routing_error.c_str());

  grpc::Status out = grpc::Status::OK;
  if (overflow == "true") {
    std::printf(
        "[server] %-14s x-process-context-overflow: true; no digest -> ACCEPT (non-blocking)\n",
        rpc);
  } else if (digest.empty()) {
    std::printf("[server] %-14s no digest provided -> ACCEPT (count=0)\n", rpc);
  } else {
    auto vr = routingmeta::VerifyDigest(contexts, digest);
    if (vr.ok) {
      std::printf("[server] %-14s digest check: OK (%s) -> ACCEPT\n", rpc,
                  vr.actual_digest.c_str());
    } else {
      std::printf("[server] %-14s digest MISMATCH expected=%s actual=%s -> REJECT\n", rpc,
                  vr.expected_digest.c_str(), vr.actual_digest.c_str());
      out = grpc::Status(grpc::StatusCode::DATA_LOSS, vr.error);
    }
  }
  std::fflush(stdout);
  return out;
}

class Sys1Impl final : public sys1::v1::Sys1Service::Service {
  grpc::Status Calculate(grpc::ServerContext* ctx, const sys1::v1::CalculateRequest*,
                         common::v1::Ack*) override {
    return VerifyFromMetadata(ctx, "sys1.Calculate");
  }
};

class Sys3Impl final : public sys3::v1::Sys3Service::Service {
  grpc::Status Submit05(grpc::ServerContext* ctx, const sys3::v1::Submit05Request*,
                        common::v1::Ack*) override {
    return VerifyFromMetadata(ctx, "sys3.Submit05");
  }
};

}  // namespace

int main(int argc, char** argv) {
  const std::string addr = (argc > 1) ? argv[1] : "127.0.0.1:50551";
  Sys1Impl sys1;
  Sys3Impl sys3;
  grpc::ServerBuilder b;
  b.AddListeningPort(addr, grpc::InsecureServerCredentials());
  b.RegisterService(&sys1);
  b.RegisterService(&sys3);
  std::unique_ptr<grpc::Server> server = b.BuildAndStart();
  if (!server) {
    std::fprintf(stderr, "[server] failed to bind %s\n", addr.c_str());
    return 1;
  }
  std::printf("[server] LISTENING on %s\n", addr.c_str());
  std::fflush(stdout);
  server->Wait();
  return 0;
}
