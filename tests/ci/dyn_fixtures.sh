#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: dyn_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/dyn"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "dyn_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== correctness fixtures: Dynamic boundary, Struct, Object =="
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
  actual_line="$(printf '%s\n' "${actual}" | head -1)"
  expected="$(cat "${expect_file}")"
  expected_line="$(printf '%s\n' "${expected}" | head -1)"
  if [[ "${actual_line}" != "${expected_line}" ]]; then
    fail "${base}: expected [${expected_line}] got [${actual_line}] $(cat "${TMP}/${base}.run.err")"
  else
    echo "dyn_fixtures: ${base} -> ${actual_line}"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "dynamic_and_objects: ${failures} failure(s)" >&2
  exit 1
fi

echo "dynamic_and_objects: passed"
