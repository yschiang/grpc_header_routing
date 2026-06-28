// =============================================================================
// common_headers.h — the 6 sender-known common headers, filled UNIFORMLY for every
// system (no per-system branching). Lifted out of main() so both the sender and the
// tests exercise the same FillCommon. The request-id / source-system / contract
// values are demo constants; a real sender supplies its own.
// =============================================================================
#pragma once
#include <string>

#include "common/metadata_sink.h"

namespace routingmeta {

// Runtime facts the sender knows at call time. The gRPC :path lacks the fab and the
// tool, so those ride as headers; everything else is constant or generated.
struct Runtime {
  std::string correlation_id;
  std::string site_id;
  std::string tool_id;
};

inline void FillCommon(const Runtime& rt, MetadataSink& sink) {
  sink.Add("x-request-id", "REQ-DEMO-0001");  // generated per request in production
  sink.Add("x-correlation-id", rt.correlation_id);
  sink.Add("x-contract-version", "v1");
  sink.Add("x-source-system", "eap");
  sink.Add("x-site-id", rt.site_id);
  sink.Add("x-tool-id", rt.tool_id);
}

}  // namespace routingmeta
