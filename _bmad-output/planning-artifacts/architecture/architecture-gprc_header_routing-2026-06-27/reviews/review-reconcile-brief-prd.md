# Reconciliation Review — ARCHITECTURE-SPINE vs BRIEF / plan.md / PRD

**Reviewer role:** independent reconciliation reviewer. Question asked: does the
spine faithfully carry the requirements inputs, and does any decision contradict them?

**Inputs read (only these):**
- Spine — `_bmad-output/planning-artifacts/architecture/architecture-gprc_header_routing-2026-06-27/ARCHITECTURE-SPINE.md`
- `refs/BRIEF.md` (scored definition of done: A–I + Verify)
- `refs/plan.md` (locked design decisions)
- `_bmad-output/planning-artifacts/PRD.md`

**Verdict: PASS-WITH-FINDINGS** — coverage is broad and the Capability→Architecture
map is honest, but **one decision (AD-4) contradicts a *locked* plan.md decision and
misreports the scope of that conflict**, and one locked P1 item (HR2) is silently
dropped. Both are reconcilable but must be surfaced before this spine is treated as
faithful to the inputs.

---

## 1. Coverage of BRIEF criteria A–I

Every criterion has an explicit home in the spine's "Capability → Architecture Map":

| Criterion | Home in spine | Faithful? |
|---|---|---|
| A Portable build | AD-13 | Yes |
| B Proven matrix | AD-14 | Yes |
| C No silent failure | AD-5, AD-8, AD-9 + Digest convention (receiver) | Yes |
| D Exact projection | AD-1 + Canonical/Digest conventions | Yes |
| E One sender path | AD-1, AD-3, AD-4 | **Behavior yes; see C1 — contradicts plan.md on placement** |
| F Policy centralized | AD-7 | Yes |
| G Testable invariants | AD-8, AD-12, conventions | Partial — see H1 (HR2/parser not represented) |
| H Perf observed | AD-6 | Yes; acceptance specifics thin — see L2 |
| I Docs match code | AD-5, AD-9 | Yes |

No criterion is left without a home. The only substantive issue is E (decision
conflict, not a coverage gap).

## 2. PRD requirement coverage (FR / NFR / CR / HR)

| Req | Home | Status |
|---|---|---|
| FR1 Failure-as-data (no throw) | AD-5 (ProjectMeta) | Governed for ProjectMeta; **Send half becomes ungoverned once AD-4 evicts Send from the kit — see M3** |
| FR2 Missing required scalar | AD-5 | Governed (exact) |
| FR3 Overflow non-blocking | AD-5 + AD-9 (count/format/suppress frozen to SPEC §5.4) | Governed |
| FR4 Build-time gate | AD-8 | Governed (exact) |
| FR5 Receiver gate | Digest convention + AD-9 | Governed |
| FR6 One sender path **in the kit** | AD-4 | **Contradicted — see C1** |
| FR7 Perf trace | AD-6 | Governed; sub-ms bar + 1/2/25/60 counts not carried — L2 |
| FR8 Canonical projection | Canonical/Digest conventions | Governed |
| NFR1 Portability | AD-13 | Governed |
| NFR2 Proven matrix | AD-14 | Governed |
| NFR3 No new runtime deps | AD-11 | Governed |
| NFR4 Determinism | (implied by Canonical/Digest conventions) | **Not named — L1** |
| NFR5 Thread-safety | AD-12 | Governed |
| NFR6 Centralized policy | AD-7 | Governed |
| NFR7 Observability w/o coupling | AD-4 + Cross-cutting convention | Governed |
| CR1 Wire unchanged | AD-9 | Governed |
| CR2 Additive API | AD-10 | Governed |
| CR3 refs/ read-only | (implied by "live copies" in map) | **Not stated as a constraint — M1** |
| HR1 Crypto/encoding vectors | AD-11 ("harden tests") | Governed (thin; specifics code-owned) |
| HR2 Parser robustness | — | **Silently dropped — H1** |
| HR3 Thread-safety + concurrent test | AD-12 (names HR3) | Governed |
| HR4 gRPC adapter in CI | AD-14 (names HR4) | Governed |

Silent drops: **HR2** (H1). Implicit-only: NFR4 (L1), CR3 (M1).

## 3. Contradiction analysis — AD-4 "the kit ships NO `Send` symbol"

The task asked to verify AD-4 against BRIEF criterion E *as literally written*, and
to confirm/refute that AD-4 "only conflicts with PRD FR6/G-E."

**BRIEF criterion E, exact text:**
> **E. One sender path** (benefit「不分歧」). `Send<>()` / `FillCommon`+`ProjectMeta`
> serve sys1/sys2/sys3 with zero `if (system==…)`.

**Confirmed:** BRIEF E is **location-agnostic.** It specifies the *behavior* — a
single branchless path (`Send<>()` *or* `FillCommon`+`ProjectMeta`), zero per-system
`if`. It says nothing about whether `Send` lives in the kit or the demo. The spine's
claim is correct, and the BRIEF Verify block exercises this via `./build/unified_sender`
(the demo), which is consistent with a demo-resident `Send`. So AD-4 does **not**
contradict BRIEF E. ✔

**Refuted:** the spine's footnote — *"Conflicts with PRD FR6/G-E … reconcile upstream"* —
claims AD-4 conflicts **only** with the PRD. It does not. `plan.md` is a **locked**
input, and it places `Send` in the kit twice:

- `plan.md` **P0.3 (must-have):** *"Failure-as-data — `ProjResult` change; **promote
  `Send` into kit**; …"* — an explicit, locked instruction to move `Send` into the kit.
- `plan.md` **"The pivot API change":** lists `template <class Req> ProjResult
  Send(const Req&, const Runtime&, MetadataSink&);` as part of the kit's pivot API
  surface, immediately beside `ProjResult` / `Issue`.

AD-4 ships **no `Send` symbol** and pushes the `Send<>()` template into the demo. That
**directly contradicts plan.md P0.3 and the plan.md pivot API** — not merely the PRD.
The footnote's "only conflicts with PRD FR6/G-E" is therefore **false**: the deeper,
higher-authority conflict is with the locked plan.

**Root cause of the misreconciliation.** AD-4 cites SPEC §7 "report, don't dictate"
and plan.md's "Failure: Report, don't dictate" / "Observability: returned result" to
justify evicting `Send`. But those locked decisions are about the *failure policy* —
the kit **reports** issues/duration and the **caller decides abort vs proceed** (which
plan.md *does* leave to the Sender, and lists as an Open cross-team decision). They are
**not** a decision about where the `Send` wrapper lives. plan.md keeps both at once:
`Send` is *in the kit* (P0.3) **and** the abort/proceed decision is the *caller's*.
AD-4 conflates "the abort/proceed *decision* is the caller's" (locked, true) with
"`Send` *orchestration* is the caller's" (contradicts plan.md). The kit can host a
`Send` template that does `FillCommon`+`ProjectMeta`+timing and returns `ProjResult`,
while the caller still owns the decision — exactly what plan.md specifies.

This is the headline finding (C1). It is reconcilable, but the spine must either align
to plan.md or — if it genuinely intends to deviate — name the *plan.md* contradiction
(not just the PRD) and route it through the same cross-team ratification plan.md
reserves, because a methodology A/B run is scored on fidelity to the locked plan.

No other AD contradicts BRIEF or plan.md. AD-7's HPACK-`32` single-source sharpen,
AD-13's pkg-config discovery, and AD-10's "provided + detectable" framing are all
consistent sharpenings, not contradictions.

## 4. Quiet requirements / tone dropped

- **Fidelity to plan.md as a *methodology A/B run*** (PRD §3.4: *"fidelity to the locked
  plan; this is a methodology A/B run"*; CLAUDE.md "build only from refs/"). The spine
  lists `refs/plan.md` as a source but neither states this constraint nor honors it —
  AD-4 actively overrides a locked plan.md P0 decision. This is the *meta* of C1: the
  one tone that most needed carrying (don't deviate from the locked plan) is the one the
  structure dropped. (H2)
- **`refs/` is read-only** (CR3; BRIEF Rules; CLAUDE.md). Present only implicitly via the
  map's "(live copies)" parenthetical. (M1)
- **No push to remote** (CLAUDE.md; BRIEF Rules; PRD §4/§6). AD-14 specifies a full
  GitHub Actions matrix with no acknowledgement that the workspace forbids push and that
  CI therefore cannot be literally green-validated here (PRD §6 risk). (M2)
- "Work only inside this workspace / don't look at other team's work" — a process rule,
  architecturally inert; not a spine defect. Noted only for completeness.

---

## Findings (tiered)

### CRITICAL

**C1 — AD-4 contradicts the locked plan.md decision "promote `Send` into kit," and the
spine misreports the conflict as PRD-only.**
- **Location:** Spine AD-4 (lines 85–88) and its footnote; Capability map row E
  (line 185). Against `plan.md` P0.3 and `plan.md` "The pivot API change".
- **What's wrong:** AD-4 ships *no* `Send` symbol and relocates `Send<>()` to the demo.
  `plan.md` (a *locked* input) explicitly says "**promote `Send` into kit**" (P0.3) and
  defines `Send` in the kit's pivot API. The AD-4 footnote claims it "Conflicts with PRD
  FR6/G-E … reconcile upstream" — naming only the derived PRD and omitting the
  higher-authority plan.md contradiction. The justification ("report, don't dictate")
  governs the *abort/proceed decision*, not `Send`'s location; plan.md keeps `Send` in
  the kit **and** the decision with the caller. (BRIEF E itself is location-agnostic, so
  AD-4 is fine against BRIEF — the conflict is with plan.md + PRD, not BRIEF.)
- **Suggested fix:** Align AD-4 to plan.md — ship the `Send<>()` template in the kit
  (`FillCommon` + `ProjectMeta` + timing → `ProjResult`), leaving only the abort/proceed
  *decision* to the caller (which already satisfies "report, don't dictate" and
  NFR7/AD-10). If deviation is truly intended, rewrite the footnote to name the
  **plan.md P0.3 / pivot-API** contradiction explicitly and mark AD-4 status as
  *pending cross-team ratification* (not silently `[ADOPTED]`-adjacent), since the A/B
  run is scored on plan.md fidelity.

### HIGH

**H1 — HR2 (parser robustness) silently dropped.**
- **Location:** Spine — absent from all ADs, Conventions, Deferred. cf. PRD HR2;
  `plan.md` P1 "Parser tests + policy". Siblings HR1/HR3/HR4 each have a home
  (AD-11 / AD-12 / AD-14), making the omission conspicuous.
- **What's wrong:** plan.md P1 (locked, "all four, trimmed") requires receiver-side
  negative no-crash cases plus documenting "lenient parse + dup-key-last-wins." The
  spine names `process_context_parser` in the Structural Seed only; no invariant or
  convention governs parser robustness or the dup-key-last-wins rule. Nothing routes it
  to Deferred either — it is a true silent drop of a locked P1 item.
- **Suggested fix:** Add a one-line convention (Error-shape/parser row) — "receiver
  parse is lenient; a malformed `%`-escape is passed through literally (SPEC §6);
  duplicate keys: last wins" — and reference HR2 under the G map row, or list it in
  Deferred with a reason if intentionally cut (it should not be — it is locked P1).

**H2 — "Fidelity to plan.md / methodology A/B run" not carried, and actively violated.**
- **Location:** Spine front-matter/header (lines 1–21) lists `refs/plan.md` as a source
  but states no fidelity constraint; AD-4 then overrides plan.md P0.3.
- **What's wrong:** PRD §3.4 makes plan.md fidelity an explicit, scored constraint for
  this A/B run. The spine's structure has no place that records "the locked plan governs;
  deviations require ratification," which is precisely how C1 slipped in unflagged.
- **Suggested fix:** Add a one-line invariant or header note: "This is a methodology A/B
  run; `refs/plan.md` decisions are locked — any AD that deviates must name the plan.md
  decision and carry a *pending-ratification* status." Then re-audit ADs against plan.md
  (only AD-4 currently violates it).

### MEDIUM

**M1 — CR3 (`refs/` read-only) not stated as a constraint.**
- **Location:** Spine — only implied by "(live copies)" in the I map row (line 189).
- **What's wrong:** CR3 is a hard guardrail (BRIEF Rules + CLAUDE.md). The doc-truth work
  (criterion I / E5) edits `CONTEXT.md`/`OVERVIEW.zh.md`/`README.md`; the spine never says
  these must be the `grpc-routing-meta/` copies and that `refs/` must not be touched.
- **Suggested fix:** Add to Conventions (or the I row): "doc updates target the
  `grpc-routing-meta/` live copies only; `refs/` and `SPEC.md` are read-only (CR3)."

**M2 — No-push / CI-can't-be-green constraint absent from AD-14.**
- **Location:** Spine AD-14 (lines 135–138); Stack/CI row. cf. CLAUDE.md, BRIEF Rules,
  PRD §6 risk row 1.
- **What's wrong:** AD-14 specifies a full GitHub Actions matrix as if it will run, with
  no note that the workspace forbids push and CI therefore can't be push-validated here
  (PRD §6 mitigation: author the workflow to mirror the locally-verified steps).
- **Suggested fix:** Add to AD-14: "No push to any remote (workspace rule); the workflow
  mirrors exactly the locally-verified steps and is validated by local path equivalence,
  not by a green remote run."

**M3 — FR1's "`Send` MUST NOT throw" guarantee becomes ungoverned (consequence of C1).**
- **Location:** AD-5 covers `ProjectMeta` no-throw; with `Send` evicted by AD-4, no kit
  contract governs the `Send` no-throw guarantee FR1 requires.
- **What's wrong:** FR1 binds *both* `ProjectMeta` *and* `Send` to no-throw. Pushing
  `Send` into the demo drops it from the governed surface.
- **Suggested fix:** Resolves automatically if C1 is fixed (Send back in kit, AD-5
  extends to it). If AD-4 stands, AD-5 must explicitly note the demo `Send` must also be
  no-throw, or FR1 must be restated.

### LOW

**L1 — NFR4 (determinism) implied but not named.**
- **Location:** Conventions (Canonical encoding + Digest).
- **Fix:** One clause — "encoding is byte-identical run-to-run; digest reproducible; key
  order fixed (NFR4)" — folded into the Canonical-encoding convention.

**L2 — Criterion H / FR7 acceptance specifics not carried into AD-6.**
- **Location:** AD-6 (lines 95–98) and H map row.
- **What's wrong:** The bench's pass condition — sub-ms, for 1/2/25/60 contexts — lives
  only in BRIEF H / FR7, not in the AD that governs the bench. The gate is underspecified
  at spine altitude.
- **Fix:** Add to AD-6 or the H row: "`bench_projection` asserts sub-ms for 1/2/25/60
  contexts."

**L3 — AD-13 discovery mechanism wording diverges from NFR1.**
- **Location:** AD-13 (pkg-config) vs PRD NFR1 ("derive from `protoc` location").
- **What's wrong:** Compatible (env override first, then discovery), but the mechanism
  wording differs; PRD §4/CR2 defers mechanism to architecture, so this is a sharpening,
  not a conflict.
- **Fix:** None required; optionally note AD-13 supersedes NFR1's parenthetical mechanism.

---

## Bottom line

The spine carries A–I and nearly all PRD requirements with an honest, traceable map.
The reconciliation defect is concentrated in **AD-4**: it is correct that BRIEF E is
location-agnostic, but it overrides the **locked** plan.md decision "promote `Send`
into kit" while telling the reader the conflict is PRD-only — exactly the kind of quiet
deviation a plan-fidelity-scored A/B run must not contain. Fix C1 (and the dropped HR2),
state the plan.md-fidelity and read-only/no-push guardrails, and the spine reconciles.
