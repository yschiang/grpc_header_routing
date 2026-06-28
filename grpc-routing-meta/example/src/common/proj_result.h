// =============================================================================
// proj_result.h — failure-as-data result for ProjectMeta (AD-5).
//
// ProjectMeta returns this instead of throwing on a data condition: the caller
// reads ok/issues to decide abort/proceed; the kit only reports (SPEC §7,
// "report, don't dictate"). Leaf header — depends on nothing kit-specific.
// =============================================================================
#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace routingmeta {

struct Issue {
  enum Kind { MissingRequired, Overflow };
  Kind kind{};       // value-init: a default-constructed Issue is deterministic, never UB
  std::string key;   // MissingRequired: the projected header key (e.g. "x-mask-id")
};

struct ProjResult {
  bool ok = true;                       // false only on a blocking issue (missing required)
  std::vector<Issue> issues;
  std::chrono::nanoseconds duration{};  // populated by ProjectMeta's self-timing; read by the caller (no kit logging — NFR7)
};

}  // namespace routingmeta
