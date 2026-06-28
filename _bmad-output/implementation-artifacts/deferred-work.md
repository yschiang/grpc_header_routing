# Deferred Work

## Deferred from: code review of 1-1-projectmeta-returns-projresult-no-throw (2026-06-28)

- **Caller acts on `ProjResult.ok` / `x-routing-error`** → Story 1.4. ✅ **RESOLVED in Story 1.4** (2026-06-28): the kit `routingmeta::Send` returns `ProjResult`; `unified_sender.cc`'s empty-mask demo block captures it and surfaces `x-routing-error: missing:x-mask-id` + duration, and a guardrail test asserts the no-throw failure-as-data path. The throw→report pivot's "caller decides" half is now demonstrated.
- **Multi-missing `x-routing-error` format** → contract ratification. A message with 2+ required scalars currently emits one `x-routing-error: missing:<key>` per missing field (duplicate headers); a single-value reader sees only the first. Unreachable in the current protos (one required scalar each) and the header format is provisional (architecture Deferred). Decide the multi-missing encoding when the format is frozen or a proto adds a second required scalar.
- **Overflow → `Issue{Overflow}` plumbing** → Story 1.2. `Issue::Overflow` is defined but never produced; overflow is reported only via the `x-process-context-overflow` sink header, not in `result.issues`. Wire `EmitProcessContexts` to record the issue (and decide `ok` semantics when overflow co-occurs with missing-required) in Story 1.2.
- **Cover all 10 sys3 required-scalar messages** → Story 1.12 (test hardening). Only Submit05 is exercised; the other nine share template-identical generated code with different getter paths. Add coverage for the deep-nested getter variants when hardening projection tests.

## Deferred from: code review of 1-2-overflow-surfaces-as-non-blocking-data (2026-06-28)

- **Exact-threshold overflow boundaries** → Story 1.12 (test hardening). The three overflow triggers are strict `>` (count>25, value>512, total>7168 B); current tests use 30 / 600 / ~9 KB, so the boundary points (25/26, 512/513, 7168/7169) are unpinned — a future `>`→`>=` slip would pass. Add boundary regression cases when hardening projection tests. (Limits/conditions are unchanged by 1.2.)
- _(Caller consuming `ProjResult` / overflow issue reaching a consumer — same item already logged under Story 1.1's review → Story 1.4. Not re-listed.)_

## Deferred from: code review of 1-4-one-branchless-routingmeta-send-lives-in-the-kit (2026-06-28)

- **Kit `Send` for a non-projecting request type** → plugin hardening (no owning story yet). The plugin emits no `ProjectMeta` for a message with neither a `(routing.project)` scalar nor a `(routing.pctx)` field (`protoc-gen-meta.cc:186,210` skip), so `routingmeta::Send` would fail to instantiate for it with a confusing template error inside `send.h`. Not triggered today (every sys1/2/3 request projects something). If such a request type is ever introduced, emit a no-op `ProjectMeta` (keeps the one-Send-for-all claim literal) or a loud build-time diagnostic at the offending proto. Pre-existing plugin behavior, surfaced (not caused) by 1.4.

## Deferred from: code review of 1-3-one-coherent-routingmeta-namespace-resolved-by-adl (2026-06-28)

- **Instantiate the `GrpcSink` + ADL path** → Story 1.9 (HR4 gRPC compile-smoke). After the namespace move, the design relies on ADL resolving the namespaced `ProjectMeta` for a `routingmeta::GrpcSink` argument, but no translation unit ever calls `ProjectMeta(req, grpcSink)` — only the `GrpcSink` class body is compile-smoked, and `build.sh` has no gRPC path. Pre-existing gap (not introduced by 1.3). When wiring the CI gRPC compile-smoke (AD-14/HR4), add a one-line compile-only instantiation under `#ifdef ROUTINGMETA_WITH_GRPC` (e.g. `if (false) { grpc::ClientContext c; routingmeta::GrpcSink s(&c); ProjectMeta(req, s); }`) so the ADL path the design depends on is actually exercised.
