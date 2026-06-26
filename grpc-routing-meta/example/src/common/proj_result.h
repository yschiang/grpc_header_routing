// =============================================================================
// proj_result.h — what a projection reports back to the caller.
//
// ProjectMeta/Send return this instead of throwing or logging: the kit REPORTS,
// the caller DECIDES (abort the RPC, dead-letter, or just metric). Keeps the kit
// free of any logging/metrics dependency — the host wires `issues` / `duration`
// into its own telemetry stack.
// =============================================================================
#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace routingmeta {

// One notable thing that happened during projection.
struct Issue {
  enum Kind {
    MissingRequired,  // a required (routing.project) scalar was empty — the request is
                      // unroutable on that key. BLOCKING: sets ProjResult::ok = false.
    Overflow,         // process-context metadata exceeded the gRPC budget; the lines were
                      // suppressed and x-process-context-overflow emitted. NON-blocking:
                      // the request still routes (common headers + body fallback).
  };
  Kind kind;
  std::string key;    // header key involved, e.g. "x-mask-id" / "x-process-context"
};

struct ProjResult {
  bool ok = true;                         // false iff a BLOCKING issue occurred
  std::vector<Issue> issues;              // everything notable, for the caller's metrics/logs
  std::chrono::nanoseconds duration{0};   // projection wall-time (stamped by Send)
};

}  // namespace routingmeta
