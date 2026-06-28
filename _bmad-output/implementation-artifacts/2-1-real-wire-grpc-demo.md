---
baseline_commit: 993ab9e03745ad15be32682f0bfcf6819febc5b6
---

# Story 2.1: Real-wire gRPC demo (live client + server carrying projected routing-meta)

Status: done

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

- [x] **Task 1 — gRPC stub generation (AC: 1, 2, 3)**
  - [x] In the demo build, run `protoc --grpc_out=<gen> --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin)` for the service protos (`sys1.proto`, and `sys3.proto` if used) → `*.grpc.pb.cc/.h`. The message `*.pb.cc` already come from `build.sh`'s `--cpp_out`; reuse them.
  - [x] Keep this OUT of `build.sh`/`CMakeLists` core path — gRPC stays optional (NFR3). Put generation in `demo/run.sh` (or a `demo/build-demo.sh` it calls).
- [x] **Task 2 — server (`demo/grpc_server.cc`) (AC: 1)**
  - [x] Implement the generated `Sys1Service::Service` (+ `Sys3Service` if used). In each handler: iterate `ctx->client_metadata()` (a `std::multimap<grpc::string_ref, grpc::string_ref>`), collect every `x-process-context` value into a `std::vector<std::string>` (preserve arrival order), read `x-process-context-digest`, then `auto vr = routingmeta::VerifyDigest(contexts, digest);`.
  - [x] Print `LotID/...` via `routingmeta::ParseContext` for human-readable output (mirror `receiver_verify.cc`), then `accept`/`reject` per `vr.ok`. Return `common::v1::Ack`.
  - [x] Listen on `127.0.0.1:<port>` with `InsecureServerCredentials()`.
- [x] **Task 3 — client (`demo/grpc_client.cc`) (AC: 2)**
  - [x] Build channel `grpc::CreateChannel("127.0.0.1:<port>", grpc::InsecureChannelCredentials())` and the stub(s).
  - [x] For each demo call: construct request + `routingmeta::Runtime`, `GrpcSink sink(&ctx)`, `auto r = routingmeta::Send(req, rt, sink);`, invoke the RPC, print `r.ok / r.issues / r.duration` + the returned `Ack`.
  - [x] Tamper case: project into a `VectorSink`, mutate one `x-process-context` value, then manually `ctx.AddMetadata(...)` the mutated set (or add a second client path) so the server sees drift — document how in DEMO.md.
- [x] **Task 4 — `demo/run.sh` (AC: 3)**
  - [x] Probe `pkg-config --exists grpc++` and `command -v grpc_cpp_plugin`; clear hint + non-zero exit if missing.
  - [x] Generate stubs, compile server+client with `-DROUTINGMETA_WITH_GRPC $(pkg-config --cflags --libs grpc++ protobuf)`, reusing `../build/generated/*.pb.cc` (run `../build.sh` first if absent).
  - [x] Start server backgrounded, poll the port until ready (bounded retries — no fixed sleep), run client, capture output, kill server on exit (`trap`).
  - [x] Assert good-case `accept` present AND tamper-case `reject` present; exit non-zero otherwise.
- [x] **Task 5 — `demo/DEMO.md` (AC: 4)** — purpose, prereqs, `./run.sh`, annotated expected output, HR4 relation.
- [x] **Task 6 — optional CI step** — note (do not require) a real-wire CI step that runs `demo/run.sh` on the cell that already has `libgrpc++-dev`, complementing the HR4 compile-smoke. Flag for the team; keep it a separate opt-in step. **(Flagged, intentionally NOT wired — task said do-not-require; see Completion Notes for the recommendation.)**

### Review Findings

_Code review 2026-06-29 (8f0350c vs 993ab9e). 3 layers (Blind Hunter, Edge Case Hunter, Acceptance Auditor). 8 patch, 1 defer, 1 dismissed._

- [x] [Review][Patch] **run.sh stale-binary fake-pass** — `set -uo pipefail` lacks `-e`; `./build.sh`, `protoc`, and the `$CXX` compile loop have no `|| exit`. A compile failure leaves last run's binaries in `build/` and the oracle still greps `DEMO PASSED`, defeating the fail-loud self-check (AC3 / criterion C). **[blind+edge+auditor, HIGH]** `demo/run.sh` build steps
- [x] [Review][Patch] **No RPC deadline / no script timeout → indefinite hang** — every `grpc::ClientContext` lacks `set_deadline`; `run.sh` has no `timeout`. A server that accepts TCP but wedges hangs CI instead of failing. **[blind+edge, MED]** `demo/grpc_client.cc` (all ctx), `demo/run.sh` client call
- [x] [Review][Patch] **Readiness poll accepts any listener / bind-failure undetected** — if the port is already held, our server exits but `/dev/tcp` succeeds against the foreign listener; no `kill -0 $SRV_PID` / post-loop success check (fold in: grep server log for `LISTENING` as the readiness signal, covers `/dev/tcp`-unavailable). **[edge, MED]** `demo/run.sh` poll loop
- [x] [Review][Patch] **Server discards expected/actual digest** — AC1 says print accept/reject "with the expected/actual digest"; `VerifyResult.expected_digest`/`actual_digest` are computed then dropped (receiver_verify prints both). **[auditor, MED]** `demo/grpc_server.cc:VerifyFromMetadata`
- [x] [Review][Patch] **"header↔body drift" wording overstated** — the gate verifies projected-context-headers vs the digest header (integrity), not a body-vs-header re-derivation; align DEMO.md + server comment to the kit's integrity-not-security framing (story 1.15). **[blind, LOW]** `demo/DEMO.md`, `demo/grpc_server.cc`
- [x] [Review][Patch] **`Ref()` may build `std::string(nullptr, 0)`** — empty metadata value → `string_ref::data()` can be null (UB by precondition; works on libc++/libstdc++). Guard `r.size() ? … : std::string()`. **[edge, LOW]** `demo/grpc_server.cc:Ref`
- [x] [Review][Patch] **mktemp logs leaked** — EXIT trap kills/reaps the server but never `rm`s `SRV_LOG`/`CLI_LOG`. **[blind+edge, LOW]** `demo/run.sh` trap
- [x] [Review][Patch] **Tamper loop silent no-op if 0 context headers** — not reachable as written (2 ctx hardcoded), latent if edited to overflow/empty; assert `tampered` actually fired. **[edge, LOW]** `demo/grpc_client.cc` case 4
- [x] [Review][Defer] **Server last-wins on duplicate single-valued metadata** `[demo/grpc_server.cc]` — deferred, generic-receiver hardening beyond this demo's scope (the client sends exactly one of each).

_Dismissed (1): AC2(c) overflow log wording ("no digest provided") differs from spec text but behavior matches intent — cosmetic._

_**All 8 patches applied + verified 2026-06-29.** Demo re-run EXIT 0 (DEMO PASSED). The HIGH stale-binary fix is proven by a negative test: a deliberately broken compile now exits 1 with the compiler error and NO "DEMO PASSED" (previously would have fake-passed on stale binaries). A second self-inflicted bug found during verification — `set -e` + the EXIT trap's `wait` on a SIGTERM'd server returned 143 — was also fixed (`|| true` in the trap). Format gate clean (17 files); core kit binaries unaffected._

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

claude-opus-4-8[1m] (dev-story workflow)

### Debug Log References

- Initial `run.sh` run failed at runtime: `dyld: libgrpc++.1.46.dylib not found` — macOS SIP strips `DYLD_LIBRARY_PATH` for system-bash-spawned processes. Fixed by baking `-Wl,-rpath,<grpc libdir>` + `<protobuf libdir>` into the binaries (mirrors `build.sh`'s protobuf rpath). Cross-platform (also correct on Linux/CI).
- gRPC 1.46 headers warn under C++17 (`std::iterator` deprecated) — third-party noise, silenced per-build with `-Wno-deprecated-declarations`; demo code itself is warning-clean.

### Completion Notes List

- **Verified live, end-to-end** (`demo/run.sh`, exit 0, `DEMO PASSED`): good case `digest check: OK -> ACCEPT`; tamper case `digest MISMATCH -> REJECT` (`DATA_LOSS`, client observes `REJECTED:`); overflow → `x-process-context-overflow: true`, no digest, ACCEPT(non-blocking); missing-required → `x-routing-error: missing:x-mask-id`, `ok=false`, still delivered. All four ACs satisfied over a real localhost channel.
- **Reused, did not reinvent:** client uses `GrpcSink` + `routingmeta::Send`; server reuses `process_context_parser.h::VerifyDigest`/`ParseContext`. Only the tamper case hand-writes metadata (on purpose).
- **No regression / no wire change (CR1):** all 5 core kit binaries still exit 0; demo only reads/writes existing projected headers; core `build.sh`/CMake untouched, gRPC stays opt-in (NFR3) — built only by `demo/run.sh`.
- **Formatting:** added `demo/` to `format-check.sh` scope; demo C++ is clang-format-18.1.8-clean (17 files clean total).
- **Task 6 (CI) — recommendation, not wired:** the team can add a real-wire CI step `working-directory: grpc-routing-meta/example; run: ./demo/run.sh` on the cell that already installs `libgrpc++-dev` (it self-checks and exits non-zero on regression), complementing the HR4 compile-smoke. Left opt-in per the task.
- **Self-check oracle:** `demo/run.sh` is itself the gate — asserts good-ACCEPT + tamper-REJECT, exits non-zero on regression or absent gRPC toolchain (fail loud).

### File List

New:
- `grpc-routing-meta/example/demo/grpc_server.cc`
- `grpc-routing-meta/example/demo/grpc_client.cc`
- `grpc-routing-meta/example/demo/run.sh`
- `grpc-routing-meta/example/demo/DEMO.md`

Modified:
- `grpc-routing-meta/example/format-check.sh` (added `demo/` to the formatted-tree scope)

## Change Log

- 2026-06-29 — Implemented Story 2.1 real-wire gRPC demo (client + server + run.sh + DEMO.md); verified live (DEMO PASSED), no core regression, gRPC opt-in. Status → review.
- 2026-06-29 — Code review (3 layers): 8 patch / 1 defer / 1 dismissed. Applied all 8 — fail-loud `set -e` + drop-stale-binaries (proven by negative test), RPC deadlines, readiness-via-LISTENING + bind-failure detection, print expected/actual digest (AC1), integrity wording, `Ref()` empty-value guard, trap log cleanup + `set -e`-safe trap, tamper assert. Status → done.
