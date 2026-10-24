// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <jello/internal/jit_arm64.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__)
static void test_register_role_constants(void) {
  assert(JIT_REG_OUT == 19);
  assert(JIT_REG_VM == 20);
  assert(JIT_REG_BASE == 21);
  assert(JIT_REG_SURVIVE == 28);
  printf("test_register_role_constants: PASS\n");
}

static void test_gpr_and_xmm_pool_tables(void) {
  assert(jit_gpr_pool_len == 6u);
  assert(jit_xmm_pool_len == 8u);
  static const uint8_t expected_gpr[] = {9, 10, 11, 12, 13, 14};
  assert(memcmp(jit_gpr_pool, expected_gpr, sizeof(expected_gpr)) == 0);
  printf("test_gpr_and_xmm_pool_tables: PASS\n");
}
#else
static void test_register_role_constants(void) {
  printf("test_register_role_constants: SKIPPED (host is not aarch64)\n");
}

static void test_gpr_and_xmm_pool_tables(void) {
  printf("test_gpr_and_xmm_pool_tables: SKIPPED (host is not aarch64)\n");
}
#endif

/* The encoders are header inlines, so their words are checked on any host. */
static void test_halfword_and_branch_encodings(void) {
  assert(a64_ldrh_w_uimm(0, 1, 2) == 0x79400420u);    // ldrh w0, [x1, #2]
  assert(a64_strh_w_uimm(0, 1, 2) == 0x79000420u);    // strh w0, [x1, #2]
  assert(a64_ldrh_w_uimm(3, 4, 8190) == 0x797FFC83u); // ldrh w3, [x4, #8190]
  assert(a64_b_cond(A64_LE, 0) == 0x5400000Du);       // b.le .
  assert(a64_b_cond(A64_GE, 0) == 0x5400000Au);       // b.ge .
  printf("test_halfword_and_branch_encodings: PASS\n");
}

int main(void) {
  test_halfword_and_branch_encodings();
  test_register_role_constants();
  test_gpr_and_xmm_pool_tables();
  printf("jit_arm64_tests: all tests passed\n");
  return 0;
}
