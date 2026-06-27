#undef NDEBUG  // asserts ARE the test harness — never compile them out
// =============================================================================
// bench_projection — criterion H: report per-call projection time. Prints
// average us/call over many iterations for 1/2/25/60 contexts and asserts the
// per-call duration stays sub-millisecond. Uses the same self-timed duration the
// kit reports to callers (ProjResult::duration).
// =============================================================================
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "common/metadata_sink.h"
#include "common/proj_result.h"
#include "sys1.proj.h"

static sys1::v1::CalculateRequest makeReq(int n) {
  sys1::v1::CalculateRequest req;
  req.set_tool_id("ETCH01");
  for (int i = 0; i < n; ++i) {
    auto* c = req.add_contexts();
    c->set_lot_id("LOT" + std::to_string(i));
    c->set_chamber_id("CH-A"); c->set_recipe_id("RCP_ETCH_V3"); c->set_tech("N5");
    c->set_part_id("PART-A"); c->set_stage_id("ETCH"); c->set_operation_no("OP100");
  }
  return req;
}

int main() {
  const int kIters = 2000;
  for (int n : {1, 2, 25, 60}) {
    auto req = makeReq(n);
    std::chrono::nanoseconds total{};
    routingmeta::ProjResult last;
    for (int i = 0; i < kIters; ++i) {
      routingmeta::VectorSink sink;
      last = routingmeta::ProjectMeta(req, sink);
      total += last.duration;
    }
    const double us = total.count() / 1000.0 / kIters;
    std::printf("%3d contexts: %7.3f us/call (last %7.3f us)\n",
                n, us, last.duration.count() / 1000.0);
    assert(us < 1000.0);   // sub-millisecond (criterion H)
  }
  std::printf("BENCH OK (sub-ms)\n");
  return 0;
}
