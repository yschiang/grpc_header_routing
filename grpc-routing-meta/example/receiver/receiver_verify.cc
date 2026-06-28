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
    c->set_lot_id(lot); c->set_chamber_id(ch); c->set_recipe_id("RCP_ETCH_V3");
    c->set_tech("N5"); c->set_part_id("PART-A"); c->set_stage_id("ETCH"); c->set_operation_no("OP100");
  };
  fill(req.add_contexts(), "LOT01", "CH-A");
  fill(req.add_contexts(), "LOT02", "CH-B");

  routingmeta::VectorSink sink;
  ProjectMeta(req, sink);

  std::vector<std::string> contexts;
  std::string digest;
  for (const auto& kv : sink.items) {
    if (kv.first == "x-process-context") contexts.push_back(kv.second);
    else if (kv.first == "x-process-context-digest") digest = kv.second;
  }

  std::printf("=== received %zu process-context header(s) ===\n", contexts.size());
  for (const auto& c : contexts) {
    auto kv = routingmeta::ParseContext(c);
    std::printf("  LotID=%s ChamberId=%s RecipeID=%s\n",
                kv["LotID"].c_str(), kv["ChamberId"].c_str(), kv["RecipeID"].c_str());
  }

  // (1) ACCEPT: untampered headers are the faithful projection of the body.
  auto accept = routingmeta::VerifyDigest(contexts, digest);
  std::printf("\n[accept] digest check: %s\n  expected: %s\n  actual:   %s\n",
              accept.ok ? "OK (header matches body)" : "FAILED",
              accept.expected_digest.c_str(), accept.actual_digest.c_str());
  if (!accept.ok) std::printf("  error: %s\n", accept.error.c_str());

  // (2) REJECT: simulate header/body drift — something mangles one context in transit
  // (here ChamberId CH-A -> CH-X) while the digest header rides through unchanged. The
  // receiver MUST catch the mismatch. This is the whole point of the digest, so run it
  // — don't just describe the accept case.
  auto tampered = contexts;
  if (!tampered.empty()) {
    auto pos = tampered[0].find("CH-A");
    if (pos != std::string::npos) tampered[0].replace(pos, 4, "CH-X");
  }
  auto reject = routingmeta::VerifyDigest(tampered, digest);
  std::printf("\n[reject] tampered body (CH-A->CH-X): %s\n  expected: %s\n  actual:   %s\n",
              reject.ok ? "ACCEPTED (BUG!)" : "rejected (mismatch caught)",
              reject.expected_digest.c_str(), reject.actual_digest.c_str());
  if (!reject.ok) std::printf("  error: %s\n", reject.error.c_str());

  // Self-check: the round-trip is correct iff clean headers verify AND tampered ones
  // are caught. Non-zero exit if either half fails.
  const bool ok = accept.ok && !reject.ok;
  std::printf("\nresult: %s\n",
              ok ? "PASS (clean accepted, tampered rejected)" : "FAIL");
  return ok ? 0 : 1;
}
