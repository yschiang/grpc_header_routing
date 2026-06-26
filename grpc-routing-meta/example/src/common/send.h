// =============================================================================
// send.h — the ONE call a sender makes: FillCommon + ProjectMeta, timed.
//
// Generated ProjectMeta lives in namespace routingmeta, so the call below resolves
// by ADL through the MetadataSink argument — include order doesn't matter, and the
// consumer never names a system. Returns the ProjResult so the caller can metric
// `issues`, read `duration`, and decide abort-vs-proceed on `ok`.
// =============================================================================
#pragma once
#include <chrono>

#include "common/common_headers.h"
#include "common/metadata_sink.h"
#include "common/proj_result.h"

namespace routingmeta {

template <class Req>
ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  const auto t0 = std::chrono::steady_clock::now();
  FillCommon(rt, sink);
  ProjResult r = ProjectMeta(req, sink);   // ADL -> routingmeta::ProjectMeta (via sink)
  r.duration = std::chrono::steady_clock::now() - t0;
  return r;
}

}  // namespace routingmeta
