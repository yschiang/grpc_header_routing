# Story 2.1: Real-wire gRPC demo (live client + server carrying projected routing-meta)

Status: ready-for-dev

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As an adopter evaluating the kit,
I want a runnable gRPC client + server that send **real** traffic carrying the projected routing-meta headers and verify them on receipt,
so that I can see — not just take on faith — that body-authoritative projection survives an actual gRPC hop and that the digest gate catches header/body drift on the wire.

This upgrades **HR4** (Story 1.9 / `tests/grpc_smoke.cc`), which is deliberately *compile-only — no live channel/server*, into a live end-to-end demo. It adds no wire bytes (CR1) and keeps the core build gRPC-free (NFR3): the demo is opt-in.

## Acceptance Criteria

1. **Live server verifies on the wire.** A `grpc_server` implements at least `Sys1Service` (recommended: also one `Sys3Service.Submit05` to exercise the missing-required path). On each RPC it reads `grpc::ServerContext::client_metadata()`, reconstructs the ordered `x-process-context` values + reads `x-process-context-digest`, calls `routingmeta::VerifyDigest(contexts, digest)`, prints `accept` / `reject` with the expected/actual digest, and returns `common.v1.Ack`. It must NOT reimplement digest logic — it reuses `process_context_parser.h`.
   - **Given** a well-formed request, **When** the client calls the RPC, **Then** the server logs `digest check: OK` and accepts.
   - **Given** a context tampered after projection (header/body drift), **When** the server verifies, **Then** `VerifyDigest` returns `ok=false` and the server rejects it (logs the mismatch; returns a non-OK `grpc::Status` or an `Ack` flagged rejected — pick one and document it in DEMO.md).

2. **Client projects through the kit's one path.** A `grpc_client` builds requests, attaches headers via `grpc::ClientContext ctx; routingmeta::GrpcSink sink(&ctx); routingmeta::Send(req, rt, sink);` (the existing kit surface — no bespoke metadata writing), then calls the stub over a real `localhost` channel and prints the returned `ProjResult` (`ok` / `issues` / `duration`) and the server's verdict.
   - **Given** three calls — (a) a good sys1 batch, (b) the empty-mask `Sys3Service.Submit05` case, (c) an overflow case (>25 / >7168 B) — **When** the client runs, **Then** (a) verifies OK, (b) surfaces `x-routing-error: missing:x-mask-id` with `ok=false` and is still delivered, (c) surfaces `x-process-context-overflow: true` and the server sees no digest (it logs "no digest provided").

3. **`run.sh` is a one-command, self-checking demo.** From `grpc-routing-meta/example/demo/`, `./run.sh` builds the demo with gRPC enabled (generates the `--grpc_out` service stubs + links `grpc++`), starts the server, waits until it is listening, runs the client, prints the headers traveling and the server verification, then tears the server down. It exits non-zero if the good-case fails to verify or the tamper-case fails to reject (it is a gate, not just a script). If `grpc++`/`grpc_cpp_plugin` are absent it prints a clear install hint and exits non-zero — never a silent or fake pass (mirrors `build.sh`'s protobuf probe).

4. **`DEMO.md` is the walkthrough.** `demo/DEMO.md` states what the demo proves, prerequisites, exactly how to run it, annotated expected output (the key wire headers + the accept and reject lines), and one sentence relating it to HR4 (compile-smoke → live). No `refs/` edits (CR3).

## Tasks / Subtasks

- [ ] **Task 1 — gRPC stub generation (AC: 1, 2, 3)**
  - [ ] In the demo build, run `protoc --grpc_out=<gen> --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin)` for the service protos (`sys1.proto`, and `sys3.proto` if used) → `*.grpc.pb.cc/.h`. The message `*.pb.cc` already come from `build.sh`'s `--cpp_out`; reuse them.
  - [ ] Keep this OUT of `build.sh`/`CMakeLists` core path — gRPC stays optional (NFR3). Put generation in `demo/run.sh` (or a `demo/build-demo.sh` it calls).
- [ ] **Task 2 — server (`demo/grpc_server.cc`) (AC: 1)**
  - [ ] Implement the generated `Sys1Service::Service` (+ `Sys3Service` if used). In each handler: iterate `ctx->client_metadata()` (a `std::multimap<grpc::string_ref, grpc::string_ref>`), collect every `x-process-context` value into a `std::vector<std::string>` (preserve arrival order), read `x-process-context-digest`, then `auto vr = routingmeta::VerifyDigest(contexts, digest);`.
  - [ ] Print `LotID/...` via `routingmeta::ParseContext` for human-readable output (mirror `receiver_verify.cc`), then `accept`/`reject` per `vr.ok`. Return `common::v1::Ack`.
  - [ ] Listen on `127.0.0.1:<port>` with `InsecureServerCredentials()`.
- [ ] **Task 3 — client (`demo/grpc_client.cc`) (AC: 2)**
  - [ ] Build channel `grpc::CreateChannel("127.0.0.1:<port>", grpc::InsecureChannelCredentials())` and the stub(s).
  - [ ] For each demo call: construct request + `routingmeta::Runtime`, `GrpcSink sink(&ctx)`, `auto r = routingmeta::Send(req, rt, sink);`, invoke the RPC, print `r.ok / r.issues / r.duration` + the returned `Ack`.
  - [ ] Tamper case: project into a `VectorSink`, mutate one `x-process-context` value, then manually `ctx.AddMetadata(...)` the mutated set (or add a second client path) so the server sees drift — document how in DEMO.md.
- [ ] **Task 4 — `demo/run.sh` (AC: 3)**
  - [ ] Probe `pkg-config --exists grpc++` and `command -v grpc_cpp_plugin`; clear hint + non-zero exit if missing.
  - [ ] Generate stubs, compile server+client with `-DROUTINGMETA_WITH_GRPC $(pkg-config --cflags --libs grpc++ protobuf)`, reusing `../build/generated/*.pb.cc` (run `../build.sh` first if absent).
  - [ ] Start server backgrounded, poll the port until ready (bounded retries — no fixed sleep), run client, capture output, kill server on exit (`trap`).
  - [ ] Assert good-case `accept` present AND tamper-case `reject` present; exit non-zero otherwise.
- [ ] **Task 5 — `demo/DEMO.md` (AC: 4)** — purpose, prereqs, `./run.sh`, annotated expected output, HR4 relation.
- [ ] **Task 6 — optional CI step** — note (do not require) a real-wire CI step that runs `demo/run.sh` on the cell that already has `libgrpc++-dev`, complementing the HR4 compile-smoke. Flag for the team; keep it a separate opt-in step.

## Dev Notes

### What already exists — REUSE, do not reinvent

- **`routingmeta::GrpcSink`** (`src/common/metadata_sink.h`, under `#ifdef ROUTINGMETA_WITH_GRPC`): wraps `grpc::ClientContext*` and writes each projected header via `ctx_->AddMetadata(key, value)`. This is the client-side metadata writer — **use it; do not call `AddMetadata` by hand** except for the deliberate tamper path. [Source: src/common/metadata_sink.h]
- **`routingmeta::Send`** (`src/common/send.h`): the one branchless sender path = `FillCommon` + generated `ProjectMeta` → `ProjResult`. The client uses this; passing a `GrpcSink` puts the headers on the wire via ADL. [Source: src/common/send.h, epics.md FR6/AR3]
- **`routingmeta::VerifyDigest(contexts, received_digest)` and `ParseContext`** (`src/common/process_context_parser.h`, header-only): the server reuses these verbatim — recompute `sha256:` over `\n`-joined contexts, compare to the received digest. Empty digest → `error="no digest provided (overflow or sender omitted)"`, `ok=false`. [Source: src/common/process_context_parser.h]
- **`receiver_verify.cc`** is the in-process analogue of the new server (parse + digest accept/reject, incl. the tamper regression). Mirror its structure over real metadata. [Source: receiver/receiver_verify.cc, epics.md Story 1.11]
- **Services already declared** — `Sys1Service.Calculate`, `Sys2Service.{Verify,Download,Qualify,Upload,List}`, `Sys3Service.Submit01..10`, all returning `common.v1.Ack`. The build just never ran `--grpc_out`. [Source: proto/sys1.proto, proto/sys2.proto, proto/sys3.proto, proto/process_context.proto]

### Exact wire keys the server reads (lowercase; gRPC lowercases metadata keys)

From `FillCommon` + `EmitProcessContext`:
`x-request-id`, `x-correlation-id`, `x-contract-version`, `x-source-system`, `x-site-id`, `x-tool-id`, `x-process-context-count`, `x-process-context-format` (`urlencoded-query-string-v1`), `x-process-context-digest` (`sha256:…`), `x-process-context` (repeated — one per context, the canonical `Key=UrlEncode(Value)&…` strings), and on the failure paths `x-process-context-overflow: true` / `x-routing-error: missing:x-mask-id`. [Source: src/common/common_headers.h:23-28, src/common/process_context_emit.h:42-71]

The server must collect ALL `x-process-context` values (it is a repeated key → multiple multimap entries) into a vector **in order**, and read the single `x-process-context-digest`. Convert `grpc::string_ref` → `std::string` via `std::string(r.data(), r.size())`.

### File locations (new — isolated demo subdir)

- `grpc-routing-meta/example/demo/grpc_server.cc`
- `grpc-routing-meta/example/demo/grpc_client.cc`
- `grpc-routing-meta/example/demo/run.sh` (executable)
- `grpc-routing-meta/example/demo/DEMO.md`
Generated stubs go under `grpc-routing-meta/example/build/generated/` (gitignored, alongside the existing `*.pb.cc`). Stay inside the workspace (CLAUDE.md). [Source: build.sh layout, grpc-routing-meta/.gitignore]

### Constraints / regressions to NOT break

- **CR1 — no wire change.** The demo only READS existing headers and only WRITES headers the kit already projects. No new header bytes. [Source: epics.md CR1]
- **NFR3 — gRPC stays optional.** Do not make `build.sh`/`CMakeLists.txt` core targets depend on gRPC. The demo build lives in `demo/run.sh`. The `ROUTINGMETA_WITH_GRPC` macro must remain compile-gated. [Source: epics.md NFR3, CMakeLists.txt:78-86]
- **Fail loud, never silent.** Absent `grpc++`/`grpc_cpp_plugin` → clear message + non-zero exit (do not skip-as-pass). The tamper case MUST reject; a demo that "passes" without proving the reject path is a false pass. [Source: BRIEF criterion C, build.sh protobuf probe]
- **CR3 — `refs/` read-only; no push.** [Source: epics.md CR3, CLAUDE.md]

### Build/run specifics (latest-tooling notes)

- gRPC C++ codegen: `protoc -I proto --grpc_out=build/generated --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" proto/sys1.proto`. `grpc_cpp_plugin` ships with `libgrpc++-dev` (already installed in CI per `ci.yml` deps). [Source: .github/workflows/ci.yml:20]
- Link: `$(pkg-config --cflags --libs grpc++ protobuf)` plus the kit includes `-I src -I build/generated`. Define `-DROUTINGMETA_WITH_GRPC` so `GrpcSink` is compiled in. CMake already has an optional `gRPC::grpc++` path (`find_package(gRPC CONFIG QUIET)`) but generates no service stubs — the demo adds the `--grpc_out` step itself. [Source: CMakeLists.txt:78-86]
- Metadata value rule: non-`-bin` keys carry ASCII; the percent-encoded contexts are ASCII-safe, so values pass gRPC's ASCII metadata check. Keys are already lowercase (gRPC requirement). No `-bin` suffix needed.
- Port/readiness: bind `127.0.0.1` on a fixed demo port (e.g. `50551`); `run.sh` polls the port (bounded retries) instead of a fixed `sleep`, then `trap`-kills the server on exit — deterministic, CI-safe.

### Testing standards

`run.sh` is the test oracle (like `build.sh`'s negative gate): it asserts the good-case `accept` and the tamper-case `reject` lines and exits non-zero on regression. No new test framework (NFR3). Optionally register `demo/run.sh` as a CI real-wire step where `libgrpc++-dev` is present (complements, does not replace, HR4 compile-smoke). [Source: epics.md HR4/NFR2, build.sh negative-gate pattern]

### Project Structure Notes

- New `demo/` subdir is isolated from `sender/`, `receiver/`, `tests/`; it depends on the kit headers + generated message code but is built only by its own `run.sh`. This keeps the core build gRPC-free (NFR3) while giving a first-class live demo. No conflict with existing structure; `build/` stays gitignored.

### References

- [Source: _bmad-output/planning-artifacts/epics.md#Epic-1] — HR4 (compile-smoke), FR6 (`Send`), FR5 (receiver digest), CR1/CR3/NFR3
- [Source: grpc-routing-meta/example/src/common/metadata_sink.h] — `GrpcSink`, `MetadataSink`, byte accounting
- [Source: grpc-routing-meta/example/src/common/process_context_parser.h] — `VerifyDigest`, `ParseContext`, `UrlDecode`
- [Source: grpc-routing-meta/example/src/common/process_context_emit.h:42-71] — emitted header keys, overflow flag
- [Source: grpc-routing-meta/example/src/common/common_headers.h:23-28] — common headers
- [Source: grpc-routing-meta/example/tests/grpc_smoke.cc] — HR4 compile-smoke this story upgrades
- [Source: grpc-routing-meta/example/receiver/receiver_verify.cc] — in-process verify to mirror over the wire
- [Source: grpc-routing-meta/example/proto/sys1.proto, sys3.proto] — service/rpc definitions to generate stubs for

## Dev Agent Record

### Agent Model Used

### Debug Log References

### Completion Notes List

### File List
