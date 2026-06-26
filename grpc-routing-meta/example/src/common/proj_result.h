// ProjResult — what a projection reports back (SPEC §7: report, don't dictate).
// The kit never throws on a data condition and never logs; the caller inspects
// `ok`/`issues` and decides (abort vs proceed), and reads `duration` for tracing.
#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace routingmeta {

struct Issue {
  enum Kind { MissingRequired, Overflow } kind;
  std::string key;   // header key for MissingRequired (e.g. "x-mask-id"); empty for Overflow
};

struct ProjResult {
  bool ok = true;                       // false iff a blocking issue (MissingRequired)
  std::vector<Issue> issues;            // non-blocking issues (Overflow) keep ok = true
  std::chrono::nanoseconds duration{};  // wall time of the projection (criterion H)
};

}  // namespace routingmeta
