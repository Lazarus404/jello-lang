#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# scripts/test.sh [TIER...] [-- ARGS...]: builds the test binary of each TIER
# under tests/ (unit, integration, e2e; all of them by default) with the
# compiler under test, stage 1 unless JELLOC_JLO says otherwise (env.sh), and
# runs it from the compiler's root with ARGS (`-- -v --run frontend/syntax/Lexer`).
# The binaries build at once, and each runs, in order, once built; the first
# that fails stops the script. As `go test ./...` does, it finds each tier's
# *.test.jello files, since the compiler takes files, but not the fixtures in
# tests/e2e/programs and tests/e2e/cli, which the e2e tests compile themselves.
# The e2e tests also get, after --, the compiler under test, the VM, the
# standard library, the decode check and the modes (degust.Programs), and, when
# FIXED_POINT says the compiler is its own build, it as --self (support.Run).
# First, jlo_tool get-deps puts the dependencies jello.json names, degust among
# them, under packages/.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${JELLOC_JLO:=$ROOT/build/jelloc1.jlo}"
. "$ROOT/scripts/lib.sh"
cd "$ROOT"

# The fixtures expect the order one scheduler runs processes in
# (scripts/schedulers.sh runs those that any order prints alike on many).

export JELLO_SCHEDULERS=1

tiers=()
while [[ $# -gt 0 && "$1" != "--" ]]; do
  case "$1" in unit|integration|e2e) tiers+=("$1") ;; *) echo "usage: $0 [unit|integration|e2e]... [-- ARGS...]" >&2; exit 2 ;; esac
  shift
done
[[ $# -eq 0 ]] || shift
if [[ ${#tiers[@]} -eq 0 ]]; then
  for tier in unit integration e2e; do if [[ -d "tests/$tier" ]]; then tiers+=("$tier"); fi; done
fi
[[ -f "$JELLOC_JLO" ]] || fail "missing $JELLOC_JLO (run ./scripts/build_jello.sh)"
deps="$(repo_jlo_tool get-deps 2>&1)" || fail "jlo_tool get-deps: $deps"

TMP="$(mktemp -d)"
trap 'kill_jobs; rm -rf "$TMP"' EXIT
pids=()
for tier in "${tiers[@]}"; do
  # A tier's tests see its own tree, the compiler's sources, degust and the
  # workload's Digest (tests/workload).
  files=()
  while IFS= read -r file; do files+=("$file"); done < <(find "tests/$tier" \( -path tests/e2e/programs -o -path tests/e2e/cli \) -prune -o -name '*.test.jello' -print 2>/dev/null | sort)
  [[ ${#files[@]} -gt 0 ]] || fail "tests/$tier holds no *.test.jello file"
  new_jelloc --test "${files[@]}" --path "tests/$tier" --path src --path packages/degust/src --path tests/workload --std ../stdlib --out "$TMP/$tier.jlo" > "$TMP/$tier.out" 2>&1 &
  pids+=($!)
done
# Each tier runs once its binary is built, while the later ones still build.
for i in "${!tiers[@]}"; do
  tier="${tiers[$i]}"
  wait "${pids[$i]}" || fail "the $tier tests do not compile:"$'\n'"$(cat "$TMP/$tier.out")"
  args=(${1+"$@"})
  if [[ "$tier" == e2e ]]; then
    # Right after the first --, so the options given there override these.
    at=${#args[@]}
    for k in ${args[@]+"${!args[@]}"}; do if [[ "${args[$k]}" == -- ]]; then at=$k; break; fi; done
    args=(${args[@]+"${args[@]:0:$at}"} -- --compiler "$(jelloc_command)" ${FIXED_POINT:+--self "$JELLOC_JLO"} --vm "$VM" --std ../stdlib --check "$ROOT/scripts/decodes.sh"
      --modes "jit,no-jit${REQUIRE_JIT:+,eager}" --out "$TMP/e2e" ${args[@]+"${args[@]:$((at + 1))}"})
  fi
  printf '== %s ==\n' "$tier"
  repo_vm "$TMP/$tier.jlo" ${args[@]+"${args[@]}"} || fail "the $tier tests failed"
done
