# A/B experiment — superpowers vs BMAD on productionizing grpc-routing-meta

Two Claude Code instances take the **same** example-grade kit to production from the
**same** inputs (`refs/`), using **different** methodologies. They cannot see each
other's work. I (this session) judge the results against the benefit claims in
`refs/OVERVIEW.zh.md`.

## The two methodologies

| | Team A | Team B |
|---|---|---|
| Method | **superpowers** | **BMAD** |
| Pipeline | `brainstorming` → `writing-plans` → TDD impl (RED→GREEN→REFACTOR) → review | `bmad-prd` → `bmad-create-architecture` → `bmad-create-epics-and-stories` → `bmad-check-implementation-readiness` → `bmad-agent-dev` → `bmad-create-story` → `bmad-dev-story` → `bmad-code-review` |
| Workspace | `../ab-superpowers/` | `../ab-bmad/` |

## The rule

- Same start: clean prototype (`grpc-routing-meta` @ git `19bb180`) + read-only `refs/`.
- Same target: `refs/BRIEF.md` (definition of done, anchored to OVERVIEW §7/§10).
- **No peeking** — each works only inside its own workspace; neither reads the other.
- `refs/` is shared and read-only. `refs/plan.md` is *grill-me decisions*, not an impl
  plan; `refs/progress.md` was stripped (it was a superpowers-flavored tracker).

## Setup (already run by `setup-ab.sh`)

```
../ab-superpowers/        ../ab-bmad/
  grpc-routing-meta/        grpc-routing-meta/   (clean prototype @ 19bb180)
  refs/                     refs/                (SPEC, CONTEXT, OVERVIEW, plan, BRIEF)
  KICKOFF.md                _bmad/  _bmad-output/ (BMAD toolchain + output)
  (git: baseline commit)    KICKOFF.md
                            (git: baseline commit)
```

Launch one Claude Code in each dir; paste that dir's `KICKOFF.md` as the first message
(or just say "follow KICKOFF.md").

## Scoring — does the build deliver OVERVIEW's benefit claims?

**Gate (pass/fail) — a benefit you can't run isn't delivered.** From the workspace:
`./build.sh` green · 3 binaries link · `test_projection` ALL PASSED · `receiver_verify`
OK · negative-codegen gate fails as designed. A benefit relying on running code cannot
score "full" if the gate is red.

**Benefit scorecard** — each OVERVIEW §7/§10 claim scored **0 / 1 / 2**
(absent / example-grade / production-grade-and-proven):

| # | OVERVIEW benefit | Scores 2 (production-grade) when… |
|---|---|---|
| 1 | Effort 省力 | sender call is 2 lines / one path; adding a 4th system = 1 proto + 1 build-list line (demonstrated or documented) |
| 2 | 治理 / 文件 | tag + contract is the single executable definition; docs match code (no stale claims) |
| 3 | 時程 / rollout | change a proto → recodegen → all callers follow; zero hand-edited projection |
| 4 | Error handling / no silent failure | all 3 gates real: build blocks bad annotation · missing `x-mask-id` → `ProjResult`+`x-routing-error` (no throw, per plan.md) · 7 KB overflow explicit flag · receiver digest reject. **The headline claim — weight this heaviest.** |
| 5 | Quality / 一致性 | header is exact body projection; digest round-trips; canonical encoding proven by test |
| 6 | 不分歧 | one `Send<>()` path, zero `if (system==…)`; 3 systems share one schema |
| 7 | 好維護 | policy constants (7168/25/512) centralized; **portable build + CI matrix prove it beyond one machine** (this is the example→production delta) |
| 8 | 解耦 | kit not bound to gRPC; `MetadataSink` abstraction; `GrpcSink` optional/compiles |
| 9 | 好上手 | `[app]` vs `[+meta]` adoption story intact and accurate |
| 10 | 可測 | each CONTEXT.md invariant has an assert; codegen negative tests run in CI; **+ perf `duration`/bench observed (plan.md P0.4)** |

Max 20. The verdict reports per-dimension **A vs B** (who scored higher and why,
with evidence), the gate result for each, then an overall winner. Tie-breakers in
order: (1) Error-handling #4, (2) gate cleanliness, (3) simplicity — least
over-engineering for the same benefit (ponytail lens).

**Also noted, not scored on the 20:** quality of the methodology's own artifacts
(A's plan / B's PRD+architecture+stories) and git history as evidence of discipline
(TDD RED→GREEN commits vs BMAD story flow). Reported as a qualitative "process" note.

## Judging procedure (run when you tell me both are done)

1. `cd ../ab-superpowers/grpc-routing-meta/example && ./build.sh` → run gate; capture output.
2. Run the 3 binaries; capture output. Inspect the diff vs the `19bb180` baseline.
3. Repeat for `../ab-bmad/…`.
4. Score each benefit 0/1/2 with a one-line evidence cite (file:line / command output).
5. Emit the scorecard table, per-dimension winner, process note, overall verdict.

I score blind to which I prefer — same rubric, same commands, same evidence for both.

---

## Kickoff prompts (also written into each workspace as KICKOFF.md)

### Team A — `../ab-superpowers/KICKOFF.md`

> You are productionizing an example-grade C++ kit. Read `refs/BRIEF.md` (the
> definition of done) and the other `refs/` files. The kit is in
> `grpc-routing-meta/`. **Method: superpowers.** Run `brainstorming` first, then
> `writing-plans` to produce a plan, then implement strictly with TDD
> (RED→GREEN→REFACTOR — no production code without a failing test first), then
> request code review. Work **only** inside this workspace; do not look elsewhere.
> Commit locally as you go (concise messages, no co-author trailer); do not push.
> Done = every acceptance criterion in `refs/BRIEF.md` met and the Verify block green.

### Team B — `../ab-bmad/KICKOFF.md`

> You are productionizing an example-grade C++ kit. Read `refs/BRIEF.md` (the
> definition of done) and the other `refs/` files. The kit is in
> `grpc-routing-meta/`. **Method: BMAD.** Run the chain: `bmad-prd` →
> `bmad-create-architecture` → `bmad-create-epics-and-stories` →
> `bmad-check-implementation-readiness` → `bmad-agent-dev` → `bmad-create-story` →
> `bmad-dev-story` → `bmad-code-review`. BMAD config + output dirs are under
> `_bmad/` and `_bmad-output/`. Work **only** inside this workspace; do not look
> elsewhere. Commit locally as you go (concise messages, no co-author trailer); do
> not push. Done = every acceptance criterion in `refs/BRIEF.md` met and the Verify
> block green.
