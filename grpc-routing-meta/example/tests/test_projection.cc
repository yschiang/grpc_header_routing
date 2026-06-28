// =============================================================================
// tests — projection, round-trip, digest, overflow (count AND bytes), scalar.
// Plain asserts, zero test deps. Covers the two code paths: process-context (sys1)
// and scalar projection (sys3). sys2 is an sys1 subset, exercised by the sender.
// =============================================================================
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "common/metadata_sink.h"
#include "common/common_headers.h"
#include "common/process_context_parser.h"
#include "common/process_context_emit.h"
#include "common/sha256.h"
#include "common/url_encode.h"
#include "sys1.proj.h"
#include "sys3.proj.h"

// n contexts; recipe lets us test encoding ('/'), pad inflates each context to test
// the byte-size overflow path independently of the count cap.
static sys1::v1::CalculateRequest sys1Req(int n, const char* recipe = "R/A", const char* pad = "") {
  sys1::v1::CalculateRequest req;
  req.set_tool_id("ETCH01");
  for (int i = 0; i < n; ++i) {
    auto* c = req.add_contexts();
    c->set_lot_id("LOT" + std::to_string(i));
    c->set_chamber_id("CH-A");
    c->set_recipe_id(recipe);
    c->set_tech("N5");
    c->set_part_id(std::string("PART-A") + pad);
    c->set_stage_id("ETCH");
    c->set_operation_no("OP100");
  }
  return req;
}

int main() {
  // --- url + sha256 ---
  assert(routingmeta::UrlEncode("R/A") == "R%2FA");
  assert(routingmeta::UrlEncode("a/b c?") == "a%2Fb%20c%3F");        // reserved set -> %XX, space -> %20
  assert(routingmeta::UrlEncode("\xC3\xA9") == "%C3%A9");            // non-ASCII high bytes -> UPPER hex
  assert(routingmeta::UrlDecode(routingmeta::UrlEncode("a b&c=d")) == "a b&c=d");
  assert(routingmeta::Sha256Hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // --- sys1 projection: key-sort, encode, digest, round-trip, tamper ---
  {
    auto req = sys1Req(2);
    routingmeta::VectorSink sink;
    auto r = ProjectMeta(req, sink);
    assert(r.ok);                                                   // happy path: ok, no issues (AC3)
    assert(r.issues.empty());
    assert(sink.Count("x-process-context") == 2);
    assert(sink.Get("x-process-context-count") == "2");
    assert(sink.Get("x-process-context-format") == "urlencoded-query-string-v1");  // wire constant
    assert(!sink.Get("x-process-context-digest").empty());
    assert(sink.Get("x-process-context-digest").rfind("sha256:", 0) == 0);          // digest prefix pinned
    const std::string c0 = sink.Get("x-process-context");
    assert(c0.find("ChamberId=CH-A") != std::string::npos);
    assert(c0.find("RecipeID=R%2FA") != std::string::npos);          // '/' encoded
    assert(c0.find("ChamberId") < c0.find("LotID"));                 // key-sorted

    std::vector<std::string> cs; std::string dg;
    for (auto& kv : sink.items) {
      if (kv.first == "x-process-context") cs.push_back(kv.second);
      else if (kv.first == "x-process-context-digest") dg = kv.second;
    }
    assert(routingmeta::VerifyDigest(cs, dg).ok);                    // sender -> receiver consistent
    assert(routingmeta::ParseContext(cs[0])["RecipeID"] == "R/A");   // decoded back
    cs[0] += "&X=1";
    assert(!routingmeta::VerifyDigest(cs, dg).ok);                   // tamper detected
  }

  // --- count=0: structure present, no digest / no lines ---
  {
    auto req = sys1Req(0);
    routingmeta::VectorSink sink;
    ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "0");
    assert(sink.Count("x-process-context") == 0);
    assert(sink.Get("x-process-context-digest").empty());
  }

  // --- overflow by COUNT (>25) ---
  {
    auto req = sys1Req(30);
    routingmeta::VectorSink sink;
    ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "30");
    assert(sink.Get("x-process-context-format") == "urlencoded-query-string-v1");  // survives overflow (inv. 8)
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0);
    assert(sink.Get("x-process-context-digest").empty());
  }

  // --- overflow by BYTES (<=25 contexts but total > 7KB via padded field) ---
  {
    const std::string pad(300, 'x');                                // each context ~390 bytes
    auto req = sys1Req(20, "RCP", pad.c_str());                      // count 20 <= 25
    routingmeta::VectorSink sink;
    ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "20");
    assert(sink.Get("x-process-context-overflow") == "true");       // tripped by SIZE, not count
    assert(sink.Count("x-process-context") == 0);
  }

  // --- sys3 scalar projection (nested) + missing-required as data (no throw) ---
  {
    sys3::v1::Submit05Request req;
    req.mutable_job()->mutable_mask()->set_mask_id("RET-9981");
    routingmeta::VectorSink sink;
    auto r = ProjectMeta(req, sink);
    assert(r.ok);                                                   // happy path: ok, no issues (AC3)
    assert(r.issues.empty());
    assert(sink.Get("x-mask-id") == "RET-9981");                    // reached via nested walk
    assert(sink.Get("x-process-context-count") == "0");            // universal pctx skeleton, empty
    assert(sink.Get("x-routing-error").empty());                   // no error header on happy path

    // empty required mask id -> failure-as-data, not a throw (FR1/FR2, AD-5)
    sys3::v1::Submit05Request empty;
    routingmeta::VectorSink s2;
    auto r2 = ProjectMeta(empty, s2);
    assert(!r2.ok);                                                 // blocking issue -> ok=false
    assert(r2.issues.size() == 1);
    assert(r2.issues[0].kind == routingmeta::Issue::MissingRequired);
    assert(r2.issues[0].key == "x-mask-id");
    assert(s2.Get("x-routing-error") == "missing:x-mask-id");       // explicit error header
    assert(s2.Get("x-mask-id").empty());                           // empty scalar NOT emitted
  }

  // --- empty fields project as `Key=` (present-but-empty); digest round-trips (inv. 1) ---
  {
    sys1::v1::CalculateRequest req;
    req.add_contexts()->set_recipe_id("RCP");                       // one sparse context
    routingmeta::VectorSink sink;
    ProjectMeta(req, sink);
    const std::string c = sink.Get("x-process-context");
    assert(c.find("LotID=&") != std::string::npos);                 // empty field present, not omitted
    assert(c.find("RecipeID=RCP") != std::string::npos);            // the one set field
    std::vector<std::string> cs{c};
    assert(routingmeta::VerifyDigest(cs, sink.Get("x-process-context-digest")).ok);  // faithful
  }

  // --- overflow by a single oversized context line (inv. 8, maxline branch) ---
  {
    sys1::v1::CalculateRequest req;
    req.add_contexts()->set_part_id(std::string(600, 'x'));         // one value > 512 B
    routingmeta::VectorSink sink;
    ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "1");             // count<=25, total<7KB:
    assert(sink.Get("x-process-context-overflow") == "true");       // only the line cap can fire
    assert(sink.Count("x-process-context") == 0);
  }

  // --- common headers: 6 present, uniform, routing-selectors absent (inv. 2, 10) ---
  {
    routingmeta::VectorSink sink;
    FillCommon(Runtime{"CORR-X", "F18", "ETCH01"}, sink);
    assert(sink.items.size() == 6);
    assert(sink.Get("x-contract-version") == "v1");
    assert(sink.Get("x-site-id") == "F18");
    assert(sink.Get("x-tool-id") == "ETCH01");
    assert(!sink.Get("x-request-id").empty());
    assert(!sink.Get("x-correlation-id").empty());
    assert(!sink.Get("x-source-system").empty());
    assert(sink.Get("x-target-system").empty());                   // selectors NOT emitted
    assert(sink.Get("x-route-profile").empty());
  }

  std::printf("ALL TESTS PASSED\n");
  return 0;
}
