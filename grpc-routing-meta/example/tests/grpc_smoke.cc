// =============================================================================
// grpc_smoke — compile-only HR4 smoke of the ROUTINGMETA_WITH_GRPC GrpcSink +
// ADL path. Proves the gRPC adapter (metadata_sink.h's GrpcSink, which pulls
// <grpcpp/grpcpp.h>) compiles AND that the UNQUALIFIED ProjectMeta(req, sink)
// resolves via argument-dependent lookup on the routingmeta::GrpcSink arg —
// the same call shape production code writes. Compiled with -fsyntax-only and
// NOT linked: the if(false) block never runs, so no live channel/server (HR4).
// Flag OFF -> the body vanishes and main() is a trivial harmless TU.
// =============================================================================
#include "common/metadata_sink.h"  // routingmeta::GrpcSink under #ifdef ROUTINGMETA_WITH_GRPC (pulls <grpcpp/grpcpp.h>)
#include "sys1.proj.h"  // routingmeta::ProjectMeta(const sys1::v1::CalculateRequest&, MetadataSink&)

int main() {
#ifdef ROUTINGMETA_WITH_GRPC
  if (false) {  // compile-only: never executes; no live channel/server (HR4)
    grpc::ClientContext ctx;
    routingmeta::GrpcSink sink(&ctx);
    sys1::v1::CalculateRequest req;
    ProjectMeta(
        req,
        sink);  // UNQUALIFIED -> ADL must resolve routingmeta::ProjectMeta via the GrpcSink arg
  }
#endif
  return 0;  // flag OFF -> trivial harmless TU
}
