#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: interp_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/interp"
BENCHDIR="${ROOT}/tests/bytecode/bench"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "interp_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

# Run BIN once with the JIT and once interpreter-only. Both must print EXPECTED.
check_both() {
  local name="$1" bin="$2" expected="$3" actual mode
  for mode in jit interp; do
    if [[ "${mode}" == interp ]]; then
      actual="$(JELLO_JIT_DISABLE=1 "${JLO_TOOL}" run "${bin}" 2>"${TMP}/${name}.run.err" || true)"
    else
      actual="$(env -u JELLO_JIT_DISABLE "${JLO_TOOL}" run "${bin}" 2>"${TMP}/${name}.run.err" || true)"
    fi
    if [[ "${actual}" != "${expected}" ]]; then
      fail "${name} (${mode}): expected [${expected}] got [${actual}] $(cat "${TMP}/${name}.run.err")"
      return
    fi
  done
  echo "interp_fixtures: ${name} -> ${actual}"
}

echo "== correctness fixtures: every typed numeric opcode =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  expect_file="${FIXDIR}/${base}.expect"
  if [[ ! -f "${expect_file}" ]]; then
    fail "${base}: no .expect file"
    continue
  fi
  bin="${TMP}/${base}.jlo"
  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${TMP}/${base}.asm.err"; then
    fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"
    continue
  fi
  check_both "${base}" "${bin}" "$(cat "${expect_file}")"
done

echo "== benchmark fixtures: fib/ackerman/tak/deep_call/pi/gc_churn (tests/bytecode/bench/) =="
for name in fib ackerman tak deep_call pi gc_churn; do
  fixture="${BENCHDIR}/${name}.jlo.txt"
  bin="${TMP}/${name}.jlo"
  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${TMP}/${name}.asm.err"; then
    fail "${name}: asm failed -- $(cat "${TMP}/${name}.asm.err")"
    continue
  fi
  case "${name}" in
    fib) expected="RESULT kind=I32 value=46368" ;;
    ackerman) expected="RESULT kind=I32 value=509" ;;
    tak) expected="RESULT kind=I32 value=7" ;;
    deep_call) expected="RESULT kind=I32 value=50000" ;;
    pi) expected="RESULT kind=F64 value=3.1415936535887745" ;;
    gc_churn) expected="RESULT kind=I32 value=600030000" ;;
  esac
  check_both "${name}" "${bin}" "${expected}"
done

if [[ ${failures} -gt 0 ]]; then
  echo "interpreter_core: ${failures} failure(s)" >&2
  exit 1
fi

echo "interpreter_core: passed"
