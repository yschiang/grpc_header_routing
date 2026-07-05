// =============================================================================
// common_headers.h — the 6 sender-known common headers, filled UNIFORMLY for every
// system (no per-system branching). Lifted out of main() so both the sender and the
// tests exercise the same FillCommon. `x-contract-version` is the only true constant;
// everything else is supplied by the caller via Runtime (SPEC §3: x-request-id MUST
// be unique per request, x-source-system is sender identity — neither can be a
// kit-side constant).
// =============================================================================
#pragma once
#include <string>

#include "common/metadata_sink.h"

// Runtime facts the sender knows at call time. The gRPC :path lacks the fab and the
// tool, so those ride as headers; everything else is constant or generated.
struct Runtime {
  std::string correlation_id;
  std::string site_id;
  std::string tool_id;
  std::string request_id;      // MUST be unique per request (SPEC §3)
  std::string source_system;   // sender identity, e.g. "eap" (SPEC §3)
};

inline void FillCommon(const Runtime& rt, routingmeta::MetadataSink& sink) {
  sink.Add("x-request-id", rt.request_id);
  sink.Add("x-correlation-id", rt.correlation_id);
  sink.Add("x-contract-version", "v1");
  sink.Add("x-source-system", rt.source_system);
  sink.Add("x-site-id", rt.site_id);
  sink.Add("x-tool-id", rt.tool_id);
}
