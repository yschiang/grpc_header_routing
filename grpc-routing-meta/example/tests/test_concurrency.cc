// =============================================================================
// test_concurrency — proves ProjectMeta is re-entrant (Story 1.14, AD-12/NFR5,
// HR3). One immutable request is read concurrently by N threads; each thread
// projects into its OWN VectorSink and must reproduce a single-threaded baseline
// byte-for-byte. No locks anywhere: if ProjectMeta touched shared mutable state,
// this fails the equality oracle and/or trips ThreadSanitizer (CI race gate).
// Plain asserts, zero test deps — stdlib <thread>/<atomic> + -pthread only.
// =============================================================================
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/metadata_sink.h"
#include "sys1.proj.h"

// Mirror test_projection.cc's sys1Req: tool_id + n process contexts, all 7 pctx
// fields set, recipe_id carries a reserved char so encoding runs on every call.
static sys1::v1::CalculateRequest sys1Req(int n) {
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
  const sys1::v1::CalculateRequest req = sys1Req(6);  // immutable, shared by all threads

  routingmeta::VectorSink base;
  ProjectMeta(req, base);
  const std::vector<std::pair<std::string, std::string>> baseline = base.items;

  constexpr int N = 8, M = 2000;
  std::atomic<bool> mismatch{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < N; ++t) {
    threads.emplace_back([&req, &baseline, &mismatch] {
      for (int i = 0; i < M; ++i) {
        routingmeta::VectorSink s;            // thread-local sink
        ProjectMeta(req, s);                  // concurrent read of the one const req
        if (s.items != baseline) mismatch.store(true);
      }
    });
  }
  for (auto& th : threads) th.join();

  assert(!mismatch.load());                   // hard functional oracle
  std::printf("CONCURRENCY TEST PASSED\n");
  return 0;
}
