#!/usr/bin/env bash
# The process host contract: jello passes everything after the module path
# to the program (args_count/args_get), and process_exit sets its status.
set -euo pipefail

JLO_TOOL="${1:?usage: process_fixtures.sh <jlo_tool> <jello>}"
JELLOVM="${2:?usage: process_fixtures.sh <jlo_tool> <jello>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STDDIR="${ROOT}/tests/bytecode/std"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "process_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

# check NAME EXPECTED_STATUS EXPECTED_OUTPUT FLAG ARGS...
check() {
  local name="$1" want_status="$2" want="$3" flag="$4" out status
  shift 4
  out="$("${JELLOVM}" ${flag} "${TMP}/${name}.jlo" "$@" 2>&1)" && status=0 || status=$?
  if [[ "${status}" != "${want_status}" || "${out}" != "${want}" ]]; then
    fail "${name} ${flag:-jit} $*: expected status ${want_status} [${want}], got ${status} [${out}]"
  fi
}

for name in process_args process_exit; do
  "${JLO_TOOL}" asm "${STDDIR}/${name}.jlo.txt" -o "${TMP}/${name}.jlo"
done
for flag in "" --no-jit --require-jit; do
  # The VM's own flags after the module path are the program's.
  check process_args 0 "last arg" "${flag}" --no-jit "last arg"
  check process_args 1 "jello: trap TYPE_MISMATCH in function 0 at pc 3
jello: native args_get failed: index out of range" "${flag}"
  check process_exit 3 "" "${flag}"
done

if [[ ${failures} -gt 0 ]]; then
  echo "process_fixtures: ${failures} failure(s)" >&2
  exit 1
fi
echo "process_fixtures: passed"
