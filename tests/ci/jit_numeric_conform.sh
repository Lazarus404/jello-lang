#!/usr/bin/env bash
set -euo pipefail

USAGE="usage: jit_numeric_conform.sh <path-to-jlo_tool> <path-to-jit_numeric_run> <path-to-jello>"
JLO_TOOL="${1:?$USAGE}"
JIT_RUN="${2:?$USAGE}"
JELLOVM="${3:?$USAGE}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/interp"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# What the JIT must do is required of a VM built for a JIT architecture: the
# VM's architecture, not the shell's, since under Rosetta an x86_64 VM runs from
# an arm64 shell.
# (Linux's file prints aarch64 and x86-64.)
JIT_VM=""
case "$(file -b "${JELLOVM}")" in
  *arm64* | *aarch64* | *x86_64* | *x86-64*) JIT_VM=1 ;;
esac

failures=0
mixed_entered=0
mixed_bridged=0
fail() {
  echo "jit_numeric_conform FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== JIT numeric/cfg conform: interp fixtures (incl. jmp / call; skip stack_overflow) =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  if [[ "${base}" == "stack_overflow" ]]; then
    echo "jit_numeric_conform: ${base} -> SKIPPED (stack overflow trap)"
    continue
  fi
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
  expected="$(env JELLO_JIT_DISABLE=1 "${JLO_TOOL}" run "${bin}" 2>"${TMP}/${base}.interp.err" || true)"
  # jello prints where a trap happened; the JIT (mixed where the module
  # needs it) must report the same function and pc as the interpreter.
  with_jit="$(env -u JELLO_JIT_DISABLE "${JELLOVM}" "${bin}" 2>&1 || true)"
  without_jit="$("${JELLOVM}" --no-jit "${bin}" 2>&1 || true)"
  if [[ "${with_jit}" != "${without_jit}" ]]; then
    fail "${base} (jello): --no-jit printed [${without_jit}], the JIT [${with_jit}]"
  fi
  if [[ "${base}" == mixed_* ]]; then
    report="$(env -u JELLO_JIT_DISABLE "${JELLOVM}" --jit-report "${bin}" 2>&1 | grep -a 'entered' || true)"
    [[ "${report}" =~ entered\ ([0-9]+)\ times,\ bridged\ ([0-9]+) ]] || fail "${base}: no --jit-report line: ${report}"
    mixed_entered=$((mixed_entered + ${BASH_REMATCH[1]:-0}))
    mixed_bridged=$((mixed_bridged + ${BASH_REMATCH[2]:-0}))
  fi
  # Mixed mode runs what compiled and interprets the rest; it must agree too.
  mixed="$("${JIT_RUN}" --mixed "${bin}" 2>"${TMP}/${base}.mixed.err" || true)"
  # mixed_* fixtures exist to test crossings; the JIT must run them mixed.
  if [[ "${mixed}" == "JIT_SKIP" && "${base}" == mixed_* && -n "${JIT_VM}" ]]; then
    fail "${base}: the JIT compiled none of it"
  elif [[ "${mixed}" != "JIT_SKIP" && "${mixed}" != "${expected}" ]]; then
    fail "${base} (mixed): expected [${expected}] got [${mixed}] jit=$(cat "${TMP}/${base}.mixed.err")"
  fi
  actual="$("${JIT_RUN}" "${bin}" 2>"${TMP}/${base}.jit.err" || true)"
  # ... and each must decline something (a SWITCH_KIND marker), or it never crosses.
  if [[ "${actual}" != "JIT_SKIP" && "${base}" == mixed_* && -n "${JIT_VM}" ]]; then
    fail "${base}: the JIT compiled all of it; its decline marker no longer declines"
  fi
  if [[ "${actual}" == "JIT_SKIP" ]]; then
    # jit_* fixtures exist to test JIT code; the JIT must compile them.
    if [[ "${base}" == jit_* && -n "${JIT_VM}" ]]; then
      fail "${base}: the JIT declined it"
    else
      echo "jit_numeric_conform: ${base} -> JIT_SKIP (not JIT-compilable yet)"
    fi
    continue
  fi
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${base}: expected [${expected}] got [${actual}] interp=$(cat "${TMP}/${base}.interp.err") jit=$(cat "${TMP}/${base}.jit.err")"
  else
    echo "jit_numeric_conform: ${base} -> ${actual}"
  fi
done

# The mixed_* fixtures must cross in both directions, or they test nothing.
if [[ -n "${JIT_VM}" ]]; then
  [[ ${mixed_entered} -gt 0 ]] || fail "mixed_* fixtures never entered JIT code from the interpreter"
  [[ ${mixed_bridged} -gt 0 ]] || fail "mixed_* fixtures never bridged from JIT code to the interpreter"
  echo "jit_numeric_conform: mixed_* fixtures entered ${mixed_entered} times, bridged ${mixed_bridged} times"
fi

if [[ ${failures} -gt 0 ]]; then
  echo "jit_conform: ${failures} failure(s)" >&2
  exit 1
fi

echo "jit_conform: passed"
