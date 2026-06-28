# Real-wire gRPC demo

A runnable client + server that send **real gRPC traffic** carrying the projected
routing-meta headers, and verify them on receipt. This is the live counterpart to
`tests/grpc_smoke.cc` (HR4), which only *compiles* the `GrpcSink` path — here the
headers actually travel over a localhost channel and a server re-runs the kit's
digest gate on what arrives.

## What it proves

1. **Body-authoritative projection survives a real hop.** The client attaches headers
   with the kit's normal two lines — `GrpcSink sink(&ctx); Send(req, rt, sink);` — and
   the server reads them back off `grpc::ServerContext::client_metadata()`.
2. **The digest gate catches header↔body drift on the wire.** A deliberately tampered
   context (one value mutated after projection, digest header left intact) is **rejected**
   by the server (`DATA_LOSS`) — exactly the `receiver_verify.cc` check, now over gRPC.
3. **Failure-as-data is observable on the wire.** Missing-required surfaces
   `x-routing-error` (still delivered); overflow surfaces `x-process-context-overflow`
   with no digest — both non-blocking, the server logs and accepts (it reports; the
   receiver decides — SPEC §7 / NFR7).
4. **No new wire contract.** The demo only reads/writes headers the kit already projects
   (CR1), and the core build stays gRPC-free — this is opt-in (NFR3).

## Prerequisites

- `grpc++` and `grpc_cpp_plugin` (ship with `libgrpc++-dev`, or a conda `grpcpp` install).
- The kit's own toolchain (protobuf + a C++17 compiler), same as `build.sh`.

`run.sh` discovers the toolchain via env + `pkg-config` (no hardcoded paths). If gRPC is
absent it prints an install hint and exits non-zero — it never fakes a pass.

## Run

```bash
# stock Linux (grpc on the default path):
cd grpc-routing-meta/example && ./demo/run.sh

# conda dev host (point at the conda prefix, like build.sh):
PROTOC=$CONDA/bin/protoc PKG_CONFIG_PATH=$CONDA/lib/pkgconfig CXX=clang++ \
  ./demo/run.sh
```

`run.sh` is also a **gate**: it asserts the good case is ACCEPTed *and* the tampered case
is REJECTed, exiting non-zero on regression.

## Expected output (annotated)

```
== grpc++ 1.46.1 | libprotoc 3.20.3 ==
[gen ] grpc stubs: sys1, sys3            # protoc --grpc_out (services already in the protos)
[cc  ] grpc_server
[cc  ] grpc_client
------------------ server ------------------
[server] LISTENING on 127.0.0.1:50551
[server] sys1.Calculate corr=CORR-LOT01-001 count=2 contexts=2
           LotID=LOT01 ChamberId=CH-A RecipeID=RCP_ETCH_V3     # contexts reconstructed off the wire
           LotID=LOT02 ChamberId=CH-B RecipeID=RCP_ETCH_V3
[server] sys1.Calculate digest check: OK -> ACCEPT             # (1) good case verifies
[server] sys1.Calculate corr=CORR-LOT01-001 count=60 contexts=0
[server] sys1.Calculate x-process-context-overflow: true; no digest -> ACCEPT (non-blocking)  # (3) overflow
[server] sys3.Submit05  corr=CORR-sys3-005 count=0 contexts=0
[server] sys3.Submit05  x-routing-error: missing:x-mask-id -> ACCEPT (surfaced, non-blocking) # (3) missing req
[server] sys3.Submit05  no digest provided -> ACCEPT (count=0)
[server] sys1.Calculate digest MISMATCH (...projection drift) -> REJECT                       # (2) tamper rejected
------------------ client ------------------
[client] sys1 good (2 ctx)              ok=true  issues=0 duration=...ns  rpc=OK
[client] sys1 overflow (60 ctx)         ok=true  issues=1 duration=...ns  rpc=OK        # Issue{Overflow}, ok stays true
[client] sys3 empty-mask (missing req)  ok=false issues=1 duration=...ns  rpc=OK        # ok=false, still delivered
[client] sys1 TAMPERED (expect reject)  ok=true  issues=0 ... rpc=REJECTED: digest mismatch: header/body projection drift
DEMO PASSED — real-wire projection verified; header<->body drift rejected
```

## The four cases

| # | Client call | On the wire | Server verdict |
|---|-------------|-------------|----------------|
| 1 | `Send` a sys1 batch (2 ctx) | full headers + digest | `digest check: OK -> ACCEPT` |
| 2 | sys1 batch, **one context tampered post-projection** | digest unchanged, body drifted | `digest MISMATCH -> REJECT` (`DATA_LOSS`) |
| 3a | sys1 overflow (60 ctx) | `x-process-context-overflow: true`, no digest | `ACCEPT (non-blocking)` |
| 3b | sys3 `Submit05` empty mask | `x-routing-error: missing:x-mask-id`, no digest | `ACCEPT (surfaced)` |

Cases 1–3 use the real adoption path (`GrpcSink` + `Send`). Case 2 hand-writes the
metadata only to *corrupt* it — the one place the demo bypasses `Send`, on purpose.

## Relation to HR4

`tests/grpc_smoke.cc` proves the `ROUTINGMETA_WITH_GRPC` adapter **compiles** and that
`ProjectMeta` resolves via ADL on `GrpcSink` (compile-only, no channel). This demo takes
that same path **live**: real channel, real metadata, real digest verification.

## Files

- `grpc_server.cc` — implements `Sys1Service.Calculate` + `Sys3Service.Submit05`; reuses
  `process_context_parser.h::VerifyDigest`.
- `grpc_client.cc` — the four calls above via `GrpcSink` + `Send` (case 2 tampers).
- `run.sh` — builds (gRPC on), runs, and self-checks; exits non-zero on regression.
