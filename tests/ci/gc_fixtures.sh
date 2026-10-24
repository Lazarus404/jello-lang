#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: gc_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/gc"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "gc_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== correctness fixtures: Bytes opcodes, traps, root survival =="
for fixture in "${FIXDIR}"/bytes_new_len.jlo.txt "${FIXDIR}"/bytes_set_get_u8.jlo.txt \
               "${FIXDIR}"/bytes_null_deref_len.jlo.txt "${FIXDIR}"/bytes_null_deref_get.jlo.txt \
               "${FIXDIR}"/bytes_null_deref_set.jlo.txt "${FIXDIR}"/bytes_bounds_get.jlo.txt \
               "${FIXDIR}"/bytes_bounds_set_index.jlo.txt "${FIXDIR}"/bytes_bounds_set_value.jlo.txt \
               "${FIXDIR}"/bytes_call_arg.jlo.txt "${FIXDIR}"/bytes_gc_survives.jlo.txt \
               "${FIXDIR}"/global_gc_survives.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  expect_file="${FIXDIR}/${base}.expect"
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
    echo "gc_fixtures: ${base} -> ${actual_line}"
  fi
done

echo "== Bytes slices, array lengths and nulls: sub-binaries, null elements and checks, negative lengths, with and without the JIT =="
for base in bytes_slice_read bytes_slice_past_end bytes_slice_negative_start bytes_slice_gc \
    array_bytes_null_slice array_bytes_null_concat array_bytes_null_get array_new_negative array_resize_negative \
    const_null_bytes_len null_check_trap null_check_present is_null_reference is_null_dynamic_present \
    is_null_dynamic_null null_check_dynamic_trap array_dynamic_null_fill global_dynamic_null \
    is_enum_same_type is_enum_other_type is_enum_bytes is_enum_i32 is_enum_null; do
  bin="${TMP}/${base}.jlo"
  if ! "${JLO_TOOL}" asm "${FIXDIR}/${base}.jlo.txt" -o "${bin}" 2>"${TMP}/${base}.asm.err"; then
    fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"
    continue
  fi
  expected="$(head -1 "${FIXDIR}/${base}.expect")"
  for jit_off in "" 1; do
    out="$(JELLO_JIT_DISABLE="${jit_off}" "${JLO_TOOL}" run "${bin}" --gc-stats 2>&1 || true)"
    actual="$(printf '%s\n' "${out}" | head -1)"
    if [[ "${actual}" != "${expected}" ]]; then fail "${base} (jit off=${jit_off:-0}): expected [${expected}] got [${actual}]"; fi
  done
  echo "gc_fixtures: ${base} -> ${expected}"
done
# The parent of the slice is dropped; it must survive a real collection through the slice.
slice_collections="$("${JLO_TOOL}" run "${TMP}/bytes_slice_gc.jlo" --gc-stats 2>&1 | awk '/^GC_COLLECTIONS/ {print $2}')"
if [[ -z "${slice_collections}" || "${slice_collections}" -le 0 ]]; then
  fail "bytes_slice_gc: expected at least one collection, got GC_COLLECTIONS=${slice_collections:-<missing>}"
fi

echo "== allocation-churn stress fixture: reclaim proven via --gc-stats, not a fixed RESULT =="
churn_fixture="${FIXDIR}/bytes_gc_churn.jlo.txt"
churn_bin="${TMP}/bytes_gc_churn.jlo"
if ! "${JLO_TOOL}" asm "${churn_fixture}" -o "${churn_bin}" 2>"${TMP}/churn.asm.err"; then
  fail "bytes_gc_churn: asm failed -- $(cat "${TMP}/churn.asm.err")"
else
  out="$("${JLO_TOOL}" run "${churn_bin}" --gc-stats 2>"${TMP}/churn.run.err" || true)"
  echo "${out}"
  collections="$(echo "${out}" | awk '/^GC_COLLECTIONS/ {print $2}')"
  live_bytes="$(echo "${out}" | awk '/^GC_LIVE_BYTES/ {print $2}')"
  # 200000 iterations x a 64-byte request (rounded up to its size class) would
  # accumulate about 20MB if nothing were ever reclaimed; live bytes staying
  # under 4MB (GC_MIN_THRESHOLD in vm/src/gc/gc.c) with at least one real
  # collection is proof reclaim is working, not just that nothing crashed (a
  # preg_gc_safepoint-style fixture is beyond what this script can test).
  if [[ -z "${collections}" || "${collections}" -le 0 ]]; then
    fail "bytes_gc_churn: expected at least one real collection, got GC_COLLECTIONS=${collections:-<missing>}"
  elif [[ -z "${live_bytes}" || "${live_bytes}" -ge 4194304 ]]; then
    fail "bytes_gc_churn: expected live bytes under 4MB after reclaim, got GC_LIVE_BYTES=${live_bytes:-<missing>}"
  else
    echo "gc_fixtures: bytes_gc_churn reclaimed correctly (collections=${collections}, live_bytes=${live_bytes})"
  fi
fi

if [[ ${failures} -gt 0 ]]; then
  echo "gc: ${failures} failure(s)" >&2
  exit 1
fi

echo "gc: passed"
