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
#include "common/send.h"
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
    assert(r.duration.count() > 0);                                 // ProjectMeta self-timed (story 1.6, AC1)
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

  // --- digest gate: accept + every reject form (FR5/AD-9, story 1.11) ---
  {
    auto req = sys1Req(2);
    routingmeta::VectorSink sink;
    ProjectMeta(req, sink);
    std::vector<std::string> cs; std::string dg;
    for (auto& kv : sink.items) {
      if (kv.first == "x-process-context") cs.push_back(kv.second);
      else if (kv.first == "x-process-context-digest") dg = kv.second;
    }
    // accept
    auto ok = routingmeta::VerifyDigest(cs, dg);
    assert(ok.ok);
    assert(ok.actual_digest == ok.expected_digest);
    // reject: tampered context value
    { auto t = cs; t[0] += "&INJECTED=1";
      auto r = routingmeta::VerifyDigest(t, dg);
      assert(!r.ok); assert(r.error.find("mismatch") != std::string::npos); }
    // reject: corrupted digest header, intact body
    { auto r = routingmeta::VerifyDigest(cs, "sha256:" + std::string(64, '0'));
      assert(!r.ok); assert(r.actual_digest != r.expected_digest);
      assert(r.error.find("mismatch") != std::string::npos); }
    // reject: dropped context (count drift)
    { auto t = cs; t.pop_back();
      auto r = routingmeta::VerifyDigest(t, dg);
      assert(!r.ok); }
    // reject: no digest provided (overflow / sender omitted)
    { auto r = routingmeta::VerifyDigest(cs, "");
      assert(!r.ok); assert(r.error.find("no digest") != std::string::npos); }
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
    auto r = ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "30");
    assert(sink.Get("x-process-context-format") == "urlencoded-query-string-v1");  // survives overflow (inv. 8)
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0);
    assert(sink.Get("x-process-context-digest").empty());
    assert(r.ok);                                                   // overflow is non-blocking (FR3, AC1)
    assert(r.issues.size() == 1);                                   // exactly one Overflow issue (AC2)
    assert(r.issues[0].kind == routingmeta::Issue::Overflow);
  }

  // --- overflow by BYTES (<=25 contexts but total > 7KB via padded field) ---
  {
    const std::string pad(300, 'x');                                // each context ~390 bytes
    auto req = sys1Req(20, "RCP", pad.c_str());                      // count 20 <= 25
    routingmeta::VectorSink sink;
    auto r = ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "20");
    assert(sink.Get("x-process-context-overflow") == "true");       // tripped by SIZE, not count
    assert(sink.Count("x-process-context") == 0);
    assert(r.ok);                                                   // non-blocking (AC1)
    assert(r.issues.size() == 1);
    assert(r.issues[0].kind == routingmeta::Issue::Overflow);
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

  // --- co-occurrence: missing required scalar AND overflow -> both issues, ok=false ---
  {
    sys3::v1::Submit05Request req;                          // mask_id left empty (required)
    for (int i = 0; i < 30; ++i) req.add_contexts();        // >25 contexts -> overflow
    routingmeta::VectorSink sink;
    auto r = ProjectMeta(req, sink);
    assert(!r.ok);                                          // blocking missing-required dominates
    assert(r.issues.size() == 2);                          // MissingRequired + non-blocking Overflow
    assert(sink.Get("x-routing-error") == "missing:x-mask-id");
    assert(sink.Get("x-process-context-overflow") == "true");
    bool hasMissing = false, hasOverflow = false;
    for (const auto& is : r.issues) {
      hasMissing  |= is.kind == routingmeta::Issue::MissingRequired;
      hasOverflow |= is.kind == routingmeta::Issue::Overflow;
    }
    assert(hasMissing && hasOverflow);                     // both reported, order-independent
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
    auto r = ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-count") == "1");             // count<=25, total<7KB:
    assert(sink.Get("x-process-context-overflow") == "true");       // only the line cap can fire
    assert(sink.Count("x-process-context") == 0);
    assert(r.ok);                                                   // non-blocking (AC1)
    assert(r.issues.size() == 1);                                   // line-cap trigger (maxline branch)
    assert(r.issues[0].kind == routingmeta::Issue::Overflow);
  }

  // --- common headers: 6 present, uniform, routing-selectors absent (inv. 2, 10) ---
  {
    routingmeta::VectorSink sink;
    FillCommon(routingmeta::Runtime{"CORR-X", "F18", "ETCH01"}, sink);
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

  // --- routingmeta::Send: branchless kit sender = FillCommon + ProjectMeta, no throw ---
  {
    sys3::v1::Submit05Request good;
    good.mutable_job()->mutable_mask()->set_mask_id("RET-1");
    routingmeta::VectorSink s1;
    auto rg = routingmeta::Send(good, routingmeta::Runtime{"C", "F18", "TOOL1"}, s1);
    assert(rg.ok);                                                  // happy path through Send
    assert(rg.issues.empty());
    assert(rg.duration.count() > 0);                                // Send propagates ProjectMeta's duration (AC2)
    assert(s1.Get("x-mask-id") == "RET-1");                         // ProjectMeta ran
    assert(s1.Get("x-tool-id") == "TOOL1");                         // FillCommon ran -> Send = both

    sys3::v1::Submit05Request bad;                                  // empty required mask
    routingmeta::VectorSink s2;
    auto rb = routingmeta::Send(bad, routingmeta::Runtime{"C", "F18", "TOOL1"}, s2);  // must NOT throw
    assert(!rb.ok);                                                 // FR1 binds Send: propagate, don't throw
    assert(rb.issues.size() == 1);
    assert(rb.issues[0].kind == routingmeta::Issue::MissingRequired);
    assert(s2.Get("x-routing-error") == "missing:x-mask-id");
    assert(s2.Get("x-mask-id").empty());                           // empty scalar suppressed
  }

  // ===========================================================================
  // Story 1.12 — regression locks. Pure assertions over CURRENT behavior; no kit,
  // plugin, proto, or wire byte changes. Each block pins a contract so a future
  // refactor that drifts it fails here.
  // ===========================================================================

  // --- 1.12 Task 1: golden canonical-projection vector (AC1) ---
  // One context, all 7 fields (PartID empty, RecipeID has '/'). Pinned to an
  // INDEPENDENT literal: key names, sort order, '&'-join, %2F encoding, and the
  // present-but-empty `PartID=` policy all fail here if any of them drifts.
  {
    sys1::v1::CalculateRequest req;
    auto* c = req.add_contexts();
    c->set_chamber_id("CH-A");
    c->set_lot_id("LOT01");
    c->set_operation_no("OP100");
    c->set_part_id("");                                    // empty -> PartID=
    c->set_recipe_id("R/A");                               // '/' -> %2F
    c->set_stage_id("ETCH");
    c->set_tech("N5");
    routingmeta::VectorSink sink;
    auto r = ProjectMeta(req, sink);
    assert(r.ok);
    assert(sink.Count("x-process-context") == 1);
    assert(sink.Get("x-process-context") ==
           "ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=&RecipeID=R%2FA&StageID=ETCH&Tech=N5");
    // Also lock the DIGEST value, not just its presence: for one context the digest
    // preimage IS the line above, so this pins the preimage construction (join/order)
    // against an INDEPENDENT reference — `printf '%s' '<line>' | shasum -a 256`. If the
    // digest were `!empty()`-only, a preimage drift (separator/order) would slip through.
    assert(sink.Get("x-process-context-digest") ==
           "sha256:3c8087d9f3dcb8c057146eabdd75b4b004f548160d1c636450176925465bf31b");
  }

  // --- 1.12 Task 2: determinism — same request projects byte-identically twice (AC2) ---
  {
    auto req = sys1Req(3);
    routingmeta::VectorSink a, b;
    ProjectMeta(req, a);
    ProjectMeta(req, b);
    assert(a.items == b.items);                            // full key/value/order seq, incl. digest
  }

  // --- 1.12 Task 3: SHA-256 KATs (independent published vectors) + url round-trips (AC3) ---
  // Expected values are NIST/published, NEVER recomputed with the kit.
  assert(routingmeta::Sha256Hex("") ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(routingmeta::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");  // 56-byte NIST KAT
  assert(routingmeta::Sha256Hex(std::string(1000000, 'a')) ==
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");  // 1M 'a' NIST KAT
  assert(routingmeta::Sha256Hex(std::string(55, 'a')) ==                       // sha256(b'a'*55), pinned
         "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  assert(routingmeta::UrlDecode("R%2FA") == "R/A");                            // %2F -> '/'
  assert(routingmeta::UrlDecode(routingmeta::UrlEncode("\xC3\xA9")) == "\xC3\xA9");  // high-byte round-trip

  // --- 1.12 Task 4: all 10 sys3 mask paths -> same x-mask-id (AC1) ---
  // Each Submit message carries the id at a different body path; one annotation,
  // the generated walkProj reaches each one.
  { routingmeta::VectorSink s; sys3::v1::Submit01Request r; r.set_mask_id("RET-01");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-01"); }              // top-level
  { routingmeta::VectorSink s; sys3::v1::Submit02Request r; r.set_reticle_id("RET-02");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-02"); }              // renamed
  { routingmeta::VectorSink s; sys3::v1::Submit03Request r; r.mutable_mask()->set_id("RET-03");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-03"); }              // nested 1
  { routingmeta::VectorSink s; sys3::v1::Submit04Request r; r.mutable_layer()->set_mask_no("RET-04");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-04"); }              // nested 1, renamed
  { routingmeta::VectorSink s; sys3::v1::Submit05Request r; r.mutable_job()->mutable_mask()->set_mask_id("RET-05");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-05"); }              // nested 2
  { routingmeta::VectorSink s; sys3::v1::Submit06Request r; r.mutable_request_header()->set_mask_id("RET-06");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-06"); }              // sub-msg
  { routingmeta::VectorSink s; sys3::v1::Submit07Request r; r.set_photo_mask_id("RET-07");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-07"); }              // renamed top-level
  { routingmeta::VectorSink s; sys3::v1::Submit08Request r; r.mutable_mask_block()->set_mask_ref("RET-08");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-08"); }              // nested
  { routingmeta::VectorSink s; sys3::v1::Submit09Request r; r.set_maskid("RET-09");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-09"); }              // top-level
  { routingmeta::VectorSink s; sys3::v1::Submit10Request r; r.mutable_exposure()->mutable_mask()->set_id("RET-10");
    assert(ProjectMeta(r, s).ok); assert(s.Get("x-mask-id") == "RET-10"); }              // nested 2
  // empty-required on a NON-Submit05, deep-nested message -> failure-as-data
  { sys3::v1::Submit10Request empty; routingmeta::VectorSink s;
    auto r = ProjectMeta(empty, s);
    assert(!r.ok);
    assert(r.issues.size() == 1 && r.issues[0].kind == routingmeta::Issue::MissingRequired);
    assert(s.Get("x-routing-error") == "missing:x-mask-id");
    assert(s.Get("x-mask-id").empty()); }

  // --- 1.12 Task 5: exact-threshold overflow boundaries (strict '>' locks; AC2-adjacent) ---
  // count: 25 -> NO overflow (digest present); 26 -> overflow. Locks `ctxs.size() > 25`.
  { sys1::v1::CalculateRequest req;
    for (int i = 0; i < 25; ++i) req.add_contexts();                      // 25 tiny contexts
    routingmeta::VectorSink sink; auto r = ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-overflow").empty());
    assert(!sink.Get("x-process-context-digest").empty());
    assert(sink.Count("x-process-context") == 25);
    assert(r.issues.empty()); }
  { sys1::v1::CalculateRequest req;
    for (int i = 0; i < 26; ++i) req.add_contexts();                      // 26 -> count>25
    routingmeta::VectorSink sink; ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0); }
  // line: encoded line EXACTLY 512 -> NO overflow; 513 -> overflow. Locks `maxline > 512`.
  // Calibrated: empty-field skeleton line = 63 B; PartID pad 449 -> 512, 450 -> 513.
  { sys1::v1::CalculateRequest req;
    req.add_contexts()->set_part_id(std::string(449, 'x'));               // line == 512
    routingmeta::VectorSink sink; ProjectMeta(req, sink);
    assert(sink.Get("x-process-context").size() == 512);
    assert(sink.Get("x-process-context-overflow").empty());
    assert(!sink.Get("x-process-context-digest").empty()); }
  { sys1::v1::CalculateRequest req;
    req.add_contexts()->set_part_id(std::string(450, 'x'));               // line == 513 > 512
    routingmeta::VectorSink sink; ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0); }
  // total: projected metadata total EXACTLY 7168 -> NO overflow; 7169 -> overflow.
  // Locks `sink.bytes() + projected > 7168`. Calibrated: 21 ctx @ PartID(200) + 1 ctx
  // @ PartID(238) -> sink.bytes()==7168 (each line<=512, count 22<=25); +1 byte -> 7169.
  { sys1::v1::CalculateRequest req;
    for (int i = 0; i < 21; ++i) req.add_contexts()->set_part_id(std::string(200, 'y'));
    req.add_contexts()->set_part_id(std::string(238, 'z'));
    routingmeta::VectorSink sink; ProjectMeta(req, sink);
    assert(sink.bytes() == 7168);                                         // total at threshold
    assert(sink.Get("x-process-context-overflow").empty());
    assert(!sink.Get("x-process-context-digest").empty()); }
  { sys1::v1::CalculateRequest req;
    for (int i = 0; i < 21; ++i) req.add_contexts()->set_part_id(std::string(200, 'y'));
    req.add_contexts()->set_part_id(std::string(239, 'z'));               // +1 byte -> 7169 > 7168
    routingmeta::VectorSink sink; ProjectMeta(req, sink);
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0); }

  // ===========================================================================
  // Story 1.13 — parser robustness (lenient, no-crash). Receiver-side LOCK: the
  // parser is lenient by construction; these non-vacuous asserts PROVE it. No
  // sender/wire/projection byte changes — UrlDecode/ParseContext/VerifyDigest
  // never touch emitted bytes. "No crash" is proven by reaching ALL TESTS PASSED.
  // ===========================================================================

  // --- 1.13 Task 1: malformed %-escape passes through LITERALLY (AC1, HR2) ---
  // Traced against UrlDecode: bound `i+2 < size` makes trailing/truncated `%`
  // un-decodable; guard `hi>=0 && lo>=0` rejects non-hex (SPEC §6).
  assert(routingmeta::UrlDecode("R%") == "R%");                  // trailing %
  assert(routingmeta::UrlDecode("%") == "%");
  assert(routingmeta::UrlDecode("%2") == "%2");                  // truncated (only 1 trailing char)
  assert(routingmeta::UrlDecode("a%2") == "a%2");
  assert(routingmeta::UrlDecode("%2G") == "%2G");                // non-hex lo
  assert(routingmeta::UrlDecode("%G2") == "%G2");                // non-hex hi
  assert(routingmeta::UrlDecode("%ZZ") == "%ZZ");
  assert(routingmeta::UrlDecode("%2F") == "/");                  // well-formed UPPER hex
  assert(routingmeta::UrlDecode("%2f") == "/");                  // well-formed lower hex
  assert(routingmeta::UrlDecode("a%2Fb%ZZc%") == "a/b%ZZc%");    // valid + bad + trailing
  // same leniency reached through ParseContext (decodes key+value)
  assert(routingmeta::ParseContext("RecipeID=R%2FA")["RecipeID"] == "R/A");
  assert(routingmeta::ParseContext("RecipeID=R%ZZ")["RecipeID"] == "R%ZZ");

  // --- 1.13 Task 2: duplicate keys -> last-wins (std::map assignment) (AC2) ---
  assert(routingmeta::ParseContext("RecipeID=A&RecipeID=B")["RecipeID"] == "B");
  assert(routingmeta::ParseContext("K=a&K=b&K=c")["K"] == "c");   // 3-way
  { auto m = routingmeta::ParseContext("K=a&K=b&Other=x");        // unrelated key unaffected
    assert(m["K"] == "b"); assert(m["Other"] == "x"); assert(m.size() == 2); }

  // --- 1.13 Task 3: garbled corpus -> no crash, exact maps (AC3) ---
  assert(routingmeta::ParseContext("").empty());                 // size 0
  assert(routingmeta::ParseContext("%").empty());                // no '=' -> skipped
  assert(routingmeta::ParseContext("%%%").empty());
  assert(routingmeta::ParseContext("&&&").empty());              // all empty pairs, no '='
  { auto m = routingmeta::ParseContext("&=&=&");                 // pairs "=" -> ""->""
    assert(m.size() == 1); assert(m[""] == ""); }
  { auto m = routingmeta::ParseContext("=");  assert(m.size() == 1); assert(m[""] == ""); }
  { auto m = routingmeta::ParseContext("=v"); assert(m.size() == 1); assert(m[""] == "v"); }
  { auto m = routingmeta::ParseContext("k="); assert(m.size() == 1); assert(m["k"] == ""); }
  assert(routingmeta::ParseContext("k").empty());                // no '=' -> skipped
  assert(routingmeta::ParseContext("k=v=w")["k"] == "v=w");      // first '=' splits
  // high-byte raw value: non-'%' bytes pass through unchanged
  assert(routingmeta::ParseContext(std::string("k=") + "\xC3\xA9")["k"] == "\xC3\xA9");
  // long value: no truncation / overflow
  assert(routingmeta::ParseContext("k=" + std::string(5000, 'x'))["k"].size() == 5000);
  // VerifyDigest robustness: errors-as-data, never throws
  assert(routingmeta::VerifyDigest({}, "").ok == false);
  assert(routingmeta::VerifyDigest({"garb=%", "x"},
                                   "sha256:" + std::string(64, '0')).ok == false);

  std::printf("ALL TESTS PASSED\n");
  return 0;
}
