# Deferred Work

## Deferred from: code review of 1-1-projectmeta-returns-projresult-no-throw (2026-06-28)

- **Caller acts on `ProjResult.ok` / `x-routing-error`** → Story 1.4. The throw→report pivot means a caller that ignores the result sends a request with `x-routing-error` but no routing key. By design at the kit level (report, don't dictate — AD-5 / SPEC §7); the demo `Send` must surface `ok`/`x-routing-error` when `Send` moves into the kit (Story 1.4 ACs already cover this).
- **Multi-missing `x-routing-error` format** → contract ratification. A message with 2+ required scalars currently emits one `x-routing-error: missing:<key>` per missing field (duplicate headers); a single-value reader sees only the first. Unreachable in the current protos (one required scalar each) and the header format is provisional (architecture Deferred). Decide the multi-missing encoding when the format is frozen or a proto adds a second required scalar.
- **Overflow → `Issue{Overflow}` plumbing** → Story 1.2. `Issue::Overflow` is defined but never produced; overflow is reported only via the `x-process-context-overflow` sink header, not in `result.issues`. Wire `EmitProcessContexts` to record the issue (and decide `ok` semantics when overflow co-occurs with missing-required) in Story 1.2.
- **Cover all 10 sys3 required-scalar messages** → Story 1.12 (test hardening). Only Submit05 is exercised; the other nine share template-identical generated code with different getter paths. Add coverage for the deep-nested getter variants when hardening projection tests.
