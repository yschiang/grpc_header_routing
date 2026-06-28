// =============================================================================
// bench_projection — sub-millisecond micro-bench for the generated ProjectMeta.
// Times the sys1 process-context projection for N in {1,2,25,60} contexts and
// proves the MEAN per-call time stays well under a 1 ms budget. Resolution-robust:
// averages a loop of kIters calls inside ONE steady_clock interval (a single call
// is too short to time against clock granularity). This is criterion-H evidence
// ("sub-ms / perf observed"), not a per-call tail-latency SLA.
//   ponytail: gates on the mean per-call (robust, non-flaky); a per-call max/p99
//   gate would trip on OS scheduling noise, not projection cost — add only if a
//   real tail-latency guarantee is ever required.
// N=60 > kMaxContexts(25) deliberately trips the overflow short-circuit (no digest
// / per-context emit), so its per-call time is LOWER than N=25 — the heaviest
// in-budget case (full encode + sha256 + 25 emits). Fresh sink each iteration (the
// sink accumulates headers, so reuse would change behavior). No test deps;
// standalone main, non-zero exit on budget breach so it gates in the build.
// =============================================================================
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>

#include "common/metadata_sink.h"
#include "sys1.proj.h"

// Minimal request shape copied from tests/test_projection.cc sys1Req(): tool_id
// plus n contexts, each with the 7 string fields ProjectMeta projects.
static sys1::v1::CalculateRequest makeReq(int n) {
  sys1::v1::CalculateRequest req;
  req.set_tool_id("ETCH01");
  for (int i = 0; i < n; ++i) {
    auto* c = req.add_contexts();
    c->set_lot_id("LOT" + std::to_string(i));
    c->set_chamber_id("CH-A");
    c->set_recipe_id("R/A");
    c->set_tech("N5");
    c->set_part_id("PART-A");
    c->set_stage_id("ETCH");
    c->set_operation_no("OP100");
  }
  return req;
}

int main() {
  constexpr int kIters = 2000;
  constexpr long long kBudgetNs = 1000000;  // 1 ms budget
  const int sizes[] = {1, 2, 25, 60};

  bool failed = false;
  std::size_t guard = 0;

  for (int n : sizes) {
    auto req = makeReq(n);  // built once; only the projection is timed

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      routingmeta::VectorSink sink;            // fresh each call (sink accumulates)
      auto r = ProjectMeta(req, sink);         // ADL on routingmeta:: sink arg
      guard += sink.items.size() + (r.ok ? 1 : 0);  // defeat -O2 elision
    }
    auto t1 = std::chrono::steady_clock::now();

    long long total = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    long long per = total / kIters;
    std::printf("projection: N=%2d contexts -> %8.3f us/call (%lld ns, %d iters)\n",
                n, per / 1000.0, per, kIters);
    if (per >= kBudgetNs) {
      std::fprintf(stderr, "FAIL: N=%d per-call %lld ns >= budget %lld ns\n", n, per, kBudgetNs);
      failed = true;
    }
  }

  std::printf("%s (guard=%zu)\n", failed ? "BENCH FAILED" : "BENCH PASSED", guard);
  return failed ? 1 : 0;
}
