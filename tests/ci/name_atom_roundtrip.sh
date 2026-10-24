#!/usr/bin/env bash
set -euo pipefail

# name_atom is additive-only. This proves both halves of that claim with the
# real jlo_tool (assembler, disassembler, runner), not just a unit test against
# the loader's own internal struct: a function *with* `name=` round-trips its
# name through the atoms table correctly and still executes, and a function
# *without* one (every fixture checked in before name atoms existed) keeps
# parsing and running exactly as it did before.

JLO_TOOL="${1:?usage: name_atom_roundtrip.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "name_atom_roundtrip FAIL: $*" >&2
  failures=$((failures + 1))
}

NAMED_SRC="${ROOT}/tests/bytecode/named_functions.jlo.txt"
NAMED_BIN="${TMP}/named_functions.jlo"
if ! "${JLO_TOOL}" asm "${NAMED_SRC}" -o "${NAMED_BIN}" 2>"${TMP}/named.asm.err"; then
  fail "named_functions: asm failed -- $(cat "${TMP}/named.asm.err")"
else
  disasm="$("${JLO_TOOL}" disasm "${NAMED_BIN}")"
  if ! grep -q "func\[0\] name=update " <<<"${disasm}"; then
    fail "named_functions: func[0]'s own name did not round-trip -- got: ${disasm}"
  elif ! grep -q "func\[1\] name=helper " <<<"${disasm}"; then
    fail "named_functions: func[1]'s own name did not round-trip -- got: ${disasm}"
  else
    echo "name_atom_roundtrip: named_functions -- both names round-tripped"
  fi
  run_out="$("${JLO_TOOL}" run "${NAMED_BIN}")"
  run_line="$(printf '%s\n' "${run_out}" | head -1)"
  if [[ "${run_line}" != "RESULT kind=I32 value=42" ]]; then
    fail "named_functions: expected [RESULT kind=I32 value=42] got [${run_line}]"
  else
    echo "name_atom_roundtrip: named_functions -- ${run_line}"
  fi
fi

# An existing, unnamed-function fixture (checked in long before name
# atoms) must keep disassembling with no `name=` attribute at all --
# additive means the old shape is still exactly the old shape, not merely
# "still parses."
UNNAMED_SRC="${ROOT}/tests/bytecode/bench/fib.jlo.txt"
UNNAMED_BIN="${TMP}/fib.jlo"
if ! "${JLO_TOOL}" asm "${UNNAMED_SRC}" -o "${UNNAMED_BIN}" 2>"${TMP}/unnamed.asm.err"; then
  fail "fib (unnamed): asm failed -- $(cat "${TMP}/unnamed.asm.err")"
else
  disasm="$("${JLO_TOOL}" disasm "${UNNAMED_BIN}")"
  if grep -q "name=" <<<"${disasm}"; then
    fail "fib (unnamed): expected no name= attribute anywhere -- got: ${disasm}"
  else
    echo "name_atom_roundtrip: fib (unnamed) -- no name= attribute, as expected"
  fi
fi

if [[ ${failures} -gt 0 ]]; then
  echo "name_atom_roundtrip: ${failures} failure(s)" >&2
  exit 1
fi

echo "name_atom_roundtrip: passed"
