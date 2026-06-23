// =============================================================================
// header_gen_plugin.cc   (SAMPLE B — production 版,protoc plugin codegen)
//
// 在 build time 讀每個 message 的 (routing.header) option,生成 type-safe 的
// ApplyHeaders()。zero reflection、無字串查找、發送路徑最快。
// 適合:online-critical 高頻發送。
//
// ✅ 已驗證(protobuf 3.21.12):plugin 編得過、能跑、生成的 code 編得過且輸出
//    與 PROPOSAL 範例 byte-identical;非 scalar 欄位標 option 會在 protoc build 時失敗。
//
// 依賴:生成的 *.headers.h 會 #include "url_encode.h"(common/ 下的共用實作),
//       sender 的 include path 需含 common/。reflection 版與本版共用同一支 UrlEncode。
//
// 編譯 plugin:
//   protoc --cpp_out=. header_options.proto
//   g++ -std=c++17 header_gen_plugin.cc header_options.pb.cc \
//       $(pkg-config --cflags --libs protobuf) \
//       -lprotoc -o protoc-gen-header
//
// 用 plugin 生成 header builder(掛進 sender 的 build):
//   protoc --plugin=protoc-gen-header \
//          --header_out=. -I. request.proto
//   # 產出 request.headers.h / request.headers.cc
//
// CMake/Bazel 整合:把上面這條 protoc 命令包成 custom build step,
// 「改 request.proto → 重新 codegen → header builder 自動跟上」即成 build 的一環。
// =============================================================================

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/zero_copy_stream.h>

#include <algorithm>
#include <string>
#include <vector>

#include "header_options.pb.h"   // plugin 必須 link option 定義才讀得到 extension

using namespace google::protobuf;
using namespace google::protobuf::compiler;

namespace {

struct FieldInfo { int order; std::string key; std::string getter; };

// proto package -> C++ namespace。package "a.b.c" 對應 namespace "a::b::c",
// 不能直接拿 package() 當 namespace(含點會生成不合法 code)。
std::string CppNamespace(const FileDescriptor* file) {
  std::string ns = file->package();
  std::string out;
  for (char c : ns) out += (c == '.') ? std::string("::") : std::string(1, c);
  return out;  // 空 package -> 空字串(global namespace)
}

// 掃一個 message 內標了指定 group 的欄位,收集 scalar;若有人把 option 標在
// 非 scalar(巢狀 message / repeated 等)上,寫入 *error 讓 protoc 在 build 時失敗
// (對齊 CONTEXT C5「僅 scalar」與「編譯期擋錯」賣點)。回傳是否成功。
bool CollectGroup(const Descriptor* d, routing::HeaderGroup want,
                  std::vector<FieldInfo>* out, std::string* error) {
  for (int i = 0; i < d->field_count(); ++i) {
    const FieldDescriptor* f = d->field(i);
    if (!f->options().HasExtension(routing::header)) continue;
    const routing::HeaderMapping& m = f->options().GetExtension(routing::header);
    if (m.group() != want) continue;

    // 拒絕非 scalar:message / group / repeated 標 header 目前不支援。
    if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE || f->is_repeated()) {
      if (error) {
        *error = "field '" + f->full_name() +
                 "' 標了 (routing.header) 但不是 scalar 欄位;目前僅支援 "
                 "string/int/bool 等 scalar 進 header (見 CONTEXT C5)。";
      }
      return false;
    }
    if (m.key().empty()) {
      if (error) *error = "field '" + f->full_name() + "' 的 (routing.header).key 不可為空。";
      return false;
    }

    int order = m.order() != 0 ? m.order() : f->number();
    out->push_back({order, m.key(), f->name() + "()"});  // C++ accessor = field 名小寫
  }
  std::sort(out->begin(), out->end(),
            [](const FieldInfo& a, const FieldInfo& b) { return a.order < b.order; });
  return true;
}

// 純查詢版(不報錯):只判斷「這個 message 在此 group 是否有任何 scalar 欄位」,
// 用於頂層 message 篩選與 repeated 子訊息辨識。
bool HasGroup(const Descriptor* d, routing::HeaderGroup want) {
  std::vector<FieldInfo> tmp; std::string ignore;
  // 這裡若遇非 scalar 會回 false,但辨識用途只需知道「有沒有合法 scalar」,足夠。
  CollectGroup(d, want, &tmp, &ignore);
  return !tmp.empty();
}

// 生成把一組欄位串成 "k=v&k=v" 的 C++ 片段,寫進 printer。
// key 與 value 都走 headergen::UrlEncode(對齊 PROPOSAL §7「key 與 value 都 encode」,
// 與 reflection 版行為一致)。
void EmitBuildValue(io::Printer& p, const std::vector<FieldInfo>& fs,
                    const std::string& dst, const std::string& src) {
  p.Print("  std::string $dst$;\n", "dst", dst);
  for (const auto& f : fs) {
    p.Print("  if (!$dst$.empty()) $dst$.push_back('&');\n", "dst", dst);
    // key 也 encode:與 reflection 版一致。key 是常數,encode 結果可在 codegen 時算好,
    // 但為了與 reflection 版「同一支 UrlEncode」語義一致,這裡同樣走 runtime UrlEncode。
    p.Print("  $dst$ += headergen::UrlEncode(\"$key$\");\n", "dst", dst, "key", f.key);
    p.Print("  $dst$.push_back('=');\n", "dst", dst);
    p.Print("  $dst$ += headergen::UrlEncode($src$.$g$);\n",
            "dst", dst, "src", src, "g", f.getter);
  }
}

class HeaderGenerator : public CodeGenerator {
 public:
  bool Generate(const FileDescriptor* file, const std::string&,
                GeneratorContext* ctx, std::string* error) const override {
    const std::string base =
        file->name().substr(0, file->name().find_last_of('.'));
    const std::string ns = CppNamespace(file);

    // ---- .h ----
    {
      std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".headers.h"));
      io::Printer p(os.get(), '$');
      p.Print(
        "// AUTO-GENERATED by protoc-gen-header. DO NOT EDIT.\n"
        "#pragma once\n"
        "#include <grpcpp/grpcpp.h>\n"
        "#include \"$base$.pb.h\"\n"
        "#include \"url_encode.h\"   // headergen::UrlEncode 共用實作\n\n",
        "base", base);
      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        if (!HasGroup(d, routing::TOOL)) continue;  // 只為頂層 request 生成入口
        p.Print("void ApplyHeaders(const $ns$::$msg$& req, grpc::ClientContext* ctx);\n",
                "ns", ns, "msg", d->name());
      }
    }

    // ---- .cc ----
    {
      std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".headers.cc"));
      io::Printer p(os.get(), '$');
      p.Print(
        "// AUTO-GENERATED by protoc-gen-header. DO NOT EDIT.\n"
        "#include \"$base$.headers.h\"\n"
        "#include <stdexcept>\n\n",
        "base", base);

      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* req = file->message_type(i);

        std::vector<FieldInfo> tool;
        if (!CollectGroup(req, routing::TOOL, &tool, error)) return false;  // 非 scalar 等 → build fail
        if (tool.empty()) continue;  // 沒 TOOL 欄位 → 非頂層 request,略過

        // 找 repeated LOT / MASK 子訊息欄位
        const FieldDescriptor* lot_field = nullptr;
        const FieldDescriptor* mask_field = nullptr;
        for (int j = 0; j < req->field_count(); ++j) {
          const FieldDescriptor* f = req->field(j);
          if (!f->is_repeated() || f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE)
            continue;
          if (HasGroup(f->message_type(), routing::LOT))  lot_field = f;
          if (HasGroup(f->message_type(), routing::MASK)) mask_field = f;
        }

        p.Print("void ApplyHeaders(const $ns$::$msg$& req, grpc::ClientContext* ctx) {\n",
                "ns", ns, "msg", req->name());

        // TOOL (required, 1)
        EmitBuildValue(p, tool, "tool", "req");
        p.Print(
          "  if (tool.empty()) throw std::runtime_error(\"tool-header required\");\n"
          "  ctx->AddMetadata(\"tool-header\", tool);\n");

        // LOT (1..N)
        if (lot_field) {
          std::vector<FieldInfo> lot;
          if (!CollectGroup(lot_field->message_type(), routing::LOT, &lot, error)) return false;
          p.Print("  if (req.$f$_size() == 0) throw std::runtime_error(\"lot-header requires 1..N\");\n",
                  "f", lot_field->name());
          p.Print("  for (const auto& e : req.$f$()) {\n", "f", lot_field->name());
          EmitBuildValue(p, lot, "v", "e");
          p.Print("    ctx->AddMetadata(\"lot-header\", v);\n  }\n");
        }

        // MASK (0..N)
        if (mask_field) {
          std::vector<FieldInfo> mask;
          if (!CollectGroup(mask_field->message_type(), routing::MASK, &mask, error)) return false;
          p.Print("  for (const auto& e : req.$f$()) {\n", "f", mask_field->name());
          EmitBuildValue(p, mask, "v", "e");
          p.Print("    ctx->AddMetadata(\"mask-header\", v);\n  }\n");
        }

        p.Print("}\n\n");
      }
    }
    return true;
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  HeaderGenerator g;
  return PluginMain(argc, argv, &g);
}
