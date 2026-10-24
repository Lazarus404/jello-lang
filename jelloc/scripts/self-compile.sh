#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Compiles the compiler's own entry, src/Main.jello, with new_jelloc (stage 0's
# build unless JELLOC_JLO says otherwise; see env.sh), writing OUT (by default
# build/jelloc1.jlo, stage 1). Exits 0 once it compiles; otherwise with the
# compiler's status (1 for a diagnostic, 3 for an internal error), the report
# on standard error.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/env.sh"
OUT="$(abspath "${OUT:-$ROOT/build/jelloc1.jlo}")"
LISTING="${LISTING:+$(abspath "$LISTING")}"
[[ -f "$JELLOC_JLO" ]] || { echo "self-compile.sh: missing $JELLOC_JLO (run ./scripts/build_jello.sh)" >&2; exit 1; }
[[ "$OUT" != "$JELLOC_JLO" ]] || { echo "self-compile.sh: OUT is the compiler running" >&2; exit 1; }

cd "$ROOT"

# OUT is replaced only once the new build is complete: an interrupted build
# leaves the previous one, and a failed build none. LISTING, when set, gets
# the allocated program the build encoded (--jlo). Diagnostics go to standard
# error.

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT.partial"
new_jelloc src/Main.jello --std ../stdlib ${LISTING:+--jlo} --out "$OUT.partial" > "${LISTING:-/dev/null}" ||
  { status=$?; rm -f "$OUT" "$OUT.partial"; exit "$status"; }
mv "$OUT.partial" "$OUT"
echo "src/Main.jello compiles"
