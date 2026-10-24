#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: bytecode_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode"
CORRUPTDIR="${FIXDIR}/corrupt"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "bytecode_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

roundtrip_one() {
  local fixture="$1"
  local base
  base="$(basename "${fixture}" .jlo.txt)"
  local bin="${TMP}/${base}.jlo"
  local out="${TMP}/${base}.roundtrip.txt"

  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}"; then
    fail "${base}: asm failed"
    return
  fi
  if ! "${JLO_TOOL}" disasm "${bin}" -o "${out}"; then
    fail "${base}: disasm failed"
    return
  fi
  if ! diff -u "${fixture}" "${out}" > "${TMP}/${base}.diff"; then
    fail "${base}: round-trip mismatch (see below)"
    cat "${TMP}/${base}.diff" >&2
  else
    echo "bytecode_fixtures: ${base} round-trips byte-identically"
  fi
}

echo "== accept fixtures: structural + semantic round-trip =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  roundtrip_one "${fixture}"
done

echo "== comments: a line starting with # is skipped, so the module is the same =="
{ echo "# expect: 3"; sed 's/^  0: /# between instructions\n  0: /' "${FIXDIR}/numeric_i32.jlo.txt"; } > "${TMP}/commented.jlo.txt"
if grep -q "^# between" "${TMP}/commented.jlo.txt" &&
   "${JLO_TOOL}" asm "${TMP}/commented.jlo.txt" -o "${TMP}/commented.jlo" &&
   "${JLO_TOOL}" asm "${FIXDIR}/numeric_i32.jlo.txt" -o "${TMP}/uncommented.jlo" &&
   cmp -s "${TMP}/commented.jlo" "${TMP}/uncommented.jlo"; then
  echo "bytecode_fixtures: comments assemble to the uncommented module"
else
  fail "comments: a commented fixture did not assemble to the uncommented module"
fi

echo "== compressed_conformance: FULL vs COMPACT decode identically =="
full_out="${TMP}/full_disasm.txt"
compact_out="${TMP}/compact_disasm.txt"
"${JLO_TOOL}" asm "${FIXDIR}/compressed_conformance.jlo.txt" -o "${TMP}/full.jlo"
"${JLO_TOOL}" disasm "${TMP}/full.jlo" -o "${full_out}"
"${JLO_TOOL}" asm "${FIXDIR}/compressed_conformance_compact.jlo.txt" -o "${TMP}/compact.jlo"
"${JLO_TOOL}" disasm "${TMP}/compact.jlo" -o "${compact_out}"
if ! diff -u <(sed -n '/^func\[/,$p' "${full_out}") <(sed -n '/^func\[/,$p' "${compact_out}") > "${TMP}/compressed.diff"; then
  fail "compressed_conformance: FULL and COMPACT encodings decoded differently"
  cat "${TMP}/compressed.diff" >&2
else
  echo "bytecode_fixtures: compressed_conformance decodes identically FULL vs COMPACT"
fi

echo "== corrupt binary fixtures: must be rejected, never crash (structural) =="
"${JLO_TOOL}" asm "${FIXDIR}/minimal_valid.jlo.txt" -o "${TMP}/valid.jlo"

valid_size=$(wc -c < "${TMP}/valid.jlo")
truncated_size=$((valid_size / 2))
head -c "${truncated_size}" "${TMP}/valid.jlo" > "${TMP}/truncated.jlo"
if "${JLO_TOOL}" disasm "${TMP}/truncated.jlo" -o "${TMP}/truncated_out.txt" 2>"${TMP}/truncated.err"; then
  fail "truncated.jlo: expected rejection, tool accepted it"
else
  echo "bytecode_fixtures: truncated.jlo correctly rejected ($(cat "${TMP}/truncated.err"))"
fi

cp "${TMP}/valid.jlo" "${TMP}/bad_magic.jlo"
printf '\x00\x00\x00\x00' | dd of="${TMP}/bad_magic.jlo" bs=1 seek=0 count=4 conv=notrunc status=none
if "${JLO_TOOL}" disasm "${TMP}/bad_magic.jlo" -o "${TMP}/bad_magic_out.txt" 2>"${TMP}/bad_magic.err"; then
  fail "bad_magic.jlo: expected rejection, tool accepted it"
else
  echo "bytecode_fixtures: bad_magic.jlo correctly rejected ($(cat "${TMP}/bad_magic.err"))"
fi

{ cat "${TMP}/valid.jlo"; printf '\x00'; } > "${TMP}/trailing.jlo"
if "${JLO_TOOL}" disasm "${TMP}/trailing.jlo" -o "${TMP}/trailing_out.txt" 2>"${TMP}/trailing.err"; then
  fail "trailing.jlo: expected rejection, tool accepted it"
else
  echo "bytecode_fixtures: trailing.jlo correctly rejected ($(cat "${TMP}/trailing.err"))"
fi

# Line tables the text format cannot spell: an empty path, a path with a NUL, a
# six-byte varint run count, and a file that ends inside the last varint.
"${JLO_TOOL}" asm "${FIXDIR}/line_table_accept.jlo.txt" -o "${TMP}/lines.jlo"
perl -0777 -pe 's/\x0a\x00\x00\x00main\.jello/\x00\x00\x00\x00/' "${TMP}/lines.jlo" > "${TMP}/line_table_empty_path.jlo"
perl -0777 -pe 's/main\.jello/ma\x00n.jello/' "${TMP}/lines.jlo" > "${TMP}/line_table_nul_path.jlo"
perl -0777 -pe 's/util\.jello\x03/util.jello\x83\x80\x80\x80\x80\x00/' "${TMP}/lines.jlo" > "${TMP}/line_table_long_varint.jlo"
perl -0777 -pe 's/\x05\z/\x85/' "${TMP}/lines.jlo" > "${TMP}/line_table_cut_varint.jlo"
for bad in line_table_empty_path line_table_nul_path line_table_long_varint line_table_cut_varint; do
  if cmp -s "${TMP}/lines.jlo" "${TMP}/${bad}.jlo"; then
    fail "${bad}.jlo: the patch did not apply"
  elif "${JLO_TOOL}" disasm "${TMP}/${bad}.jlo" -o "${TMP}/${bad}_out.txt" 2>"${TMP}/${bad}.err"; then
    fail "${bad}.jlo: expected rejection, tool accepted it"
  else
    echo "bytecode_fixtures: ${bad}.jlo correctly rejected ($(cat "${TMP}/${bad}.err"))"
  fi
done

echo "== corrupt semantic fixtures: structurally valid, semantically invalid =="
for fixture in "${CORRUPTDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  bin="${TMP}/corrupt_${base}.jlo"
  out="${TMP}/corrupt_${base}_out.txt"
  err="${TMP}/corrupt_${base}.err"

  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${err}"; then
    fail "${base}: expected asm to succeed (fixture is structurally valid; only semantics are wrong) -- $(cat "${err}")"
    continue
  fi
  # BASE.expect, when present, holds text the rejection must contain, so a
  # fixture cannot pass by tripping an unrelated check.
  if "${JLO_TOOL}" disasm "${bin}" -o "${out}" 2>"${err}"; then
    fail "${base}: expected disasm to reject this module, tool accepted it"
  elif [[ -f "${CORRUPTDIR}/${base}.expect" && "$(cat "${err}")" != *"$(cat "${CORRUPTDIR}/${base}.expect")"* ]]; then
    fail "${base}: rejected for the wrong reason: $(cat "${err}")"
  else
    echo "bytecode_fixtures: ${base} correctly rejected ($(cat "${err}"))"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "bytecode_and_validation: ${failures} failure(s)" >&2
  exit 1
fi

echo "bytecode_and_validation: passed"
