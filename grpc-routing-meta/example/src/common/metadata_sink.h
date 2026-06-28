// MetadataSink — minimal abstraction the generated ProjectMeta() writes into.
//
// Decouples the kit from a hard gRPC dependency (tests use VectorSink; production
// uses GrpcSink wrapping a grpc::ClientContext). It also tracks the running total
// metadata size using gRPC's own accounting, so the size guard in
// process_context_emit.h can keep the WHOLE projection under the gRPC limit.
//
// Re-entrancy invariant (AD-12/NFR5): the sink is per-call, caller-owned state and
// ProjectMeta holds no shared mutable state, so concurrent calls on DISTINCT sinks
// are safe without locks. Proven by tests/test_concurrency.cc under ThreadSanitizer.
#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace routingmeta {

// gRPC/HPACK per-entry overhead (RFC 7541 §4.1) — the ONE definition. This is the
// leaf header, so the policy header (process_context_emit.h, which #includes this)
// references it from here; defining it there instead would need a circular include.
constexpr std::size_t kHpackEntryOverhead = 32;

class MetadataSink {
 public:
  virtual ~MetadataSink() = default;

  // Non-virtual: every Add updates the running byte total, then delegates to Write.
  // gRPC bounds metadata by sum(name.size + value.size + overhead) — RFC 7541 §4.1.
  // Using the same formula here means our budget check matches what gRPC enforces.
  void Add(const std::string& key, const std::string& value) {
    bytes_ += key.size() + value.size() + kHpackEntryOverhead;
    Write(key, value);
  }
  std::size_t bytes() const { return bytes_; }

 protected:
  virtual void Write(const std::string& key, const std::string& value) = 0;

 private:
  std::size_t bytes_ = 0;
};

// Test/inspection sink: records all key/value pairs in order.
class VectorSink : public MetadataSink {
 public:
  std::string Get(const std::string& key) const {
    for (const auto& kv : items) if (kv.first == key) return kv.second;
    return {};
  }
  int Count(const std::string& key) const {
    int n = 0; for (const auto& kv : items) if (kv.first == key) ++n; return n;
  }
  std::vector<std::pair<std::string, std::string>> items;

 protected:
  void Write(const std::string& key, const std::string& value) override {
    items.emplace_back(key, value);
  }
};

// Adapter for grpc::ClientContext, compiled only when gRPC is available.
// Usage:  GrpcSink sink(&ctx); ProjectMeta(req, sink);
#ifdef ROUTINGMETA_WITH_GRPC
}  // namespace routingmeta
#include <grpcpp/grpcpp.h>
namespace routingmeta {
class GrpcSink : public MetadataSink {
 public:
  explicit GrpcSink(grpc::ClientContext* ctx) : ctx_(ctx) {}
 protected:
  void Write(const std::string& key, const std::string& value) override {
    ctx_->AddMetadata(key, value);
  }
 private:
  grpc::ClientContext* ctx_;
};
#endif

}  // namespace routingmeta
