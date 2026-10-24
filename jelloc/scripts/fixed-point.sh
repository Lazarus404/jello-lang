#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# ./scripts/build_jello.sh has the seed - old compiler - build the compiler,
# build/jelloc.jlo ("jelloc0"), and that build - new compiler - build/jelloc1.jlo.
# The new compiler builds build/jelloc2.jlo and that build/jelloc3.jlo

# (self-compile.sh). Then:
# - jelloc1, jelloc2 and jelloc3 are one program, byte for byte; the fixed
#   point. jelloc0 and jelloc1 run the same source, so jelloc2 equal to
#   jelloc1 says both compile src/Main.jello alike; a difference is a
#   nondeterminism or a miscompile, by the seed or by the compiler itself, or
#   a new compiler built from other sources (rerun ./scripts/build_jello.sh).
#
# - jelloc1 compiles every swept source (sweep) without trapping, and jelloc0
#   agrees with it on each; the --hir, --ir and --jlo output, the module
#   bytes, the diagnostics and the exit status. When the seed is current,
#   jelloc0 is jelloc1, byte for byte, and is not run again.
#
# - The new compiler emits the seed's own module, byte for byte, for every swept
#   source the seed compiles, so a change to the compiler leaves the code it emits
#   alone until a promotion accepts it (promote-seed.sh, which sets
#   PROMOTING and is not held to it). A source only the new compiler
#   compiles is new and is not compared.

# LISTING, when set, gets stage 2's allocated program, which is stage 1's.
# Sorry I couldn't make it any more clear. It makes me cross-eyed, too.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Always from build_jello.sh's builds, whatever compiler the caller tests.

unset JELLOC_JLO JELLOC_VM JELLOC_JDLL
. "$ROOT/scripts/lib.sh"
cd "$ROOT"
TMP="$(mktemp -d)"
trap 'kill_jobs; rm -rf "$TMP"' EXIT

# sweep COMPILER: COMPILER's output for every source in itests, the e2e
# fixtures (tests/e2e/programs and tests/e2e/cli) and the workload
# (tests/workload; find does not follow its frontend link), in
# $TMP/COMPILER-sweep, one file per form, each ending with the exit status.
# --hir checks what --ir and --jlo do first, so a source it rejects is not
# run again. A run must compile (0) or reject (1) without trapping; anything
# else did not run the compiler, and would compare equal to the same failure
# under the other one.

sweep() (
  export JELLOC_JLO="$PWD/$1"
  each_source "$TMP/$(basename "$1" .jlo)-sweep" '
    run() {
      new_jelloc "${@:2}" > "$1" 2>&1 && status=0 || status=$?
      echo "status $status" >> "$1"
      if [[ $status -gt 1 ]] || grep -qE "(^|: )jello: " "$1"; then echo "$1: $(head -3 "$1")" >&2; exit 1; fi
    }
    run "$o.hir" "$1" --std ../stdlib --hir
    [[ $status -eq 0 ]] || exit 0
    run "$o.ir" "$1" --std ../stdlib --ir
    run "$o.listing" "$1" --std ../stdlib --jlo --out "$o.jlo"' itests tests/e2e/programs tests/e2e/cli tests/workload
)

# seed_sweep: the seed's module for every swept source it compiles, in
# $TMP/seed-sweep; a source it rejects (1) leaves none, and anything else
# fails, as in sweep.

seed_sweep() (
  export JELLOC_JLO="$SEED"
  each_source "$TMP/seed-sweep" 'new_jelloc "$1" --std ../stdlib --out "$o.jlo" > /dev/null 2>&1 || [[ $? -eq 1 ]]' \
    itests tests/e2e/programs tests/e2e/cli tests/workload
)

for stage in build/jelloc.jlo build/jelloc1.jlo; do
  [[ -f "$stage" ]] || fail "missing $stage (run ./scripts/build_jello.sh)"
done
if cmp -s build/jelloc.jlo build/jelloc1.jlo; then sweeps=(build/jelloc1.jlo); else sweeps=(build/jelloc.jlo build/jelloc1.jlo); fi
pids=()
for stage in "${sweeps[@]}"; do sweep "$stage" & pids+=($!); done
seed_pid=""
if [[ -z "${PROMOTING:-}" ]] && ! cmp -s "$SEED" build/jelloc1.jlo; then seed_sweep & seed_pid=$!; fi
JELLOC_JLO=build/jelloc1.jlo OUT=build/jelloc2.jlo scripts/self-compile.sh >/dev/null || fail "stage 1 cannot compile src/Main.jello"
LISTING= JELLOC_JLO=build/jelloc2.jlo OUT=build/jelloc3.jlo scripts/self-compile.sh >/dev/null || fail "stage 2 cannot compile src/Main.jello"
cmp -s build/jelloc1.jlo build/jelloc2.jlo || fail "stage 2 differs from stage 1: stage 0's build and stage 1 compile src/Main.jello differently"
cmp -s build/jelloc2.jlo build/jelloc3.jlo || fail "stage 3 differs from stage 2: the compiler is not deterministic"
echo "== fixed point: jelloc1 = jelloc2 = jelloc3 =="
if cmp -s "$SEED" build/jelloc1.jlo; then echo "== the seed is stage 1 =="; else echo "== the seed is older than stage 1 (scripts/promote-seed.sh) =="; fi

for i in "${!sweeps[@]}"; do wait "${pids[$i]}" || fail "the sweep under ${sweeps[$i]} failed"; done
count="$(find "$TMP/jelloc1-sweep" -name '*.hir' | wc -l)"
compiled="$(find "$TMP/jelloc1-sweep" -name '*.jlo' | wc -l)"
if [[ ${#sweeps[@]} -eq 1 ]]; then
  printf '== stage 1 runs on %d sources (%d compile); stage 0'\''s build is stage 1 ==\n' "$count" "$compiled"
else
  differences="$(cd "$TMP" && diff -r jelloc-sweep jelloc1-sweep)" ||
    fail "stage 0's build and stage 1 disagree:"$'\n'"$(head -40 <<< "$differences")"
  printf '== stage 0'\''s build and stage 1 agree on %d sources (%d compile) ==\n' "$count" "$compiled"
fi

# New compiler emits the seed's modules.

if [[ -n "$seed_pid" ]]; then
  wait "$seed_pid" || fail "the seed's sweep failed"
  differences=()
  while IFS= read -r module; do
    source="${module#"$TMP/seed-sweep/"}"
    if [[ ! -f "$TMP/jelloc1-sweep/$source" ]]; then differences+=("${source%.jlo}.jello: rejected, but the seed compiles it")
    elif ! cmp -s "$module" "$TMP/jelloc1-sweep/$source"; then differences+=("${source%.jlo}.jello: other bytes"); fi
  done < <(find "$TMP/seed-sweep" -name '*.jlo' | sort)
  [[ ${#differences[@]} -eq 0 ]] ||
    fail "stage 1 emits other modules than the seed:"$'\n'"$(printf '%s\n' "${differences[@]}" | head -40)"
  echo "== stage 1 emits the seed's modules =="
fi
