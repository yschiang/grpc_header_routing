#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <string>
#include <vector>
#include <algorithm>
#include "ctx_options.pb.h"
using namespace google::protobuf;
using namespace google::protobuf::compiler;
namespace {
std::string ns_of(const FileDescriptor* f){ std::string o; for(char c:f->package()) o+= (c=='.')?std::string("::"):std::string(1,c); return o; }

class CtxGen : public CodeGenerator {
 public:
  bool Generate(const FileDescriptor* file, const std::string&, GeneratorContext* ctx, std::string* err) const override {
    const std::string base = file->name().substr(0, file->name().find_last_of('.'));
    const std::string ns = ns_of(file);
    std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".ctx.cc"));
    io::Printer p(os.get(), '$');
    p.Print("// AUTO-GENERATED. DO NOT EDIT.\n#include \"$b$.pb.h\"\n#include \"url_encode.h\"\n"
            "#include \"sha256.h\"\n#include <grpcpp/grpcpp.h>\n#include <stdexcept>\n#include <vector>\n#include <algorithm>\n\n","b",base);
    for(int i=0;i<file->message_type_count();++i){
      const Descriptor* d = file->message_type(i);
      // header fields on the top-level message
      std::vector<const FieldDescriptor*> hdrs;
      const FieldDescriptor* ctx_repeated = nullptr;  // repeated message field whose subfields carry ctx option
      for(int j=0;j<d->field_count();++j){
        const FieldDescriptor* f = d->field(j);
        if(f->options().HasExtension(routing::header)) hdrs.push_back(f);
        if(f->is_repeated() && f->cpp_type()==FieldDescriptor::CPPTYPE_MESSAGE){
          for(int k=0;k<f->message_type()->field_count();++k)
            if(f->message_type()->field(k)->options().HasExtension(routing::ctx)){ ctx_repeated=f; break; }
        }
      }
      if(hdrs.empty() && !ctx_repeated) continue;

      p.Print("void ApplyMeta(const $ns$::$m$& req, grpc::ClientContext* c){\n","ns",ns,"m",d->name());
      // Layer 1+2 single-value headers
      for(auto f: hdrs){
        if(f->is_repeated()||f->cpp_type()==FieldDescriptor::CPPTYPE_MESSAGE){
          *err="header option must be scalar single-value: "+f->full_name(); return false;
        }
        const auto& h=f->options().GetExtension(routing::header);
        std::string g=f->name()+"()";
        if(h.required()) p.Print("  if(req.$g$.empty()) throw std::runtime_error(\"$k$ required\");\n","g",g,"k",h.key());
        p.Print("  if(!req.$g$.empty()) c->AddMetadata(\"$k$\", headergen::UrlEncode(req.$g$));\n","g",g,"k",h.key());
      }
      // Layer 3 repeated process-context
      if(ctx_repeated){
        const Descriptor* cm = ctx_repeated->message_type();
        // collect ctx fields, sorted by key for canonical digest
        std::vector<std::pair<std::string,std::string>> cf; // key, getter
        for(int k=0;k<cm->field_count();++k){
          const FieldDescriptor* sf=cm->field(k);
          if(!sf->options().HasExtension(routing::ctx)) continue;
          if(sf->is_repeated()||sf->cpp_type()==FieldDescriptor::CPPTYPE_MESSAGE){
            *err="ctx field must be scalar: "+sf->full_name(); return false;
          }
          cf.push_back({sf->options().GetExtension(routing::ctx).key(), sf->name()+"()"});
        }
        std::sort(cf.begin(),cf.end()); // canonical: key-sorted (order-independent digest)
        std::string rep = ctx_repeated->name();
        p.Print("  {\n    std::vector<std::string> ctxs;\n");
        p.Print("    for(const auto& e : req.$r$()){\n","r",rep);
        p.Print("      std::string s;\n");
        bool first=true;
        for(auto& kv: cf){
          p.Print("      if(!s.empty()) s.push_back('&');\n");
          p.Print("      s += \"$k$=\"; s += headergen::UrlEncode(e.$g$);\n","k",kv.first,"g",kv.second);
          (void)first;
        }
        p.Print("      ctxs.push_back(std::move(s));\n    }\n");
        // overflow policy (Appendix C: v1 max 25)
        p.Print("    c->AddMetadata(\"x-process-context-count\", std::to_string(ctxs.size()));\n");
        p.Print("    c->AddMetadata(\"x-process-context-format\", \"urlencoded-query-string-v1\");\n");
        p.Print("    if(ctxs.size() > 25){\n");
        p.Print("      c->AddMetadata(\"x-process-context-overflow\", \"true\");\n");
        p.Print("    } else {\n");
        // digest over canonical (key-sorted already; join by newline per Appendix B)
        p.Print("      std::string canon; for(size_t i=0;i<ctxs.size();++i){ if(i) canon.push_back('\\n'); canon+=ctxs[i]; }\n");
        p.Print("      c->AddMetadata(\"x-process-context-digest\", \"sha256:\"+headergen::Sha256Hex(canon));\n");
        p.Print("      for(const auto& s: ctxs) c->AddMetadata(\"x-process-context\", s);\n");
        p.Print("    }\n  }\n");
      }
      p.Print("}\n\n");
    }
    return true;
  }
};
}
int main(int a,char**v){ CtxGen g; return PluginMain(a,v,&g); }
