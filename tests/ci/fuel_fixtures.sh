#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: fuel_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/fuel"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "fuel_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

assemble() {
  local base="$1"
  "${JLO_TOOL}" asm "${FIXDIR}/${base}.jlo.txt" -o "${TMP}/${base}.jlo" 2>"${TMP}/${base}.asm.err" \
    || { fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"; return 1; }
  return 0
}

# Runs jlo_tool under a 10-second alarm, so a path that never charges fuel fails rather than
# hangs. $1 is JELLO_JIT_DISABLE: 1 for the interpreter alone, empty to let the JIT compile.
run_bounded() {
  local jit_off="$1"
  shift
  JELLO_JIT_DISABLE="${jit_off}" perl -e 'alarm shift; exec @ARGV' 10 "${JLO_TOOL}" run "$@" \
    2>"${TMP}/run.err" || true
}

# Fuel is charged at every possible loop back-edge: a backward jump, a call or tail call (before
# it replaces the frame), and a THROW (a handler may sit before it). Each fixture, run in the
# interpreter and under the JIT, must give its exact result with no fuel limit, and with
# --fuel 10 trap FUEL at the same charging instruction in both modes.
echo "== fuel: every loop back-edge charges, interpreted and compiled =="
check_fuel() {
  local base="$1" want="$2" want_func="$3" want_pc="$4"
  assemble "${base}" || return 0
  local jit_off out pc_line
  for jit_off in 1 ""; do
    out="$(run_bounded "${jit_off}" "${TMP}/${base}.jlo" | head -1)"
    if [[ "${out}" != "RESULT kind=I32 value=${want}" ]]; then
      fail "${base} (no fuel, JELLO_JIT_DISABLE=${jit_off}): expected [RESULT kind=I32 value=${want}] got [${out}] $(cat "${TMP}/run.err")"
    fi
    { read -r out || true; read -r pc_line || true; } < <(run_bounded "${jit_off}" "${TMP}/${base}.jlo" --fuel 10 --trap-pc)
    if [[ "${out}" != "TRAP code=FUEL" ]]; then
      fail "${base} (--fuel 10, JELLO_JIT_DISABLE=${jit_off}): expected [TRAP code=FUEL] got [${out}] $(cat "${TMP}/run.err")"
    elif [[ "${pc_line}" != "TRAP_PC func_index=${want_func} pc=${want_pc}" ]]; then
      fail "${base} (--fuel 10, JELLO_JIT_DISABLE=${jit_off}): expected [TRAP_PC func_index=${want_func} pc=${want_pc}] got [${pc_line}]"
    else
      echo "fuel_fixtures: ${base} (--fuel 10, JELLO_JIT_DISABLE=${jit_off}) -> ${out} ${pc_line}"
    fi
  done
}
check_fuel loop_sum 4950 0 7
# A backward JMP_IF that is never taken still charges, in both modes.
check_fuel jmp_if_untaken 4950 0 8
check_fuel recursive_countdown 1275 1 3
check_fuel tailcall_loop 500500 1 4
check_fuel tailcallr_loop 500500 1 5
check_fuel callr_recursion 5050 1 4
check_fuel throw_loop 5050 0 8

echo "== max_bytes_len: disabled by default, enforced once set =="
if assemble "bytes_over_limit"; then
  # Also a regression fixture for a JIT gap: emit_store_out (both backends)
  # once had no case for a top-level RET of a pointer kind
  # (Bytes/Struct/Object/Dynamic), silently returning kind=VOID instead of
  # kind=PTR. Verified here directly.
  line="$("${JLO_TOOL}" run "${TMP}/bytes_over_limit.jlo" 2>"${TMP}/bol.run.err" | head -1 || true)"
  if [[ "${line}" != "RESULT kind=PTR value=NON_NULL" ]]; then
    fail "bytes_over_limit (no limit): expected [RESULT kind=PTR value=NON_NULL] got [${line}] $(cat "${TMP}/bol.run.err")"
  else
    echo "fuel_fixtures: bytes_over_limit (no limit) -> ${line}"
  fi
  line="$("${JLO_TOOL}" run "${TMP}/bytes_over_limit.jlo" --max-bytes-len 100 2>"${TMP}/bol.limit.err" | head -1 || true)"
  if [[ "${line}" != "TRAP code=LIMIT" ]]; then
    fail "bytes_over_limit (--max-bytes-len 100): expected [TRAP code=LIMIT] got [${line}] $(cat "${TMP}/bol.limit.err")"
  else
    echo "fuel_fixtures: bytes_over_limit (--max-bytes-len 100) -> ${line}"
  fi
fi

if [[ ${failures} -gt 0 ]]; then
  echo "fuel_and_limits: ${failures} failure(s)" >&2
  exit 1
fi

echo "fuel_and_limits: passed"
