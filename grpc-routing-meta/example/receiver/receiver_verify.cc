// =============================================================================
// receiver_verify — receiver / backend side (round-trip consistency).
//
// One uniform parser handles every system: read the common headers, then if
// x-process-context is present, parse it and verify the sha256 digest. A mismatch
// means the header projection drifted from the body. sys3 (no process-context)
// would simply yield zero contexts — no per-system branch needed.
//
// Here we project an sys1 request (stands in for what the gateway received) and feed
// the resulting headers back through the parser.
// =============================================================================
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "common/metadata_sink.h"
#include "common/process_context_parser.h"
#include "sys1.proj.h"

int main() {
  sys1::v1::CalculateRequest req;
  req.set_tool_id("ETCH01");
  auto fill = [](common::v1::ProcessContext* c, const char* lot, const char* ch) {
    c->set_lot_id(lot);
    c->set_chamber_id(ch);
    c->set_recipe_id("RCP_ETCH_V3");
    c->set_tech("N5");
    c->set_part_id("PART-A");
    c->set_stage_id("ETCH");
    c->set_operation_no("OP100");
  };
  fill(req.add_contexts(), "LOT01", "CH-A");
  fill(req.add_contexts(), "LOT02", "CH-B");

  routingmeta::VectorSink sink;
  ProjectMeta(req, sink);

  std::vector<std::string> contexts;
  std::string digest;
  for (const auto& kv : sink.items) {
    if (kv.first == "x-process-context")
      contexts.push_back(kv.second);
    else if (kv.first == "x-process-context-digest")
      digest = kv.second;
  }

  std::printf("=== received %zu process-context header(s) ===\n", contexts.size());
  for (const auto& c : contexts) {
    auto kv = routingmeta::ParseContext(c);
    std::printf("  LotID=%s ChamberId=%s RecipeID=%s\n", kv["LotID"].c_str(),
                kv["ChamberId"].c_str(), kv["RecipeID"].c_str());
  }

  auto vr = routingmeta::VerifyDigest(contexts, digest);
  std::printf("\ndigest check: %s\n  expected: %s\n  actual:   %s\n",
              vr.ok ? "OK (header matches body)" : "FAILED", vr.expected_digest.c_str(),
              vr.actual_digest.c_str());
  if (!vr.ok) std::printf("  error: %s\n", vr.error.c_str());

  // Reject-path regression guard (AD-9): a tampered body MUST fail the digest.
  if (!contexts.empty()) {
    auto tampered = contexts;
    tampered[0] += "&INJECTED=1";  // header<->body drift
    auto bad = routingmeta::VerifyDigest(tampered, digest);
    std::printf("tamper check: %s (expected reject)%s%s\n", bad.ok ? "ACCEPTED (BUG!)" : "rejected",
                bad.error.empty() ? "" : " — ", bad.error.c_str());
    assert(!bad.ok);  // aborts (non-zero) only if the gate regresses
  }

  return vr.ok ? 0 : 1;
}
