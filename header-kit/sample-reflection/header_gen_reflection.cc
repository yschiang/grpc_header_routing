// =============================================================================
// header_gen_reflection.cc   (SAMPLE A — 入門版,runtime reflection)
//
// 特點:不需改 build / 不需 protoc plugin。link 了 message proto 即可跑。
// 適合:先把整條鏈路(option → header)跑通、低頻發送、快速 PoC。
// 缺點:每次發送都遍歷 descriptor,效能不如 codegen;欄位以字串查找,失去 type safety。
//
// production 高頻路徑請改用 SAMPLE B (protoc plugin codegen)。
//
// ✅ 已驗證(protobuf 3.21.12):編得過,輸出與 PROPOSAL 範例一致。
// 依賴:#include "url_encode.h"(common/ 下共用實作),include path 需含 common/。
//
// 編譯範例:
//   protoc --cpp_out=. -I. header_options.proto request.proto
//   g++ -std=c++17 -I. -I../common header_gen_reflection.cc \
//       header_options.pb.cc request.pb.cc \
//       $(pkg-config --cflags --libs protobuf grpc++) -o demo
// =============================================================================

#include <grpcpp/grpcpp.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>

#include <map>
#include <string>
#include <vector>
#include <optional>
#include <stdexcept>

#include "header_options.pb.h"   // routing::header extension, routing::HeaderGroup
#include "request.pb.h"          // demo::Request (範本訊息)
#include "url_encode.h"          // headergen::UrlEncode — 共用實作(single source)

namespace headergen {

using google::protobuf::Message;
using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Reflection;

// URL-encode 由 url_encode.h 提供(reflection 與 plugin 共用同一份規則)。

// 一個欄位轉成 "key=value"(value 已 URL-encode)。
struct KV { int order; std::string key; std::string value; };

// 把一個 scalar field 的值讀成字串。
std::string ScalarToString(const Message& msg, const FieldDescriptor* f) {
  const Reflection* r = msg.GetReflection();
  switch (f->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: return r->GetString(msg, f);
    case FieldDescriptor::CPPTYPE_INT32:  return std::to_string(r->GetInt32(msg, f));
    case FieldDescriptor::CPPTYPE_INT64:  return std::to_string(r->GetInt64(msg, f));
    case FieldDescriptor::CPPTYPE_UINT32: return std::to_string(r->GetUInt32(msg, f));
    case FieldDescriptor::CPPTYPE_UINT64: return std::to_string(r->GetUInt64(msg, f));
    case FieldDescriptor::CPPTYPE_BOOL:   return r->GetBool(msg, f) ? "true" : "false";
    default:
      throw std::runtime_error("unsupported (non-scalar) header field: " + f->full_name());
  }
}

// 掃一個 message,把標了指定 group 的 scalar 欄位收集成 "k1=v1&k2=v2"。
std::string BuildOneHeaderValue(const Message& msg, routing::HeaderGroup want) {
  const Descriptor* d = msg.GetDescriptor();
  std::vector<KV> kvs;

  for (int i = 0; i < d->field_count(); ++i) {
    const FieldDescriptor* f = d->field(i);
    const auto& opts = f->options();
    if (!opts.HasExtension(routing::header)) continue;

    const routing::HeaderMapping& m = opts.GetExtension(routing::header);
    if (m.group() != want) continue;

    int order = m.order() != 0 ? m.order() : f->number();  // 未填 order 用 field number
    kvs.push_back({order, m.key(), UrlEncode(ScalarToString(msg, f))});
  }

  std::sort(kvs.begin(), kvs.end(),
            [](const KV& a, const KV& b) { return a.order < b.order; });

  std::string out;
  for (size_t i = 0; i < kvs.size(); ++i) {
    if (i) out.push_back('&');
    out += UrlEncode(kvs[i].key);   // key 也 encode,防 key 含特殊字元
    out.push_back('=');
    out += kvs[i].value;
  }
  return out;
}

// -----------------------------------------------------------------------------
// 對外:把 Request 套到 ClientContext 的 metadata。
// 注意這裡示範用泛型遍歷;repeated 子訊息 (Lot / Mask) 需逐一處理。
// 為了 sample 清楚,這支函式針對 demo::Request 寫死 repeated 欄位名。
// reflection 版要做到「完全泛型遍歷任意 message tree」會更複雜,
// 真要泛型化建議直接上 SAMPLE B 的 codegen。
// -----------------------------------------------------------------------------
void ApplyHeaders(const demo::Request& req, grpc::ClientContext* ctx) {
  // ---- TOOL: required, 1 筆 ----
  std::string tool = BuildOneHeaderValue(req, routing::TOOL);
  if (tool.empty())
    throw std::runtime_error("tool-header is required but produced empty value");
  ctx->AddMetadata("tool-header", tool);

  // ---- LOT: 1..N ----
  if (req.lots_size() == 0)
    throw std::runtime_error("lot-header requires at least one lot (cardinality 1..N)");
  for (const auto& lot : req.lots()) {
    std::string v = BuildOneHeaderValue(lot, routing::LOT);
    ctx->AddMetadata("lot-header", v);   // 同名多筆 = repeated metadata
  }

  // ---- MASK: 0..N ----
  for (const auto& mask : req.masks()) {
    std::string v = BuildOneHeaderValue(mask, routing::MASK);
    ctx->AddMetadata("mask-header", v);
  }
}

}  // namespace headergen

// -----------------------------------------------------------------------------
// 使用範例
// -----------------------------------------------------------------------------
//
//   demo::Request req;
//   req.set_eqp_id("T01");
//   req.set_chamber_id("C1");
//   auto* lot = req.add_lots();
//   lot->set_lot_id("L001"); lot->set_step("ST10"); lot->set_recipe("R/A");
//   auto* mask = req.add_masks();
//   mask->set_mask_id("M99"); mask->set_layer_id("METAL1");
//
//   grpc::ClientContext ctx;
//   headergen::ApplyHeaders(req, &ctx);
//   // 此時 ctx 上會帶:
//   //   tool-header: EqpID=T01&ChamberId=C1
//   //   lot-header:  LotId=L001&Step=ST10&Recipe=R%2FA
//   //   mask-header: MaskId=M99&LayerId=METAL1
//   stub->YourRpc(&ctx, req, &resp);
