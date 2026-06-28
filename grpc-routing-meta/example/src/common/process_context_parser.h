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
//
// Lenient by construction (SPEC §6): a malformed escape is passed through
// LITERALLY and never crashes. The `i + 2 < in.size()` bound makes a trailing
// or truncated `%` (e.g. "R%", "%2") read out of range impossible, and the
// `hi >= 0 && lo >= 0` guard rejects non-hex digits (e.g. "%2G", "%ZZ"),
// emitting the `%` and its trailing chars verbatim. Both upper- and lower-case
// hex decode ("%2F" and "%2f" -> '/').
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
// Lenient: a pair without '=' is SKIPPED (not inserted); the first '=' splits
// key/value so values may themselves contain '=' ("k=v=w" -> k->"v=w"). Keys
// and values are UrlDecode'd, so malformed escapes pass through literally.
// Because the backing store is std::map, a DUPLICATE KEY is LAST-WINS: each
// pair assigns kv[key], so the final occurrence overwrites earlier ones
// ("K=a&K=b&K=c" -> K->"c").
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
// the received x-process-context-digest. Detects drift between header and body.
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
    r.error = "no digest provided (overflow or sender omitted)";
    r.ok = false;
  } else {
    r.ok = (r.actual_digest == received_digest);
    if (!r.ok) r.error = "digest mismatch: header/body projection drift";
  }
  return r;
}

}  // namespace routingmeta
