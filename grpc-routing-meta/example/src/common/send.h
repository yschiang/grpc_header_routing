// =============================================================================
// send.h — the kit's single, branchless sender path (AD-4 / FR6).
//
// Send = FillCommon + generated ProjectMeta, returns ProjResult, never throws on a
// data condition. One template serves every request type with ZERO per-system
// branching; the caller owns the abort/proceed decision (reads ok/issues).
//
// ProjectMeta(req, sink) is a TYPE-DEPENDENT call: it is resolved by ADL on the
// routingmeta::MetadataSink argument at the point of instantiation (where the caller
// has already included the relevant <sys>.proj.h). So this header includes NO
// generated *.proj.h — the kit never depends on per-system generated code (AD-3
// makes this resolution work; AD-4/AR3 the dependency direction it preserves).
// =============================================================================
#pragma once
#include "common/common_headers.h"  // routingmeta::Runtime + FillCommon
#include "common/metadata_sink.h"   // routingmeta::MetadataSink
#include "common/proj_result.h"     // routingmeta::ProjResult

namespace routingmeta {

template <class Req>
ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  FillCommon(rt, sink);
  return ProjectMeta(req, sink);  // ADL on `sink` resolves the generated ProjectMeta
}

}  // namespace routingmeta
