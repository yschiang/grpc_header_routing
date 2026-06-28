#!/usr/bin/env bash
# Formatting gate: the hand-written tree must be clang-format-clean under the
# PINNED version (skew = phantom diffs, so the version is enforced, not assumed).
# sha256.h's dense reference crypto is exempted in-file via // clang-format off.
#
#   ./format-check.sh        verify (CI mode): non-zero exit on any drift
#   ./format-check.sh fix    apply formatting in place
#
# Override the binary with CLANG_FORMAT=/path/to/clang-format (CI points this at
# the pip-pinned build: pip install clang-format==18.1.8).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"

PINNED=18.1.8
CF="${CLANG_FORMAT:-clang-format}"
have="$("$CF" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
[ "$have" = "$PINNED" ] || {
  echo "clang-format $PINNED required (got '${have:-none}'). Install: pip install clang-format==$PINNED" >&2
  exit 2
}

# Hand-written sources only; generated code lives under build/ and is never listed.
FILES=$(find receiver sender src tests demo -type f \( -name '*.cc' -o -name '*.h' \) | sort)

if [ "${1:-}" = "fix" ]; then
  $CF -i $FILES
  echo "formatted $(echo "$FILES" | wc -l | tr -d ' ') files"
else
  $CF --dry-run -Werror $FILES
  echo "format-check OK — $(echo "$FILES" | wc -l | tr -d ' ') files clean (clang-format $PINNED)"
fi
