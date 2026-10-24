// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_impl.h>

#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jello/internal/platform.h>

static jello_bc_module make_i32_module(jello_type_entry* types,
                                       jello_type_id* reg_types,
                                       jello_insn* insns,
                                       jello_bc_function* func,
                                       uint32_t nregs,
                                       uint32_t ninsns) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  func->nregs = nregs;
  func->cap_start = 0;
  func->reg_types = reg_types;
  func->ninsns = ninsns;
  func->insns = insns;

  jello_bc_module m = {0};
  m.ntypes = 1;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = func;
  m.entry = 0;
  return m;
}

static void init_i32_reg_types(jello_type_id* reg_types, uint32_t nregs) {
  for(uint32_t i = 0; i < nregs; i++) reg_types[i] = 0;
}

#if defined(__x86_64__) || defined(__aarch64__)
static jello_exec_status run_jit_entry(const jello_bc_module* m,
                                       jello_numeric_result* out) {
  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(m, 0, &code) == 1);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  atomic_store(&vm->in_exec, 1);
  vm_frame_reset(vm);
  vm_enter_module(vm, m);
  assert(vm_push_frame(vm, m, 0, 0, 0, 0, 0) == 1);

  jello_jit_entry_fn entry =
      (jello_jit_entry_fn)(void*)(code.buf.mem + code.entry_off);
  jello_exec_status st = entry(vm, jit_frame_entry_base(vm), out);

  atomic_store(&vm->in_exec, 0);
  jello_jit_code_free(&code);
  jello_vm_destroy(vm);
  return st;
}
#endif

static void test_add_i32(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func = {0};

  init_i32_reg_types(reg_types, 3);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = 3;
  insns[2].op = (uint8_t)JOP_ADD_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 8);
  printf("test_add_i32: PASS\n");
#else
  printf("test_add_i32: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
}

static void test_div_i32_by_zero_trap(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func = {0};

  init_i32_reg_types(reg_types, 3);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_DIV_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_numeric_result out;
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_TRAP);
  printf("test_div_i32_by_zero_trap: PASS\n");
#else
  printf("test_div_i32_by_zero_trap: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
}

static void test_div_i32_min_neg1_trap(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func = {0};

  init_i32_reg_types(reg_types, 3);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = (uint32_t)INT32_MIN;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = (uint32_t)-1;
  insns[2].op = (uint8_t)JOP_DIV_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_numeric_result out;
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_TRAP);
  printf("test_div_i32_min_neg1_trap: PASS\n");
#else
  printf("test_div_i32_min_neg1_trap: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
}

/** Runs `r2 = op(r0, r1); ret r2` with float operands `x` and `y` through the JIT and the
 * interpreter, and checks both give C's answer, which is false whenever either is NaN. */
static void check_float_cmp(jello_op op, double x, double y) {
#if defined(__x86_64__) || defined(__aarch64__)
  int is_f64 = op == JOP_EQ_F64 || op == JOP_LT_F64;
  jello_type_entry types[2];
  memset(types, 0, sizeof(types));
  types[0].kind = is_f64 ? JELLO_T_F64 : JELLO_T_F32;
  types[1].kind = JELLO_T_BOOL;
  jello_type_id reg_types[3] = {0, 0, 1};
  double pool[2] = {x, y};
  float fx = (float)x, fy = (float)y;
  uint32_t bx, by;
  memcpy(&bx, &fx, 4);
  memcpy(&by, &fy, 4);

  jello_insn insns[4];
  memset(insns, 0, sizeof(insns));
  insns[0].op = (uint8_t)(is_f64 ? JOP_CONST_F64 : JOP_CONST_F32); insns[0].a = 0;
  insns[0].imm = is_f64 ? 0u : bx;
  insns[1].op = (uint8_t)(is_f64 ? JOP_CONST_F64 : JOP_CONST_F32); insns[1].a = 1;
  insns[1].imm = is_f64 ? 1u : by;
  insns[2].op = (uint8_t)op; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_function func = {0};
  func.nregs = 3;
  func.reg_types = reg_types;
  func.ninsns = 4;
  func.insns = insns;
  jello_bc_module m = {0};
  m.ntypes = 2;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &func;
  m.nconst_f64 = 2;
  m.const_f64 = pool;

  int is_eq = op == JOP_EQ_F32 || op == JOP_EQ_F64;
  int want = is_f64 ? (is_eq ? x == y : x < y) : (is_eq ? fx == fy : fx < fy);

  jello_numeric_result jit_out;
  memset(&jit_out, 0, sizeof(jit_out));
  assert(run_jit_entry(&m, &jit_out) == JELLO_EXEC_OK);

  jello_setenv("JELLO_JIT_DISABLE", "1");
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result interp_out;
  memset(&interp_out, 0, sizeof(interp_out));
  assert(jello_vm_exec_numeric(vm, &m, &interp_out) == JELLO_EXEC_OK);
  jello_vm_destroy(vm);
  jello_unsetenv("JELLO_JIT_DISABLE");

  assert(interp_out.kind == JELLO_NUM_BOOL && jit_out.kind == JELLO_NUM_BOOL);
  if(jit_out.as.b != interp_out.as.b || interp_out.as.b != want) {
    fprintf(stderr, "%s_F%d(%g, %g): jit=%d interp=%d\n", is_eq ? "EQ" : "LT", is_f64 ? 64 : 32,
            x, y, (int)jit_out.as.b, (int)interp_out.as.b);
    assert(0 && "the JIT's float compare disagrees with the interpreter");
  }
#else
  (void)op; (void)x; (void)y;
#endif
}

static void test_float_cmp_nan_matches_interpreter(void) {
  static const jello_op ops[4] = {JOP_EQ_F32, JOP_LT_F32, JOP_EQ_F64, JOP_LT_F64};
  const double cases[][2] = {
    {NAN, 1.0}, {1.0, NAN}, {NAN, NAN}, {1.0, 2.0}, {2.0, 1.0}, {1.0, 1.0},
  };
  for(size_t oi = 0; oi < 4; oi++) {
    for(size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
      check_float_cmp(ops[oi], cases[ci][0], cases[ci][1]);
  }
  printf("test_float_cmp_nan_matches_interpreter: PASS\n");
}

static void test_accept_jmp_function(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[1];
  jello_insn insns[2];
  jello_bc_function func = {0};

  init_i32_reg_types(reg_types, 1);
  insns[0].op = (uint8_t)JOP_JMP; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 1, 2);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(code.buf.mem != NULL);
  jello_jit_code_free(&code);
  printf("test_accept_jmp_function: PASS\n");
}

static void test_reject_call_function(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[2];
  jello_insn insns[2];
  jello_bc_function func = {0};

  init_i32_reg_types(reg_types, 2);
  insns[0].op = (uint8_t)JOP_CALL; insns[0].a = 0; insns[0].b = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 2, 2);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 0);
  assert(code.buf.mem == NULL);
  printf("test_reject_call_function: PASS\n");
}

static void test_reload_count_const_add_add(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[1];
  jello_insn insns[4];
  jello_bc_function func = {0};

  init_i32_reg_types(reg_types, 1);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 1;
  insns[1].op = (uint8_t)JOP_ADD_I32; insns[1].a = 0; insns[1].b = 0; insns[1].c = 0;
  insns[2].op = (uint8_t)JOP_ADD_I32; insns[2].a = 0; insns[2].b = 0; insns[2].c = 0;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 0;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 1, 4);

  jit_reg_reload_count = 0;
  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(jit_reg_reload_count == 0);
  jello_jit_code_free(&code);
  printf("test_reload_count_const_add_add: PASS (reload_count=%u)\n", jit_reg_reload_count);
}

int main(void) {
  test_add_i32();
  test_div_i32_by_zero_trap();
  test_div_i32_min_neg1_trap();
  test_float_cmp_nan_matches_interpreter();
  test_accept_jmp_function();
  test_reject_call_function();
  test_reload_count_const_add_add();
  printf("jit_numeric_tests: all tests passed\n");
  return 0;
}
