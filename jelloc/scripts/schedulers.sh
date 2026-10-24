#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# scripts/schedulers.sh: the program fixtures whose output no order of running
# changes, marked `// schedulers: N`, compiled with the compiler under test
# (env.sh) and run ROUNDS times (default 5) on N schedulers in every mode, as
# the tiers run every fixture on one. Each must print what its `// expect:`
# lines say, within a minute. On a VM built with -fsanitize=thread
# (JELLO_VM_BUILD_DIR), a race it reports fails the run too.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$COMPILER_DIR"
ROUNDS="${ROUNDS:-5}"
TMP="$(mktemp -d)"
trap 'kill_jobs; rm -rf "$TMP"' EXIT

count=0
for source in tests/e2e/programs/processes/*.jello tests/e2e/programs/net/*.jello; do
  n="$(sed -n 's|^// schedulers: ||p' "$source")"
  [[ -n "$n" ]] || continue
  name="$(basename "$source" .jello)"
  out="$(new_jelloc "$source" --std ../stdlib --out "$TMP/$name.jlo" 2>&1)" || fail "$source does not compile: $out"
  want="$(sed -n 's|^// expect: ||p' "$source")"
  for mode in jit no-jit ${REQUIRE_JIT:+eager}; do
    round=0
    while [[ $round -lt $ROUNDS ]]; do
      round=$((round + 1))
      got="$(JELLO_SCHEDULERS="$n" repo_vm_within 60 $(mode_flag "$mode") "$TMP/$name.jlo" 2>&1)" ||
        fail "$source ($mode, $n schedulers, round $round) failed: $got"
      expect "$got" "$want" "$source ($mode, $n schedulers, round $round)"
    done
  done
  count=$((count + 1))
done
[[ $count -gt 0 ]] || fail "no fixture is marked // schedulers:"
echo "== $count fixtures on many schedulers =="
