// Send<>() — the one call a client makes: common headers + body projection, timed.
// ProjectMeta is resolved by ADL at instantiation (the caller's TU includes the
// relevant generated *.proj.h). No per-system/method branching — the overload is
// chosen by Req. Returns ProjResult: the caller inspects ok/issues and decides.
#pragma once
#include "common/common_headers.h"
#include "common/metadata_sink.h"
#include "common/proj_result.h"

namespace routingmeta {

template <class Req>
ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  FillCommon(rt, sink);
  return ProjectMeta(req, sink);
}

}  // namespace routingmeta
