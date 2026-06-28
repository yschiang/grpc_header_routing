#!/usr/bin/env bash
# Build two isolated A/B workspaces from the clean prototype (19bb180) + refs.
# Safe: refuses to clobber an existing workspace. Run once from repo root.
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
BASE="$(dirname "$REPO")"                 # ~/workspace
PROTO_REF="19bb180"                        # clean example-grade prototype
A="$BASE/ab-superpowers"
B="$BASE/ab-bmad"

make_ws() {                                # $1=dir  $2=method-label
  local ws="$1"
  [ -e "$ws" ] && { echo "SKIP: $ws exists (not clobbering)"; return 1; }
  mkdir -p "$ws"
  git -C "$REPO" archive "$PROTO_REF" grpc-routing-meta | tar -x -C "$ws"
  cp -R "$REPO/refs" "$ws/refs"
  printf '.DS_Store\n' > "$ws/.gitignore"
  # fairness: drop superseded archive; sync stale in-kit CONTEXT to canonical refs
  rm -rf "$ws/grpc-routing-meta/archive"
  cp "$ws/refs/CONTEXT.md" "$ws/grpc-routing-meta/CONTEXT.md"
  sed -i '' \
    -e '/\*\*Full background:\*\*/d' \
    -e '/^  summary, codegen walkthrough\.$/d' \
    -e '/^archive\//d' \
    "$ws/grpc-routing-meta/README.md"
  cat > "$ws/CLAUDE.md" <<'EOF'
# CLAUDE.md — workspace rules

Isolated A/B-experiment workspace. Task + definition of done: `refs/BRIEF.md`.
Read `refs/` (read-only) and the live code in `grpc-routing-meta/`. Method: `KICKOFF.md`.

## Rules
- **Commits:** never add a `Co-Authored-By` trailer; keep messages concise and concrete.
  Commit locally as you work; do **not** push to any remote.
- **Do not read superseded design docs.** `archive/` was removed on purpose — build only
  from `refs/` (SPEC, CONTEXT, OVERVIEW, plan, BRIEF) and the live code.
- **Stay in this workspace.** Do not read outside it; do not look for another team's work.
EOF
  return 0
}

baseline() {                               # $1=dir
  git -C "$1" init -q
  git -C "$1" add -A
  git -C "$1" -c user.name='ab-setup' -c user.email='ab@local' \
      commit -q -m "baseline: clean prototype (19bb180) + refs + workspace rules"
}

# ---- Team A: superpowers ----
if make_ws "$A"; then
  cat > "$A/KICKOFF.md" <<'EOF'
# KICKOFF — Team A (superpowers)

You are productionizing an example-grade C++ kit. Read `refs/BRIEF.md` (the
definition of done) and the other `refs/` files. The kit is in `grpc-routing-meta/`.

**Method: superpowers.** Run `brainstorming` first, then `writing-plans` to produce
a plan, then implement strictly with TDD (RED -> GREEN -> REFACTOR; no production
code without a failing test first), then request code review.

Work **only** inside this workspace; do not look elsewhere. Commit locally as you go
(concise messages, no co-author trailer); do not push. Done = every acceptance
criterion in `refs/BRIEF.md` met and its Verify block green.
EOF
  baseline "$A"
  echo "OK: $A"
fi

# ---- Team B: BMAD ----
if make_ws "$B"; then
  cp -R "$REPO/_bmad" "$B/_bmad"
  cp -R "$REPO/_bmad-output" "$B/_bmad-output"
  printf '.DS_Store\n_bmad/\n' > "$B/.gitignore"   # track _bmad-output (B's work), ignore toolchain
  cat > "$B/KICKOFF.md" <<'EOF'
# KICKOFF — Team B (BMAD)

You are productionizing an example-grade C++ kit. Read `refs/BRIEF.md` (the
definition of done) and the other `refs/` files. The kit is in `grpc-routing-meta/`.

**Method: BMAD.** Run the chain:
`bmad-prd` -> `bmad-create-architecture` -> `bmad-create-epics-and-stories` ->
`bmad-check-implementation-readiness` -> `bmad-agent-dev` -> `bmad-create-story` ->
`bmad-dev-story` -> `bmad-code-review`.
BMAD config + output dirs are under `_bmad/` and `_bmad-output/`.

Work **only** inside this workspace; do not look elsewhere. Commit locally as you go
(concise messages, no co-author trailer); do not push. Done = every acceptance
criterion in `refs/BRIEF.md` met and its Verify block green.
EOF
  baseline "$B"
  echo "OK: $B"
fi

echo "Done. Launch a Claude Code in each dir and point it at KICKOFF.md."
