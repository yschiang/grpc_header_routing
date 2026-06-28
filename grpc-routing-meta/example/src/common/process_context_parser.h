// =============================================================================
// process_context_parser.h — receiver-side parse & verify
//
// The mirror of the sender projection: takes the x-process-context headers a
// gateway/backend received and (1) parses each into key/value pairs, (2) verifies
// the sha256 digest matches, detecting projection/serialization drift.
//
// Header-only so the receiver app and tests can use it without extra build wiring.
// =============================================================================
#pragma once
#include <map>
#include <string>
#include <vector>

#include "common/sha256.h"
#include "common/url_encode.h"

namespace routingmeta {

// URL-decode (inverse of UrlEncode): %XX -> byte, leave others as-is.
inline std::string UrlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

// Parse one "k=v&k=v" context string into an ordered key->value map (decoded).
//
// Policy: LENIENT, LAST-WINS. This parses bytes already received over the wire, so it
// never throws or aborts on malformed input: a pair without '=' is skipped, a truncated
// or invalid %-escape is emitted literally (see UrlDecode), and a duplicate key keeps the
// LAST value (std::map assignment). Robustness over strictness — a mangled header
// degrades to a best-effort parse; VerifyDigest is what actually catches drift/tampering.
inline std::map<std::string, std::string> ParseContext(const std::string& ctx) {
  std::map<std::string, std::string> kv;
  size_t i = 0;
  while (i < ctx.size()) {
    size_t amp = ctx.find('&', i);
    std::string pair = ctx.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      kv[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return kv;
}

struct VerifyResult {
  bool ok = false;
  std::string expected_digest;  // from header
  std::string actual_digest;    // recomputed
  std::string error;
};

// Recompute the digest over the received contexts (same canonical rule as the
// sender: contexts already key-sorted internally; join by '\n') and compare to
// the received x-process-context-digest **if one is present** (verify-if-present);
// an absent digest is skipped, not treated as drift. Detects drift between header and body.
inline VerifyResult VerifyDigest(const std::vector<std::string>& contexts,
                                 const std::string& received_digest) {
  VerifyResult r;
  r.expected_digest = received_digest;
  std::string canon;
  for (size_t i = 0; i < contexts.size(); ++i) {
    if (i) canon.push_back('\n');
    canon += contexts[i];
  }
  r.actual_digest = "sha256:" + Sha256Hex(canon);
  if (received_digest.empty()) {
    // Verify-if-present: an ABSENT digest is not drift. The sender may have omitted it
    // (emit_digest=false), or overflow suppressed it — there is nothing to recompute
    // against, so accept. A present-but-wrong digest still rejects below.
    r.ok = true;
    return r;
  }
  r.ok = (r.actual_digest == received_digest);
  if (!r.ok) r.error = "digest mismatch: header/body projection drift";
  return r;
}

}  // namespace routingmeta
