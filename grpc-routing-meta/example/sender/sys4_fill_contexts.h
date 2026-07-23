#pragma once
// =============================================================================
// sys4 producer-side normalization — NOT part of the kit (ADR 0001: filling the
// body is the producer's job). Three exclusive batch shapes carry lot ids under
// different field names; this maps new_mat_lot_id -> contexts (LotID only).
// Design + wire examples: docs/sys4-update-material.zh.md.
// =============================================================================
#include <string>

#include "sys4.pb.h"

namespace sys4demo {

// false = exclusive-source violation (>1 source non-empty): producer MUST refuse
// to send — never a silent first-pick. contexts is left untouched in that case.
// Caller contract: call once per freshly-built request, before Send — a second call
// on the same request would append duplicate contexts.
inline bool FillContexts(sys4::v1::UpdateMaterialRequest& req) {
  const int sources = (req.id_change_size() > 0) + (req.lot_add_size() > 0) +
                      (req.lot_change_size() > 0);
  if (sources > 1) return false;
  // Only LotID; the other 6 fields stay empty and project as `Key=` (faithful).
  auto add = [&req](const std::string& lot) { req.add_contexts()->set_lot_id(lot); };
  for (const auto& e : req.id_change())  add(e.new_mat_lot_id());
  for (const auto& e : req.lot_add())    add(e.new_mat_lot_id());
  for (const auto& e : req.lot_change()) add(e.new_mat_lot_id());
  return true;
}

}  // namespace sys4demo
