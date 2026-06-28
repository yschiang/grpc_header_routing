# Deferred Work

## Deferred from: code review of 1-1-projectmeta-returns-projresult-no-throw (2026-06-28)

- **Caller acts on `ProjResult.ok` / `x-routing-error`** → Story 1.4. The throw→report pivot means a caller that ignores the result sends a request with `x-routing-error` but no routing key. By design at the kit level (report, don't dictate — AD-5 / SPEC §7); the demo `Send` must surface `ok`/`x-routing-error` when `Send` moves into the kit (Story 1.4 ACs already cover this).
- **Multi-missing `x-routing-error` format** → contract ratification. A message with 2+ required scalars currently emits one `x-routing-error: missing:<key>` per missing field (duplicate headers); a single-value reader sees only the first. Unreachable in the current protos (one required scalar each) and the header format is provisional (architecture Deferred). Decide the multi-missing encoding when the format is frozen or a proto adds a second required scalar.
- **Overflow → `Issue{Overflow}` plumbing** → Story 1.2. `Issue::Overflow` is defined but never produced; overflow is reported only via the `x-process-context-overflow` sink header, not in `result.issues`. Wire `EmitProcessContexts` to record the issue (and decide `ok` semantics when overflow co-occurs with missing-required) in Story 1.2.
- **Cover all 10 sys3 required-scalar messages** → Story 1.12 (test hardening). Only Submit05 is exercised; the other nine share template-identical generated code with different getter paths. Add coverage for the deep-nested getter variants when hardening projection tests.

## Deferred from: code review of 1-2-overflow-surfaces-as-non-blocking-data (2026-06-28)

- **Exact-threshold overflow boundaries** → Story 1.12 (test hardening). The three overflow triggers are strict `>` (count>25, value>512, total>7168 B); current tests use 30 / 600 / ~9 KB, so the boundary points (25/26, 512/513, 7168/7169) are unpinned — a future `>`→`>=` slip would pass. Add boundary regression cases when hardening projection tests. (Limits/conditions are unchanged by 1.2.)
- _(Caller consuming `ProjResult` / overflow issue reaching a consumer — same item already logged under Story 1.1's review → Story 1.4. Not re-listed.)_
