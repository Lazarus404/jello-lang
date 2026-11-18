#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# HashMap versus interned ID arrays. This reports measurements only; it has no
# baseline or performance threshold, so benchmark noise never fails a test.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAMPLES="${SAMPLES:-5}"
[[ "$SAMPLES" =~ ^[1-9][0-9]*$ ]] || { echo "bench_hash_map.sh: SAMPLES must be positive" >&2; exit 2; }

JELLOC_JLO="${JELLOC_JLO:-$ROOT/jelloc/build/jelloc1.jlo}"
[[ -f "$JELLOC_JLO" ]] || { echo "bench_hash_map.sh: missing $JELLOC_JLO (run ./scripts/test_jello.sh)" >&2; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
JELLO_JDLL_PATH="$ROOT/build/vm" "$ROOT/build/vm/jello" "$JELLOC_JLO" \
  "$ROOT/tests/bench/hash_map.jello" --std "$ROOT/stdlib" --out "$TMP/hash_map.jlo"

python3 - "$SAMPLES" "$ROOT/build/vm/jello" "$ROOT/build/vm" "$TMP/hash_map.jlo" <<'EOF'
import os, re, statistics, subprocess, sys

samples, vm, native_dir, program = int(sys.argv[1]), *sys.argv[2:]
fields = ("map-insert-ms", "map-lookup-ms", "ids-insert-ms", "ids-lookup-ms")
for mode, flag in (("JIT", []), ("interpreter", ["--no-jit"])):
    rows = []
    for _ in range(samples):
        out = subprocess.check_output([vm, *flag, program], env=dict(os.environ, JELLO_JDLL_PATH=native_dir), text=True).strip()
        row = dict(re.findall(r"([a-z-]+)=([0-9]+)", out))
        if set(fields) - row.keys() or "keys" not in row or "checksum" not in row:
            raise SystemExit(f"unexpected benchmark output: {out}")
        rows.append(row)
    print(f"{mode}: keys={rows[0]['keys']} samples={samples} checksum={rows[0]['checksum']}")
    print("  " + " ".join(f"{field}={statistics.median(int(row[field]) for row in rows):.0f}ms" for field in fields))
EOF
