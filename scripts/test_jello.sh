#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Tests the compiler under test (JELLOC_JLO, JELLOC_VM, JELLOC_JDLL; env.sh)
# with the test tiers (scripts/test.sh). For stage 0's build, the default, it
# first builds the compiler from the seed (./scripts/build_jello.sh), then checks the
# fixed point and sweeps the sources (scripts/fixed-point.sh), runs the tiers
# under stage 1, and checks what no test stage 1 builds can vouch for: stage
# 1's bytes decode, stage 1 compiles alike in every mode, and jelloc, its
# native build, compiles the compiler to stage 1.

set -euo pipefail
[[ $# -eq 0 ]] || { echo "usage: $0 (takes no arguments; jelloc/scripts/test.sh runs chosen tiers)" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/jelloc/scripts/lib.sh"
cd "$ROOT/jelloc"
TMP="$(mktemp -d)"

# A failure stops the background runs (kill_jobs).

trap 'kill_jobs; rm -rf "$TMP"' EXIT

# The width tables are generated from the vendored UCD; drift fails.

command -v python3 >/dev/null || fail "python3 is required to check src/frontend/syntax/UnicodeWidth.jello"
scripts/gen_unicode_width.py > "$TMP/UnicodeWidth.jello" || fail "scripts/gen_unicode_width.py failed"
cmp -s "$TMP/UnicodeWidth.jello" src/frontend/syntax/UnicodeWidth.jello ||
  fail "src/frontend/syntax/UnicodeWidth.jello differs from scripts/gen_unicode_width.py's output; regenerate it"

# Another compiler gets the tiers alone. The default is env.sh's path, which
# on a fresh clone names no file until build_jello.sh builds it.

DEFAULT="$COMPILER_DIR/build/jelloc.jlo"
if ! [[ "$JELLOC_JLO" == "$DEFAULT" || "$JELLOC_JLO" -ef "$DEFAULT" ]]; then
  scripts/test.sh > "$TMP/tiers.out" 2>&1 || fail "$(cat "$TMP/tiers.out")"
  grep -v '^=== RUN' "$TMP/tiers.out"
  exit 0
fi
"$ROOT/scripts/build_jello.sh" > /dev/null

# The fixed point and the sweep, the test tiers and jelloc compiling the
# compiler run at once, the tiers under stage 1, which the fixed point makes
# stages 2 and 3 too. When stage 0's build is another program, the sweep
# holds it to stage 1's output on every source.

STAGE1="$PWD/build/jelloc1.jlo"
LISTING="$TMP/stage1.ir" scripts/fixed-point.sh > "$TMP/stages.out" 2>&1 &
stages_pid=$!

# At the fixed point, stage 1 is what it builds from its own sources
# (Shape.test).

JELLOC_JLO="$STAGE1" FIXED_POINT=1 scripts/test.sh > "$TMP/tiers.out" 2>&1 &
tiers_pid=$!

# The fixtures that any order of running prints alike, on many schedulers.

JELLOC_JLO="$STAGE1" scripts/schedulers.sh > "$TMP/schedulers.out" 2>&1 &
schedulers_pid=$!
env -u JELLO_JDLL_PATH "$VM_BUILD/jelloc" src/Main.jello --std ../stdlib --out "$TMP/native.jlo" > "$TMP/native.out" 2>&1 &
native_pid=$!

# stage1 VM-FLAG ARGS...: stage 1 on this VM, run with VM-FLAG; first, lazily
# on the JIT, as the tiers run it, the program the other modes must match.

stage1() { JELLOC_JLO="$STAGE1" JELLOC_VM="$VM $1" new_jelloc "${@:2}" 2>&1; }
parser=tests/workload/parser.jello
joined=tests/e2e/programs/lists/joined.jello
stage1 "" "$parser" --std "$ROOT/stdlib" --out "$TMP/parser.jlo" > "$TMP/parser.out" &
parser_pid=$!

# Stage 1 loads (the VM validates it) and decodes to the program it
# allocated (Shape.test holds its moves). Eagerly (arm64) and interpreted,
# it must compile a program to the same bytes as on the JIT and reject one
# with the same diagnostic.

wait "$stages_pid" || fail "$(cat "$TMP/stages.out")"
cat "$TMP/stages.out"
decodes "$TMP/stage1.ir" "$STAGE1" "$TMP/stage1.dis" "stage 1"
wait "$parser_pid" || fail "stage 1 cannot compile workload/parser: $(cat "$TMP/parser.out")"
rejected="$(stage1 "" "$joined" --out "$TMP/stage1-rejected.jlo")" || true
for mode in ${REQUIRE_JIT:+eager} no-jit; do
  flag="$(mode_flag "$mode")"
  rm -f "$TMP/stage1-parser.jlo"
  result="$(stage1 "$flag" "$parser" --std "$ROOT/stdlib" --out "$TMP/stage1-parser.jlo")" ||
    fail "stage 1 ($mode) cannot compile workload/parser: $result"
  cmp -s "$TMP/stage1-parser.jlo" "$TMP/parser.jlo" || fail "stage 1 ($mode) compiles workload/parser to other bytes"
  result="$(stage1 "$flag" "$joined" --out "$TMP/stage1-rejected.jlo")" && status=0 || status=$?
  expect "$status:$result" "1:$rejected" "stage 1 ($mode) rejects lists/joined"
done

# jelloc, the native build of stage 1 (./scripts/build_jello.sh), loads std.jdll from
# beside it, prints nothing when it compiles, and compiles the compiler to
# stage 1.

wait "$native_pid" || fail "jelloc cannot compile src/Main.jello: $(cat "$TMP/native.out")"
[[ ! -s "$TMP/native.out" ]] || fail "jelloc printed $(cat "$TMP/native.out") compiling src/Main.jello"
cmp -s "$TMP/native.jlo" "$STAGE1" || fail "jelloc compiles src/Main.jello to other bytes than stage 1's"
echo "== jelloc is stage 1 =="

wait "$tiers_pid" || fail "$(cat "$TMP/tiers.out")"
grep -v '^=== RUN' "$TMP/tiers.out"
wait "$schedulers_pid" || fail "$(cat "$TMP/schedulers.out")"
cat "$TMP/schedulers.out"

# The reference's native-library examples, whose C Docs.test cannot build.

"$ROOT/scripts/test_native_docs.py" || fail "the native-library examples failed"
