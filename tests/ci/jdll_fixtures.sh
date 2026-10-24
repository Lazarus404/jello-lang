#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: jdll_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/jdll"
CORRUPTDIR="${FIXDIR}/corrupt"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

: "${JELLO_JDLL_PATH:?JELLO_JDLL_PATH must point at the directory containing test_native.jdll (test_vm.sh sets it)}"

failures=0
fail() {
  echo "jdll_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== round-trip fixtures: JOP_CALL_NATIVE via test_native.jdll =="
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
  actual="$("${JLO_TOOL}" run "${bin}" 2>"${TMP}/${base}.run.err" || true)"
  expected="$(cat "${expect_file}")"
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${base}: expected [${expected}] got [${actual}] $(cat "${TMP}/${base}.run.err")"
  else
    echo "jdll_fixtures: ${base} -> ${actual}"
  fi
done

# Runs a command in its own process group for at most $1 seconds, then kills the whole group
# (leaks and the run it launched) and exits 124.
within() {
  perl -e '
    my $secs = shift;
    my $pid = fork // die "fork: $!";
    if (!$pid) { setpgrp(0, 0); exec @ARGV or exit 127 }
    $SIG{ALRM} = sub { kill "KILL", -$pid; exit 124 };
    alarm $secs;
    waitpid $pid, 0;
    exit($? & 127 ? 128 + ($? & 127) : $? >> 8);
  ' "$@"
}

echo "== replaced or abandoned native results leak nothing (macOS leaks, where it can inspect) =="
if command -v leaks >/dev/null 2>&1; then
  inspected=0
  for base in bytes_then_fail bytes_twice bytes_wrong_kind handle_then_i32; do
    # leaks waits forever where it can't get permission to inspect (as on CI runners). A first
    # run that times out means that, so the check is skipped; a later one means leaks is
    # merely slow, which fails. The report goes to a file, never a pipe a run could hold open.
    status=0
    within 60 leaks --atExit -- "${JLO_TOOL}" run "${TMP}/${base}.jlo" \
      </dev/null >"${TMP}/${base}.leaks" 2>&1 || status=$?
    report="$(<"${TMP}/${base}.leaks")"
    if [[ "${status}" == 124 && "${inspected}" == 0 ]]; then
      echo "jdll_fixtures: leaks timed out on ${base}, so it can't inspect here; skipped"
      break
    elif [[ "${status}" == 124 ]]; then
      fail "${base}: leaks timed out after inspecting ${inspected} runs"
    elif [[ "${report}" != *" for "*" total leaked bytes."* ]]; then
      fail "${base}: leaks gave no report (status ${status}): ${report}"
    elif [[ "${report}" != *": 0 leaks for 0 total leaked bytes."* ]]; then
      fail "${base}: leaked: $(printf '%s\n' "${report}" | grep -E 'leaks? for|ROOT LEAK' || true)"
    else
      inspected=$((inspected + 1))
      echo "jdll_fixtures: ${base} leaks nothing"
    fi
  done
fi

echo "== corrupt fixtures: declared import signature must match the real .jdll =="
for fixture in "${CORRUPTDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  bin="${TMP}/corrupt_${base}.jlo"
  err="${TMP}/corrupt_${base}.err"

  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${err}"; then
    fail "${base}: expected asm to succeed (fixture is structurally valid; only the .jdll resolution is wrong) -- $(cat "${err}")"
    continue
  fi
  if "${JLO_TOOL}" run "${bin}" >"${TMP}/corrupt_${base}.out" 2>"${err}"; then
    fail "${base}: expected load-time rejection, tool accepted and ran it"
  else
    echo "jdll_fixtures: ${base} correctly rejected ($(cat "${err}"))"
  fi
done

echo "== std JDLL: regexp natives and native handles, with and without the JIT =="
# File I/O, get_env and argv are covered by tests/csrc/thread_tests.c.
STDDIR="${ROOT}/tests/bytecode/std"
for base in regexp_prefix regexp_wrong_class regexp_handles_gc; do
  bin="${TMP}/std_${base}.jlo"
  if ! "${JLO_TOOL}" asm "${STDDIR}/${base}.jlo.txt" -o "${bin}" 2>"${TMP}/std_${base}.err"; then
    fail "std ${base}: asm failed -- $(cat "${TMP}/std_${base}.err")"
    continue
  fi
  expected="$(cat "${STDDIR}/${base}.expect")"
  for jit_off in "" 1; do
    out="$(JELLO_JIT_DISABLE="${jit_off}" "${JLO_TOOL}" run "${bin}" --gc-stats 2>&1 || true)"
    actual="$(printf '%s\n' "${out}" | head -1)"
    if [[ "${actual}" != "${expected}" ]]; then fail "std ${base} (jit off=${jit_off:-0}): expected [${expected}] got [${out}]"; fi
    if [[ "${base}" == regexp_handles_gc ]]; then
      finalized="$(printf '%s\n' "${out}" | awk '/^GC_FINALIZED_HANDLES/ {print $2}')"
      if [[ -z "${finalized}" || "${finalized}" -lt 10000 ]]; then
        fail "std ${base}: expected the collector to finalize dead regexps, got GC_FINALIZED_HANDLES=${finalized:-<missing>}"
      fi
    fi
  done
  echo "jdll_fixtures: std ${base} -> ${expected}"
done

if [[ ${failures} -gt 0 ]]; then
  echo "jdll_and_std: ${failures} failure(s)" >&2
  exit 1
fi

echo "jdll_and_std: passed"
