// =============================================================================
// protoc-gen-meta — body-projection code generator
//
// Generates ProjectMeta(req, sink) which writes ONLY body-derived metadata:
//   - (routing.project) scalars, walked to their (possibly nested) field path
//                       -> single-valued headers (e.g. x-mask-id)
//   - repeated (routing.pctx) process-context fields -> x-process-context
//                       (+ count, format, sha256 digest, overflow policy)
//
// It does NOT generate the sender-known common headers (e.g. x-tool-id); the
// sender fills those directly. Generated code writes into a
// routingmeta::MetadataSink, so it has no hard gRPC dependency.
// =============================================================================

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/zero_copy_stream.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "metadata_options.pb.h"

using namespace google::protobuf;
using namespace google::protobuf::compiler;

namespace {

std::string ns_of(const FileDescriptor* f) {
  std::string o;
  for (char c : f->package()) o += (c == '.') ? std::string("::") : std::string(1, c);
  return o;
}

struct Proj { std::string key; bool required; std::string getter; };

// Recurse non-repeated message fields collecting (routing.project) scalars and the
// getter path to each (e.g. "job().mask().mask_id()"). `onpath` is the set of
// descriptors on the CURRENT path; erase-on-exit guards against infinite recursion
// on recursive message types while still allowing the same type at sibling (diamond)
// paths.
void walkProj(const Descriptor* d, const std::string& prefix, std::vector<Proj>* out,
              std::set<const Descriptor*>* onpath) {
  if (!onpath->insert(d).second) return;   // already on this path -> cycle, stop
  for (int i = 0; i < d->field_count(); ++i) {
    const FieldDescriptor* f = d->field(i);
    if (f->is_repeated()) continue;
    if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      walkProj(f->message_type(), prefix + f->name() + "().", out, onpath);
    } else if (f->options().HasExtension(routing::project)) {
      const auto& pj = f->options().GetExtension(routing::project);
      out->push_back({pj.key(), pj.required(), prefix + f->name() + "()"});
    }
  }
  onpath->erase(d);
}

inline void walkProj(const Descriptor* d, const std::string& prefix, std::vector<Proj>* out) {
  std::set<const Descriptor*> onpath;
  walkProj(d, prefix, out, &onpath);
}

const FieldDescriptor* FindCtx(const Descriptor* d) {
  for (int j = 0; j < d->field_count(); ++j) {
    const FieldDescriptor* f = d->field(j);
    if (f->is_repeated() && f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
      for (int k = 0; k < f->message_type()->field_count(); ++k)
        if (f->message_type()->field(k)->options().HasExtension(routing::pctx))
          return f;
  }
  return nullptr;
}

// True if any field of `d`, recursing through BOTH repeated and non-repeated
// messages, carries (routing.project). Cycle-guarded.
bool AnyProject(const Descriptor* d, std::set<const Descriptor*>* onpath) {
  if (!onpath->insert(d).second) return false;
  bool found = false;
  for (int i = 0; i < d->field_count() && !found; ++i) {
    const FieldDescriptor* f = d->field(i);
    found = (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
              ? AnyProject(f->message_type(), onpath)
              : f->options().HasExtension(routing::project);
  }
  onpath->erase(d);
  return found;
}

// Reject (routing.project) reachable under a repeated field: a single-valued header
// cannot represent N values, and walkProj silently skips repeated subtrees — so
// without this check the tag would vanish with no diagnostic. Cycle-guarded.
bool NoProjectUnderRepeated(const Descriptor* d, std::set<const Descriptor*>* onpath,
                            std::string* err) {
  if (!onpath->insert(d).second) return true;
  bool ok = true;
  for (int i = 0; i < d->field_count() && ok; ++i) {
    const FieldDescriptor* f = d->field(i);
    if (f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) continue;
    if (f->is_repeated()) {
      std::set<const Descriptor*> seen;
      if (AnyProject(f->message_type(), &seen)) {
        *err = "(routing.project) is set under repeated field \"" + f->name() +
               "\" in message " + d->name() +
               " — a repeated value cannot project to a single-valued header";
        ok = false;
      }
    } else {
      ok = NoProjectUnderRepeated(f->message_type(), onpath, err);
    }
  }
  onpath->erase(d);
  return ok;
}

// (routing.project) is collected only off a NON-repeated SCALAR leaf (see walkProj).
// A tag sitting on a repeated field, or on a message-typed field, is something
// walkProj would silently skip — so reject it loudly here. Descends non-repeated
// messages exactly like walkProj; repeated subtrees are NoProjectUnderRepeated's job.
bool ProjectOnlyOnScalarLeaf(const Descriptor* d, std::set<const Descriptor*>* onpath,
                             std::string* err) {
  if (!onpath->insert(d).second) return true;
  bool ok = true;
  for (int i = 0; i < d->field_count() && ok; ++i) {
    const FieldDescriptor* f = d->field(i);
    const bool is_msg = f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE;
    if (f->options().HasExtension(routing::project) && (f->is_repeated() || is_msg)) {
      *err = "(routing.project) on field \"" + f->name() + "\" in message " + d->name() +
             " must be a non-repeated scalar — a " +
             std::string(f->is_repeated() ? "repeated" : "message") +
             " field cannot project to a single-valued header";
      ok = false;
    } else if (is_msg && !f->is_repeated()) {
      ok = ProjectOnlyOnScalarLeaf(f->message_type(), onpath, err);
    }
  }
  onpath->erase(d);
  return ok;
}

// Per-message validation, run before any output is written so codegen fails LOUDLY
// (the kit's "never silent" rule) instead of emitting a broken header contract.
bool Validate(const Descriptor* d, std::string* err) {
  { std::set<const Descriptor*> onpath;
    if (!ProjectOnlyOnScalarLeaf(d, &onpath, err)) return false; }
  std::vector<Proj> projs; walkProj(d, "", &projs);
  std::set<std::string> keys;
  for (const auto& pj : projs)
    if (!keys.insert(pj.key).second) {
      *err = "duplicate (routing.project) key \"" + pj.key + "\" in message " +
             d->name() + " — a single-valued header would be emitted twice";
      return false;
    }
  std::set<const Descriptor*> onpath;
  return NoProjectUnderRepeated(d, &onpath, err);
}

class ProjGen : public CodeGenerator {
 public:
  bool Generate(const FileDescriptor* file, const std::string&,
                GeneratorContext* ctx, std::string* err) const override {
    const std::string base = file->name().substr(0, file->name().find_last_of('.'));
    const std::string ns = ns_of(file);

    // Fail loud on contract violations before emitting anything.
    for (int i = 0; i < file->message_type_count(); ++i)
      if (!Validate(file->message_type(i), err)) return false;

    {
      std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".proj.h"));
      io::Printer p(os.get(), '$');
      p.Print(
        "// AUTO-GENERATED by protoc-gen-meta. DO NOT EDIT.\n"
        "#pragma once\n"
        "#include \"$b$.pb.h\"\n"
        "#include \"common/metadata_sink.h\"\n"
        "#include \"common/proj_result.h\"\n\n",
        "b", base);
      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        std::vector<Proj> projs; walkProj(d, "", &projs);
        if (projs.empty() && !FindCtx(d)) continue;
        p.Print("routingmeta::ProjResult ProjectMeta(const $ns$::$m$& req, routingmeta::MetadataSink& sink);\n",
                "ns", ns, "m", d->name());
      }
    }

    {
      std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".proj.cc"));
      io::Printer p(os.get(), '$');
      p.Print(
        "// AUTO-GENERATED by protoc-gen-meta. DO NOT EDIT.\n"
        "// Writes ONLY body-derived metadata; sender fills the common headers.\n"
        "#include \"$b$.proj.h\"\n"
        "#include \"common/url_encode.h\"\n"
        "#include \"common/process_context_emit.h\"\n"
        "#include <vector>\n#include <string>\n\n",
        "b", base);

      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        std::vector<Proj> projs; walkProj(d, "", &projs);
        const FieldDescriptor* ctxf = FindCtx(d);
        if (projs.empty() && !ctxf) continue;

        p.Print("routingmeta::ProjResult ProjectMeta(const $ns$::$m$& req, routingmeta::MetadataSink& sink) {\n"
                "  routingmeta::ProjResult result;\n",
                "ns", ns, "m", d->name());

        for (const auto& pj : projs) {
          std::string v = "req." + pj.getter;
          if (pj.required) {
            // required: missing -> failure-as-data (Issue + x-routing-error), no throw,
            // and the empty scalar header is NOT emitted (FR1/FR2, AD-5).
            p.Print("  if ($v$.empty()) {\n"
                    "    result.ok = false;\n"
                    "    result.issues.push_back({routingmeta::Issue::MissingRequired, \"$k$\"});\n"
                    "    sink.Add(\"x-routing-error\", \"missing:$k$\");\n"
                    "  } else {\n"
                    "    sink.Add(\"$k$\", routingmeta::UrlEncode($v$));\n"
                    "  }\n",
                    "v", v, "k", pj.key);
          } else {
            // optional: empty body field -> omit the header entirely.
            p.Print("  if (!$v$.empty()) sink.Add(\"$k$\", routingmeta::UrlEncode($v$));\n",
                    "v", v, "k", pj.key);
          }
        }

        if (ctxf) {
          std::vector<std::pair<std::string, std::string>> cf;
          const Descriptor* cm = ctxf->message_type();
          for (int k = 0; k < cm->field_count(); ++k) {
            const FieldDescriptor* sf = cm->field(k);
            if (!sf->options().HasExtension(routing::pctx)) continue;
            cf.push_back({sf->options().GetExtension(routing::pctx).key(), sf->name() + "()"});
          }
          std::sort(cf.begin(), cf.end());
          p.Print("  {\n    std::vector<std::string> ctxs;\n    for (const auto& e : req.$r$()) {\n      std::string s;\n",
                  "r", ctxf->name());
          for (size_t j = 0; j < cf.size(); ++j) {
            // '&' folded into the literal for all but the first field — no runtime check.
            const std::string sep = (j == 0) ? "" : "&";
            p.Print("      s += \"$sep$$k$=\"; s += routingmeta::UrlEncode(e.$g$);\n",
                    "sep", sep, "k", cf[j].first, "g", cf[j].second);
          }
          p.Print("      ctxs.push_back(std::move(s));\n    }\n");
          // count / format / digest / overflow + the 7KB total-size guard all live
          // in the shared helper, so the policy is in one place (not baked into each
          // generated file).
          p.Print("    routingmeta::EmitProcessContexts(sink, ctxs);\n  }\n");
        }
        p.Print("  return result;\n}\n\n");
      }
    }
    return true;
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  ProjGen g;
  return PluginMain(argc, argv, &g);
}
