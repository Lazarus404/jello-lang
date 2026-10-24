// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* Every check in this file is an assert() around jello_jit_ir_verify's own
 * return value -- a -DNDEBUG (Release) build would strip every one of them
 * to a no-op, making this whole file report "all tests passed" while
 * actually checking nothing. #undef first makes assert() unconditional in
 * this translation unit regardless of the build's own global NDEBUG
 * setting: test code must never depend on NDEBUG being unset. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* -- jir_tests --------------------------------------------------
 * jello_jit_ir_verify's own negative fixtures -- hand-constructed
 * jello_jit_ir_function/jello_bc_function pairs, built directly here
 * rather than through jello_jit_ir_build, exactly as
 * tests/csrc/thread_tests.c already hand-constructs bytecode
 * modules where a .jlo.txt fixture cannot express what the test needs.
 * jello_jit_ir_build itself is already proven correct by the golden
 * .jir.txt fixtures (tests/ci/jir_fixtures.sh) -- this file's own job is
 * narrower: prove jello_jit_ir_verify actually rejects the two shapes of
 * malformed IR it claims to check for, which no fixture built through the
 * real lowering pass could ever produce in the first place (jello_jit_ir_build
 * only ever writes in-range registers and real bc_pc values).
 */

static jello_bc_function make_src(uint32_t nregs, uint32_t ninsns) {
  jello_bc_function f = {0};
  f.nregs = nregs;
  f.ninsns = ninsns;
  f.reg_types = NULL;
  f.insns = NULL;
  f.cap_start = 0;
  return f;
}

static void test_verify_accepts_well_formed_ir(void) {
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/2);
  jello_jit_ir_insn insns[2] = {
    { (uint16_t)JIR_MOV, 0, 1, 0, 0, 0 },
    { (uint16_t)JIR_RET, 0, 0, 0, 0, 1 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 2;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NUMERIC_FAST;

  assert(jello_jit_ir_verify(&f, &src) == 1);
  printf("test_verify_accepts_well_formed_ir: PASS\n");
}

static void test_verify_accepts_safepoint_sharing_bc_pc(void) {
  /* A JIR_SAFEPOINT may carry the *same* bc_pc as the branch/call instruction
   * immediately following it -- this is the one place instruction count
   * differs from the source's own ninsns. */
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/1);
  jello_jit_ir_insn insns[2] = {
    { (uint16_t)JIR_SAFEPOINT, 0, 0, 0, 0, 0 },
    { (uint16_t)JIR_CALL_SELF, 0, 0, 0, 0, 0 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 2;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 1);
  printf("test_verify_accepts_safepoint_sharing_bc_pc: PASS\n");
}

static void test_verify_rejects_out_of_range_register(void) {
  /* nregs=2, so register index 5 on JIR_MOV's own `a` (a genuine register
   * slot per jello_jit_ir_op_reg_mask) must be rejected. */
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/1);
  jello_jit_ir_insn insns[1] = {
    { (uint16_t)JIR_MOV, 5, 0, 0, 0, 0 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 1;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 0);
  printf("test_verify_rejects_out_of_range_register: PASS\n");
}

static void test_verify_ignores_non_register_slots_for_bounds_check(void) {
  /* JIR_BITS_WRITE's own `c` is a literal width, not a register -- a value
   * of 32 here must *not* trip the register bounds check even though
   * nregs is far smaller, exactly the regression a smoke test against the
   * full fixture corpus first caught. */
  jello_bc_function src = make_src(/*nregs=*/3, /*ninsns=*/1);
  jello_jit_ir_insn insns[1] = {
    { (uint16_t)JIR_BITS_WRITE, 0, 1, 32, 0, 0 },
  };
  jello_jit_ir_function f;
  f.nregs = 3;
  f.ninsns = 1;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 1);
  printf("test_verify_ignores_non_register_slots_for_bounds_check: PASS\n");
}

static void test_verify_rejects_fabricated_bc_pc(void) {
  /* src has only 1 real bytecode instruction (index 0); bc_pc=7 does not
   * correspond to anything in it. */
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/1);
  jello_jit_ir_insn insns[1] = {
    { (uint16_t)JIR_RET, 0, 0, 0, 0, 7 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 1;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 0);
  printf("test_verify_rejects_fabricated_bc_pc: PASS\n");
}

int main(void) {
  test_verify_accepts_well_formed_ir();
  test_verify_accepts_safepoint_sharing_bc_pc();
  test_verify_rejects_out_of_range_register();
  test_verify_ignores_non_register_slots_for_bounds_check();
  test_verify_rejects_fabricated_bc_pc();
  printf("jir_tests: all tests passed\n");
  return 0;
}
