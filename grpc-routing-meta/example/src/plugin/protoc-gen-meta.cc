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

struct Proj { std::string key{}; bool required = false; std::string getter{}; };

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

// A uniform_across_repeated projection: `rep` is the getter path to the ONE
// repeated message (e.g. "jobs()"), `leaf` the direct scalar inside each element
// (e.g. "mask_id()"). Generated code projects element[0] and verifies all agree.
struct UProj { std::string key{}; bool required = false; std::string rep{}, leaf{}; };

// Collect uniform projections: descend non-repeated messages exactly like walkProj;
// at each repeated message field, collect its DIRECT flagged string scalars
// (CheckRepeatedSubtree has already rejected anything deeper). Cycle-guarded.
void walkUniform(const Descriptor* d, const std::string& prefix, std::vector<UProj>* out,
                 std::set<const Descriptor*>* onpath) {
  if (!onpath->insert(d).second) return;
  for (int i = 0; i < d->field_count(); ++i) {
    const FieldDescriptor* f = d->field(i);
    if (f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) continue;
    if (!f->is_repeated()) {
      walkUniform(f->message_type(), prefix + f->name() + "().", out, onpath);
      continue;
    }
    const Descriptor* m = f->message_type();
    for (int k = 0; k < m->field_count(); ++k) {
      const FieldDescriptor* sf = m->field(k);
      if (!sf->options().HasExtension(routing::project)) continue;
      const auto& pj = sf->options().GetExtension(routing::project);
      if (pj.uniform_across_repeated())
        out->push_back({pj.key(), pj.required(), prefix + f->name() + "()", sf->name() + "()"});
    }
  }
  onpath->erase(d);
}

inline void walkUniform(const Descriptor* d, std::vector<UProj>* out) {
  std::set<const Descriptor*> onpath;
  walkUniform(d, "", out, &onpath);
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

inline bool IsUniform(const FieldDescriptor* f) {
  return f->options().HasExtension(routing::project) &&
         f->options().GetExtension(routing::project).uniform_across_repeated();
}

// Like AnyProject but only counts tags with uniform_across_repeated set.
bool AnyUniform(const Descriptor* d, std::set<const Descriptor*>* onpath) {
  if (!onpath->insert(d).second) return false;
  bool found = false;
  for (int i = 0; i < d->field_count() && !found; ++i) {
    const FieldDescriptor* f = d->field(i);
    found = (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
              ? AnyUniform(f->message_type(), onpath)
              : IsUniform(f);
  }
  onpath->erase(d);
  return found;
}

// Inside ONE repeated message (field `f` of `parent`): a DIRECT string-scalar
// field may carry uniform_across_repeated (verified-uniform projection, see
// walkUniform). Anything else tagged (routing.project) here — unflagged, wrong
// type, or flagged deeper (another message hop / second repeated level, where
// uniformity across a cross-product is undefined) — is rejected loudly.
bool CheckRepeatedSubtree(const Descriptor* m, const Descriptor* parent,
                          const FieldDescriptor* f, std::string* err) {
  for (int k = 0; k < m->field_count(); ++k) {
    const FieldDescriptor* sf = m->field(k);
    if (IsUniform(sf)) {
      if (sf->is_repeated() || sf->cpp_type() != FieldDescriptor::CPPTYPE_STRING) {
        *err = "(routing.project) on field \"" + sf->name() + "\" in message " +
               m->name() + " must be a string scalar";
        return false;
      }
      continue;  // valid uniform projection — collected by walkUniform
    }
    if (sf->options().HasExtension(routing::project)) {
      *err = "(routing.project) is set under repeated field \"" + f->name() +
             "\" in message " + parent->name() +
             " — a repeated value cannot project to a single-valued header"
             " (set uniform_across_repeated if the value is duplicated identically)";
      return false;
    }
    if (sf->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      { std::set<const Descriptor*> seen;
        if (AnyUniform(sf->message_type(), &seen)) {
          *err = "uniform_across_repeated below field \"" + sf->name() +
                 "\" in message " + m->name() +
                 " — the flag must be on a direct field of exactly one repeated message";
          return false;
        } }
      std::set<const Descriptor*> seen;
      if (AnyProject(sf->message_type(), &seen)) {
        *err = "(routing.project) is set under repeated field \"" + f->name() +
               "\" in message " + parent->name() +
               " — a repeated value cannot project to a single-valued header";
        return false;
      }
    }
  }
  return true;
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
      ok = CheckRepeatedSubtree(f->message_type(), d, f, err);
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
    if (IsUniform(f)) {
      // This walk only ever descends NON-repeated paths from the request root, so a
      // flag seen here promises uniformity across a repeated ancestor that doesn't
      // exist — a dead annotation that would silently mean nothing.
      *err = "uniform_across_repeated on field \"" + f->name() + "\" in message " +
             d->name() + " has no repeated ancestor — the flag only applies to a "
             "direct field of a repeated message";
      ok = false;
    } else if (f->options().HasExtension(routing::project) && (f->is_repeated() || is_msg)) {
      *err = "(routing.project) on field \"" + f->name() + "\" in message " + d->name() +
             " must be a non-repeated scalar — a " +
             std::string(f->is_repeated() ? "repeated" : "message") +
             " field cannot project to a single-valued header";
      ok = false;
    } else if (f->options().HasExtension(routing::project) &&
               f->cpp_type() != FieldDescriptor::CPPTYPE_STRING) {
      // Generated code calls .empty() and UrlEncode() on the field, which only
      // compile for std::string — a numeric/bool/enum tag would emit code that
      // fails to build instead of failing loud here.
      *err = "(routing.project) on field \"" + f->name() + "\" in message " + d->name() +
             " must be a string scalar";
      ok = false;
    } else if (is_msg && !f->is_repeated()) {
      ok = ProjectOnlyOnScalarLeaf(f->message_type(), onpath, err);
    }
  }
  onpath->erase(d);
  return ok;
}

// Reject a second repeated process-context-bearing field (FindCtx returns only the
// first match, so a second would silently vanish from the projection — count would
// under-report the body, violating "never silent") and reject (routing.pctx) on a
// non-string field (the generated context loop calls UrlEncode() on it, which only
// compiles for std::string).
bool ValidateContexts(const Descriptor* d, std::string* err) {
  int ctx_fields = 0;
  for (int i = 0; i < d->field_count(); ++i) {
    const FieldDescriptor* f = d->field(i);
    if (!f->is_repeated() || f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) continue;
    bool is_ctx = false;
    for (int k = 0; k < f->message_type()->field_count(); ++k) {
      const FieldDescriptor* sf = f->message_type()->field(k);
      if (!sf->options().HasExtension(routing::pctx)) continue;
      is_ctx = true;
      if (sf->cpp_type() != FieldDescriptor::CPPTYPE_STRING) {
        *err = "(routing.pctx) on field \"" + sf->name() + "\" in message " +
               f->message_type()->name() + " must be a string scalar";
        return false;
      }
    }
    if (is_ctx && ++ctx_fields > 1) {
      *err = "message " + d->name() + " has more than one repeated process-context "
             "field (\"" + f->name() + "\" is the second) — only the first is "
             "projected; the rest would silently vanish";
      return false;
    }
  }
  return true;
}

// Per-message validation, run before any output is written so codegen fails LOUDLY
// (the kit's "never silent" rule) instead of emitting a broken header contract.
bool Validate(const Descriptor* d, std::string* err) {
  { std::set<const Descriptor*> onpath;
    if (!ProjectOnlyOnScalarLeaf(d, &onpath, err)) return false; }
  std::vector<Proj> projs; walkProj(d, "", &projs);
  std::vector<UProj> uprojs; walkUniform(d, &uprojs);
  std::set<std::string> keys;
  for (const auto& pj : projs)
    if (!keys.insert(pj.key).second) {
      *err = "duplicate (routing.project) key \"" + pj.key + "\" in message " +
             d->name() + " — a single-valued header would be emitted twice";
      return false;
    }
  for (const auto& u : uprojs)   // uniform keys share the same single-header namespace
    if (!keys.insert(u.key).second) {
      *err = "duplicate (routing.project) key \"" + u.key + "\" in message " +
             d->name() + " — a single-valued header would be emitted twice";
      return false;
    }
  { std::set<const Descriptor*> onpath;
    if (!NoProjectUnderRepeated(d, &onpath, err)) return false; }
  return ValidateContexts(d, err);
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
        "#include \"common/proj_result.h\"\n\n"
        "namespace routingmeta {\n",
        "b", base);
      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        std::vector<Proj> projs; walkProj(d, "", &projs);
        std::vector<UProj> uprojs; walkUniform(d, &uprojs);
        if (projs.empty() && uprojs.empty() && !FindCtx(d)) continue;
        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink, bool emit_digest = true);\n",
                "ns", ns, "m", d->name());
      }
      p.Print("}  // namespace routingmeta\n");
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
        "#include <chrono>\n#include <vector>\n#include <string>\n\n"
        "namespace routingmeta {\n\n",
        "b", base);

      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        std::vector<Proj> projs; walkProj(d, "", &projs);
        std::vector<UProj> uprojs; walkUniform(d, &uprojs);
        const FieldDescriptor* ctxf = FindCtx(d);
        if (projs.empty() && uprojs.empty() && !ctxf) continue;

        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink, bool emit_digest) {\n"
                "  ProjResult _r;\n"
                "  const auto _t0 = std::chrono::steady_clock::now();\n",
                "ns", ns, "m", d->name());

        for (const auto& pj : projs) {
          std::string v = "req." + pj.getter;
          if (pj.required) {
            p.Print(
              "  if ($v$.empty()) {\n"
              "    _r.ok = false;\n"
              "    _r.issues.push_back({Issue::MissingRequired, \"$k$\"});\n"
              "    sink.Add(\"x-routing-error\", \"missing:$k$\");\n"
              "  } else {\n"
              "    sink.Add(\"$k$\", UrlEncode($v$));\n"
              "  }\n",
              "v", v, "k", pj.key);
          } else {
            p.Print("  if (!$v$.empty()) sink.Add(\"$k$\", UrlEncode($v$));\n",
                    "v", v, "k", pj.key);
          }
        }

        for (const auto& u : uprojs) {
          // Project element[0]; verify every element agrees (the annotation is the
          // sender's promise of a body invariant the schema can't express). Divergence
          // is a BLOCKING Inconsistent issue — routing a batch on job[0]'s value while
          // the rest disagree would misroute them, so no header is emitted.
          p.Print("  {\n"
                  "    bool _first = true, _consistent = true;\n"
                  "    std::string _val;\n"
                  "    for (const auto& e : req.$rep$) {\n"
                  "      if (_first) { _val = e.$leaf$; _first = false; }\n"
                  "      else if (e.$leaf$ != _val) _consistent = false;\n"
                  "    }\n"
                  "    if (!_consistent) {\n"
                  "      _r.ok = false;\n"
                  "      _r.issues.push_back({Issue::Inconsistent, \"$k$\"});\n"
                  "      sink.Add(\"x-routing-error\", \"inconsistent:$k$\");\n",
                  "rep", u.rep, "leaf", u.leaf, "k", u.key);
          if (u.required) {
            p.Print("    } else if (_val.empty()) {\n"
                    "      _r.ok = false;\n"
                    "      _r.issues.push_back({Issue::MissingRequired, \"$k$\"});\n"
                    "      sink.Add(\"x-routing-error\", \"missing:$k$\");\n"
                    "    } else {\n"
                    "      sink.Add(\"$k$\", UrlEncode(_val));\n"
                    "    }\n  }\n",
                    "k", u.key);
          } else {
            p.Print("    } else if (!_val.empty()) {\n"
                    "      sink.Add(\"$k$\", UrlEncode(_val));\n"
                    "    }\n  }\n",
                    "k", u.key);
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
            const std::string sep = (j == 0) ? "" : "&";
            p.Print("      s += \"$sep$$k$=\"; s += UrlEncode(e.$g$);\n",
                    "sep", sep, "k", cf[j].first, "g", cf[j].second);
          }
          p.Print("      ctxs.push_back(std::move(s));\n    }\n");
          p.Print("    if (EmitProcessContexts(sink, ctxs, emit_digest)) _r.issues.push_back({Issue::Overflow, \"\"});\n  }\n");
        }
        // duration_cast, not implicit conversion: steady_clock::duration -> nanoseconds
        // is only implicitly convertible where it's lossless; cast makes it portable.
        p.Print("  _r.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(\n"
                "      std::chrono::steady_clock::now() - _t0);\n  return _r;\n}\n\n");
      }
      p.Print("}  // namespace routingmeta\n");
    }
    return true;
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  ProjGen g;
  return PluginMain(argc, argv, &g);
}
