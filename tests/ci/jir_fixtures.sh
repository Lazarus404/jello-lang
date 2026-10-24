#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Jahred Love. All rights reserved.
#
# Golden .jir.txt fixtures for jello_jit_ir_build/verify, one per IR shape.
# The fixtures reuse an existing bytecode fixture this platform's own
# interpreter/bench/packet suites already prove executes correctly --
# matching a runtime fixture 1:1 rather than inventing a parallel bytecode
# corpus purely for the IR layer. `tak` covers a genuinely multi-argument
# JIR_CALL_SELF, which `fib` alone (arity 1) does not exercise.
set -euo pipefail

JLO="${1:?usage: jir_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IR_DIR="$ROOT/tests/jit/ir"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

check_pair() {
  local src="$1" golden="$2" label="$3"
  local bin="$TMP/$label.jlo"
  local got="$TMP/$label.jir.txt"
  if ! "$JLO" asm "$src" -o "$bin"; then
    echo "FAIL ($label): asm of $src failed"
    fail=1
    return
  fi
  if ! "$JLO" dumpir "$bin" > "$got" 2>"$TMP/$label.err"; then
    echo "FAIL ($label): dumpir failed: $(cat "$TMP/$label.err")"
    fail=1
    return
  fi
  if ! diff -u "$golden" "$got" > "$TMP/$label.diff"; then
    echo "FAIL ($label): dumpir output does not match golden fixture $golden"
    cat "$TMP/$label.diff"
    fail=1
    return
  fi
  echo "PASS ($label)"
}

# Pure-I32 arithmetic (JIR_BIN_I32/JIR_ADD_I32_IMM/JIR_LT_I32_IMM),
# numeric_fast, and both JIR_CALL_DIRECT (fn[0] -> fn[1]) and JIR_CALL_SELF
# (fn[1] -> fn[1]) in the same module.
check_pair "$ROOT/tests/bytecode/bench/fib.jlo.txt" \
           "$IR_DIR/fib_bin_i32_call_self_call_direct.jir.txt" \
           "fib_bin_i32_call_self_call_direct"

# Multi-argument self-calls: tak's own three-argument self-recursive calls
# exercise JIR_CALL_SELF with a real arg count > 1, which fib's own arity-1
# self-call does not.
check_pair "$ROOT/tests/bytecode/bench/tak.jlo.txt" \
           "$IR_DIR/tak_call_self.jir.txt" \
           "tak_call_self"

# OBJ_NEW/OBJ_SET_ATOM/OBJ_GET_ATOM -- proves flags excludes numeric_fast
# (JELLO_OPF_TOUCHES_DYNAMIC on every one of these opcodes).
check_pair "$ROOT/tests/bytecode/dyn/obj_set_get_atom_typed.jlo.txt" \
           "$IR_DIR/obj_set_get_atom_not_numeric_fast.jir.txt" \
           "obj_set_get_atom_not_numeric_fast"

# BYTES_WRITE_U32_LE/BYTES_BLIT/BYTES_READ_U32_LE -- proves the
# JIR_BYTES_READ/JIR_BYTES_WRITE consolidation (imm names the width+endian+
# kind) and that JIR_BYTES_BLIT stays one-to-one.
check_pair "$ROOT/tests/bytecode/packet/blit_mid_offset.jlo.txt" \
           "$IR_DIR/blit_mid_offset_bytes_read_write_blit.jir.txt" \
           "blit_mid_offset_bytes_read_write_blit"

# A loop with a backward JMP -- proves JIR_SAFEPOINT insertion at exactly the
# right site (immediately before the backward branch), mirroring exec_loop.c's
# own vm_safepoint_check condition.
check_pair "$ROOT/tests/bytecode/bench/gc_churn.jlo.txt" \
           "$IR_DIR/gc_churn_backward_jmp_safepoint.jir.txt" \
           "gc_churn_backward_jmp_safepoint"

# BITS_WRITE/BITS_READ's own literal width -- proves both that
# jello_jit_ir_verify does not mistake it for an out-of-range register
# (jello_jit_ir_op_reg_mask) *and* that jlo_tool's own dumpir renderer still
# surfaces it as real instruction data (`width=N`) rather than silently
# dropping it just because it is not a register.
check_pair "$ROOT/tests/bytecode/bits_accept.jlo.txt" \
           "$IR_DIR/bits_accept_width_is_a_literal_not_a_register.jir.txt" \
           "bits_accept_width_is_a_literal_not_a_register"

exit $fail
