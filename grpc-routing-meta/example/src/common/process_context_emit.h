// =============================================================================
// process_context_emit.h — the ONE place the process-context projection policy
// lives. The generated ProjectMeta() builds the per-context query strings (that
// part is field-specific, so it must be generated) and hands them here; this
// helper does count / format / digest / overflow uniformly for every system.
//
// Size guard (the important part): gRPC bounds total metadata. Exceeding it does
// not fail cleanly — APISIX / HTTP2 reset or truncate the stream, an opaque
// "silent" error. So before emitting the (unbounded, repeated) context headers we
// check the WHOLE running metadata size via sink.bytes() and, if projecting would
// blow the budget, emit an explicit x-process-context-overflow flag instead. The
// request still routes on the small common headers; the backend reads full detail
// from the body (spec §7.3 / Appendix C). An opaque transport failure becomes an
// explicit, in-band application signal.
// =============================================================================
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "common/metadata_sink.h"
#include "common/sha256.h"

namespace routingmeta {

// Budget for the metadata WE generate (common + scalar + process-context).
// Transport/auth headers (:path, :authority, te, grpc-*, tokens) count against the
// gRPC limit too and are NOT included here — keep this below the hard limit.
constexpr size_t kMaxTotalMetaBytes = 7168;   // 7 KB — total of all headers WE emit
constexpr size_t kMaxContexts       = 25;     // also cap raw count (spec Appendix C)
constexpr size_t kMaxLineBytes      = 512;    // cap on the encoded VALUE of one context
                                              // (its HPACK entry is this + name + 32)
constexpr size_t kHpackEntryOverhead = 32;    // gRPC/HPACK per-entry, RFC 7541 §4.1;
                                              // must match the +32 in metadata_sink.h Add()

// Emit the Layer 3 process-context headers for one request. `ctxs` are the already
// URL-encoded, key-sorted "k=v&k=v" strings, in body order.
inline void EmitProcessContexts(MetadataSink& sink, const std::vector<std::string>& ctxs) {
  // count + format are always sent: they describe the body even when contexts are
  // suppressed, so the backend knows how many to expect.
  sink.Add("x-process-context-count", std::to_string(ctxs.size()));
  sink.Add("x-process-context-format", "urlencoded-query-string-v1");
  if (ctxs.empty()) return;                                   // count=0: nothing to project

  static const std::string kKey = "x-process-context";
  // What projecting all contexts (+ the digest header) would add to the metadata.
  // Digest entry = name(24) + value 71 ("sha256:" 7 + 64 hex) + HPACK overhead.
  size_t projected = 24 + 71 + kHpackEntryOverhead;
  size_t maxline = 0;
  for (const auto& c : ctxs) {
    projected += kKey.size() + c.size() + kHpackEntryOverhead;
    maxline = std::max(maxline, c.size());
  }

  const bool overflow = ctxs.size() > kMaxContexts
                     || maxline > kMaxLineBytes
                     || sink.bytes() + projected > kMaxTotalMetaBytes;
  if (overflow) {
    sink.Add("x-process-context-overflow", "true");           // explicit, never silent
    return;                                                    // backend reads full detail from body
  }

  // Within budget: digest over the canonical (sorted, '\n'-joined) projection, then
  // one header per context.
  std::string canon;
  for (size_t i = 0; i < ctxs.size(); ++i) {
    if (i) canon.push_back('\n');
    canon += ctxs[i];
  }
  sink.Add("x-process-context-digest", "sha256:" + Sha256Hex(canon));
  for (const auto& c : ctxs) sink.Add(kKey, c);
}

}  // namespace routingmeta
