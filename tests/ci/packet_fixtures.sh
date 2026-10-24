#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: packet_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/packet"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "packet_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== correctness/trap fixtures: Bytes memory-layout ops =="
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
    echo "packet_fixtures: ${base} -> ${actual}"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "bytes_layout: ${failures} failure(s)" >&2
  exit 1
fi

echo "bytes_layout: passed"
