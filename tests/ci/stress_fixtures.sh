#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: stress_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/stress"
GEN="${ROOT}/tests/ci/gen_stress_fixtures.py"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "stress_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

python3 "${GEN}" "${TMP}/gen"

jlo_run() {
  local disable_jit="$1"
  shift
  if [[ "${disable_jit}" == "1" ]]; then
    JELLO_JIT_DISABLE=1 "${JLO_TOOL}" run "$@" 2>"${TMP}/run.err" || true
  else
    env -u JELLO_JIT_DISABLE "${JLO_TOOL}" run "$@" 2>"${TMP}/run.err" || true
  fi
}

expect_line() {
  local label="$1"
  local want="$2"
  local disable_jit="$3"
  shift 3
  local got
  got="$(jlo_run "${disable_jit}" "$@" | head -1)"
  if [[ "${got}" != "${want}" ]]; then
    fail "${label}: expected [${want}] got [${got}] $(cat "${TMP}/run.err")"
  else
    echo "stress_fixtures: ${label} -> ${got}"
  fi
}

expect_trap_pc() {
  local label="$1"
  local want_trap="$2"
  local want_func="$3"
  local want_pc="$4"
  local disable_jit="$5"
  shift 5
  local out trap_line pc_line
  out="$(jlo_run "${disable_jit}" "$@" --trap-pc)"
  trap_line="$(printf '%s\n' "${out}" | awk '/^TRAP code=/{print; exit}')"
  pc_line="$(printf '%s\n' "${out}" | awk '/^TRAP_PC /{print; exit}')"
  if [[ "${trap_line}" != "${want_trap}" ]]; then
    fail "${label}: expected [${want_trap}] got [${trap_line}] $(cat "${TMP}/run.err")"
    return
  fi
  if [[ "${pc_line}" != "TRAP_PC func_index=${want_func} pc=${want_pc}" ]]; then
    fail "${label}: expected TRAP_PC func_index=${want_func} pc=${want_pc} got [${pc_line}]"
    return
  fi
  echo "stress_fixtures: ${label} -> ${trap_line} ${pc_line}"
}

asm() {
  local src="$1"
  local bin="$2"
  if ! "${JLO_TOOL}" asm "${src}" -o "${bin}" 2>"${TMP}/asm.err"; then
    fail "asm $(basename "${src}"): $(cat "${TMP}/asm.err")"
    return 1
  fi
  return 0
}

echo "== generate combinatorial fixtures =="
echo "stress_fixtures: wrote generated fixtures under ${TMP}/gen"

echo "== frame-stack / nregs ceilings =="
if asm "${FIXDIR}/frames_ceiling.jlo.txt" "${TMP}/frames_ceiling.jlo"; then
  expect_trap_pc "frames_ceiling (no fuel) interp" "TRAP code=STACK_OVERFLOW" 0 0 1 \
    "${TMP}/frames_ceiling.jlo"
  expect_trap_pc "frames_ceiling --fuel 50000 (CALL_FRAMES_MAX not reached) interp" \
    "TRAP code=FUEL" 0 0 1 "${TMP}/frames_ceiling.jlo" --fuel 50000
  expect_trap_pc "frames_ceiling --fuel 1500000 (frames ceiling wins) interp" \
    "TRAP code=STACK_OVERFLOW" 0 0 1 "${TMP}/frames_ceiling.jlo" --fuel 1500000
  expect_trap_pc "frames_ceiling (no fuel) jit" "TRAP code=STACK_OVERFLOW" 0 0 0 \
    "${TMP}/frames_ceiling.jlo"
fi

if asm "${TMP}/gen/bytes_stack_ceiling.jlo.txt" "${TMP}/bytes_stack_ceiling.jlo"; then
  # 2 KiB frames: 64 MiB, then 1 GiB of segments, fill before a million frames or the fuel.
  expect_trap_pc "bytes_stack_ceiling --fuel 700000 (frame bytes win before fuel) interp" \
    "TRAP code=STACK_OVERFLOW" 0 0 1 "${TMP}/bytes_stack_ceiling.jlo" --fuel 700000
  expect_trap_pc "bytes_stack_ceiling --fuel 700000 jit" \
    "TRAP code=STACK_OVERFLOW" 0 0 0 "${TMP}/bytes_stack_ceiling.jlo" --fuel 700000
fi

if asm "${TMP}/gen/nregs_at_loader_maximum.jlo.txt" "${TMP}/nregs_at.jlo"; then
  expect_line "nregs_at_loader_maximum interp" "RESULT kind=I32 value=1" 1 "${TMP}/nregs_at.jlo"
  expect_line "nregs_at_loader_maximum jit" "RESULT kind=I32 value=1" 0 "${TMP}/nregs_at.jlo"
fi

if "${JLO_TOOL}" asm "${TMP}/gen/nregs_over_loader_maximum.jlo.txt" -o "${TMP}/nregs_over.jlo" \
    2>"${TMP}/nregs_over.asm.err"; then
  fail "nregs_over_loader_maximum: assembler accepted nregs=257"
else
  if ! grep -q "256" "${TMP}/nregs_over.asm.err"; then
    fail "nregs_over_loader_maximum: assembler rejected, but not the 256 ceiling -- $(cat "${TMP}/nregs_over.asm.err")"
  else
    echo "stress_fixtures: nregs_over_loader_maximum assembler reject -> $(cat "${TMP}/nregs_over.asm.err")"
  fi
fi

python3 - "${TMP}/nregs_at.jlo" "${TMP}/nregs_over.bin" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
sig = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x00, 0x00])
idx = data.find(sig)
if idx < 0:
    sys.stderr.write("nregs patch: function header signature not found\n")
    sys.exit(1)
data[idx + 4:idx + 8] = (257).to_bytes(4, "little")
open(dst, "wb").write(data)
PY
if "${JLO_TOOL}" disasm "${TMP}/nregs_over.bin" -o "${TMP}/nregs_over.txt" 2>"${TMP}/nregs_over.load.err"; then
  fail "nregs_over binary: loader accepted nregs=257"
else
  if ! grep -q "nregs > 256" "${TMP}/nregs_over.load.err"; then
    fail "nregs_over binary: expected 'nregs > 256', got $(cat "${TMP}/nregs_over.load.err")"
  else
    echo "stress_fixtures: nregs_over loader reject -> $(cat "${TMP}/nregs_over.load.err")"
  fi
fi

echo "== JIT register-allocator overflow =="
if asm "${TMP}/gen/reg_pressure_gpr.jlo.txt" "${TMP}/reg_gpr.jlo"; then
  expect_line "reg_pressure_gpr interp" "RESULT kind=I32 value=136" 1 "${TMP}/reg_gpr.jlo"
  expect_line "reg_pressure_gpr jit" "RESULT kind=I32 value=136" 0 "${TMP}/reg_gpr.jlo"
fi
if asm "${TMP}/gen/reg_pressure_xmm.jlo.txt" "${TMP}/reg_xmm.jlo"; then
  expect_line "reg_pressure_xmm interp" "RESULT kind=F64 value=136" 1 "${TMP}/reg_xmm.jlo"
  expect_line "reg_pressure_xmm jit" "RESULT kind=F64 value=136" 0 "${TMP}/reg_xmm.jlo"
fi
if asm "${TMP}/gen/reg_pressure_mixed.jlo.txt" "${TMP}/reg_mixed.jlo"; then
  expect_line "reg_pressure_mixed interp" "RESULT kind=I32 value=156" 1 "${TMP}/reg_mixed.jlo"
  expect_line "reg_pressure_mixed jit" "RESULT kind=I32 value=156" 0 "${TMP}/reg_mixed.jlo"
fi
if asm "${TMP}/gen/reg_pressure_survive_interaction.jlo.txt" "${TMP}/reg_surv.jlo"; then
  expect_line "reg_pressure_survive interp" "RESULT kind=I32 value=55" 1 "${TMP}/reg_surv.jlo"
  expect_line "reg_pressure_survive jit" "RESULT kind=I32 value=55" 0 "${TMP}/reg_surv.jlo"
fi

echo "== GC all size classes + pointer churn + pause ceiling =="
if asm "${TMP}/gen/all_size_classes_churn.jlo.txt" "${TMP}/gc_all.jlo"; then
  out="$("${JLO_TOOL}" run "${TMP}/gc_all.jlo" --gc-stats 2>"${TMP}/gc_all.err" || true)"
  echo "${out}"
  collections="$(echo "${out}" | awk '/^GC_COLLECTIONS/ {print $2}')"
  pause="$(echo "${out}" | awk '/^GC_PAUSE_MS/ {print $2}')"
  if [[ -z "${collections}" || "${collections}" -le 1 ]]; then
    fail "all_size_classes_churn: expected GC_COLLECTIONS > 1, got ${collections:-<missing>}"
  elif ! python3 -c "import sys; sys.exit(0 if float('${pause:-99999}') < 5000.0 else 1)"; then
    fail "all_size_classes_churn: GC_PAUSE_MS ${pause} exceeded 5000ms ceiling"
  else
    echo "stress_fixtures: all_size_classes_churn collections=${collections} pause_ms=${pause}"
  fi
fi
if asm "${TMP}/gen/struct_object_pointer_churn.jlo.txt" "${TMP}/gc_ptr.jlo"; then
  out="$("${JLO_TOOL}" run "${TMP}/gc_ptr.jlo" --gc-stats 2>"${TMP}/gc_ptr.err" || true)"
  echo "${out}"
  collections="$(echo "${out}" | awk '/^GC_COLLECTIONS/ {print $2}')"
  if [[ -z "${collections}" || "${collections}" -le 0 ]]; then
    fail "struct_object_pointer_churn: expected GC_COLLECTIONS > 0, got ${collections:-<missing>}"
  else
    echo "stress_fixtures: struct_object_pointer_churn collections=${collections}"
  fi
fi

echo "== Bytes/packet boundary sweep =="
if asm "${FIXDIR}/bytes_len_1000.jlo.txt" "${TMP}/bytes_1000.jlo"; then
  expect_line "bytes at max_bytes_len=1000 interp" "RESULT kind=PTR value=NON_NULL" \
    1 "${TMP}/bytes_1000.jlo" --max-bytes-len 1000
  expect_line "bytes one under max_bytes_len=1001 interp" "RESULT kind=PTR value=NON_NULL" \
    1 "${TMP}/bytes_1000.jlo" --max-bytes-len 1001
  expect_line "bytes one over max_bytes_len=999 interp" "TRAP code=LIMIT" \
    1 "${TMP}/bytes_1000.jlo" --max-bytes-len 999
  expect_line "bytes at limit jit" "RESULT kind=PTR value=NON_NULL" \
    0 "${TMP}/bytes_1000.jlo" --max-bytes-len 1000
  expect_line "bytes one over jit" "TRAP code=LIMIT" \
    0 "${TMP}/bytes_1000.jlo" --max-bytes-len 999
fi

for w in $(seq -w 1 32); do
  src="${TMP}/gen/bits_width_${w}.jlo.txt"
  bin="${TMP}/bits_${w}.jlo"
  if asm "${src}" "${bin}"; then
    expect_line "bits_width_${w} interp" "RESULT kind=BOOL value=1" 1 "${bin}"
    expect_line "bits_width_${w} jit" "RESULT kind=BOOL value=1" 0 "${bin}"
  fi
done

if asm "${TMP}/gen/blit_exactly_at_end.jlo.txt" "${TMP}/blit_at.jlo"; then
  expect_line "blit exactly at end interp" "RESULT kind=I32 value=12" 1 "${TMP}/blit_at.jlo"
  expect_line "blit exactly at end jit" "RESULT kind=I32 value=12" 0 "${TMP}/blit_at.jlo"
fi
if asm "${TMP}/gen/blit_one_past_end.jlo.txt" "${TMP}/blit_past.jlo"; then
  expect_line "blit one past end interp" "TRAP code=BOUNDS" 1 "${TMP}/blit_past.jlo"
  expect_line "blit one past end jit" "TRAP code=BOUNDS" 0 "${TMP}/blit_past.jlo"
fi
if asm "${TMP}/gen/blit_u32_max_offset.jlo.txt" "${TMP}/blit_u32.jlo"; then
  expect_line "blit UINT32_MAX offset interp" "TRAP code=BOUNDS" 1 "${TMP}/blit_u32.jlo"
  expect_line "blit UINT32_MAX offset jit" "TRAP code=BOUNDS" 0 "${TMP}/blit_u32.jlo"
fi
if asm "${TMP}/gen/blit_near_u32_max.jlo.txt" "${TMP}/blit_near.jlo"; then
  expect_line "blit UINT32_MAX-7 offset interp" "TRAP code=BOUNDS" 1 "${TMP}/blit_near.jlo"
  expect_line "blit UINT32_MAX-7 offset jit" "TRAP code=BOUNDS" 0 "${TMP}/blit_near.jlo"
fi

echo "== fuel vs stack-overflow ordering + fuel under survive =="
if asm "${FIXDIR}/frames_ceiling.jlo.txt" "${TMP}/fuel_race.jlo"; then
  expect_trap_pc "fuel wins (budget 1000) interp" "TRAP code=FUEL" 0 0 1 \
    "${TMP}/fuel_race.jlo" --fuel 1000
  expect_trap_pc "stack overflow wins (budget 1500000) interp" "TRAP code=STACK_OVERFLOW" 0 0 1 \
    "${TMP}/fuel_race.jlo" --fuel 1500000
  expect_trap_pc "fuel wins jit" "TRAP code=FUEL" 0 0 0 \
    "${TMP}/fuel_race.jlo" --fuel 1000
  expect_trap_pc "stack overflow wins jit" "TRAP code=STACK_OVERFLOW" 0 0 0 \
    "${TMP}/fuel_race.jlo" --fuel 1500000
fi
if asm "${FIXDIR}/fuel_under_survive.jlo.txt" "${TMP}/fuel_surv.jlo"; then
  expect_line "fuel_under_survive unlimited interp" "RESULT kind=I32 value=144" 1 "${TMP}/fuel_surv.jlo"
  expect_line "fuel_under_survive unlimited jit" "RESULT kind=I32 value=144" 0 "${TMP}/fuel_surv.jlo"
  expect_trap_pc "fuel_under_survive --fuel 8 interp" "TRAP code=FUEL" 1 3 1 \
    "${TMP}/fuel_surv.jlo" --fuel 8
  expect_trap_pc "fuel_under_survive --fuel 8 jit" "TRAP code=FUEL" 1 3 0 \
    "${TMP}/fuel_surv.jlo" --fuel 8
fi

if [[ ${failures} -gt 0 ]]; then
  echo "stress_bytecode: ${failures} failure(s)" >&2
  exit 1
fi

echo "stress_bytecode: passed"
