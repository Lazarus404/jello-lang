// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Mixed JIT/interpreter execution: the cases the bytecode fixtures
 * (tests/bytecode/interp/mixed_*) cannot express -- a VM reused after a
 * trap, and a thread whose stack is far smaller than the main thread's.
 * Modules are built by hand, as in trap_pc_tests.c. A function with a
 * SWITCH_KIND is never compiled; one with no cases is a no-op on a Dynamic
 * register (DECLINE), so it is how these modules force a crossing. */

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jello/internal/platform.h>

enum { T_I32 = 0, T_DYN = 1, T_BOOL = 2, T_FUN = 3, T_BYTES = 4 };

static jello_type_entry types[5];
static const jello_type_id sig_args[1] = {T_I32};
static const jello_fun_sig sigs[1] = {{T_I32, 1, sig_args}};

static jello_insn I(jello_op op, uint8_t a, uint8_t b, uint8_t c, uint32_t imm) {
  jello_insn ins;
  ins.op = (uint8_t)op; ins.a = a; ins.b = b; ins.c = c; ins.imm = imm;
  return ins;
}

/* Two per function, where the spill pair used to be, so pcs are unchanged. */
static jello_insn DECLINE(uint8_t dynamic_reg) { return I(JOP_SWITCH_KIND, dynamic_reg, 0, 0, 0); }

static jello_bc_function F(jello_type_id* regs, uint32_t nregs, jello_insn* insns, uint32_t ninsns) {
  jello_bc_function f = {0};
  f.name_atom = JELLO_BC_NO_NAME;
  f.nregs = nregs;
  f.cap_start = nregs;
  f.reg_types = regs;
  f.ninsns = ninsns;
  f.insns = insns;
  return f;
}

static jello_bc_module M(jello_bc_function* funcs, uint32_t nfuncs) {
  types[T_I32].kind = JELLO_T_I32;
  types[T_DYN].kind = JELLO_T_DYNAMIC;
  types[T_BOOL].kind = JELLO_T_BOOL;
  types[T_FUN].kind = JELLO_T_FUNCTION;
  types[T_FUN].as.fun.sig_id = 0;
  types[T_BYTES].kind = JELLO_T_BYTES;
  jello_bc_module m = {0};
  m.ntypes = 5;
  m.types = types;
  m.nsigs = 1;
  m.sigs = sigs;
  m.nfuncs = nfuncs;
  m.funcs = funcs;
  m.entry = 0;
  return m;
}

static int jit_on(void) {
#if defined(__aarch64__) || defined(__x86_64__)
  return getenv("JELLO_JIT_DISABLE") == NULL;
#else
  return 0;
#endif
}

/* A trap leaves its frames pushed. f1 below traps with a try scope live
 * on frame 1; the next run reuses frame 1 for a JIT call into a declined
 * function that throws. Unless the reset cleared that scope, the throw
 * lands in it and the run returns normally instead of trapping. */
static void test_rerun_after_trap_with_live_try(void) {
  /* Every function of the first run declines, so
   * the whole run is interpreted and its frames stay pushed after the
   * trap. */
  jello_type_id r_a1[2] = {T_I32, T_DYN};
  jello_insn c_a1[] = {DECLINE(1), DECLINE(1), I(JOP_TRY, 1, 0, 0, 2),
                       I(JOP_CALL, 0, 0, 0, 2), I(JOP_ENDTRY, 0, 0, 0, 0), I(JOP_RET, 0, 0, 0, 0)};
  jello_type_id r_a2[3] = {T_I32, T_I32, T_DYN};
  jello_insn c_a2[] = {DECLINE(2), DECLINE(2), I(JOP_CONST_I32, 1, 0, 0, 0),
                       I(JOP_DIV_I32, 0, 1, 1, 0), I(JOP_RET, 0, 0, 0, 0)};
  jello_type_id r_a0t[2] = {T_I32, T_DYN};
  jello_insn c_a0t[] = {DECLINE(1), DECLINE(1), I(JOP_CALL, 0, 0, 0, 1),
                        I(JOP_RET, 0, 0, 0, 0)};
  jello_bc_function fa[3] = {F(r_a0t, 2, c_a0t, 4), F(r_a1, 2, c_a1, 6), F(r_a2, 3, c_a2, 5)};
  jello_bc_module ma = M(fa, 3);

  jello_type_id r_b0[1] = {T_I32};
  jello_insn c_b0[] = {I(JOP_CALL, 0, 0, 0, 1), I(JOP_RET, 0, 0, 0, 0)};
  jello_type_id r_b1[2] = {T_I32, T_DYN};
  jello_insn c_b1[] = {DECLINE(1), DECLINE(1), I(JOP_CONST_NULL, 1, 0, 0, 0),
                       I(JOP_THROW, 1, 0, 0, 0), I(JOP_CONST_I32, 0, 0, 0, 7), I(JOP_RET, 0, 0, 0, 0)};
  jello_bc_function fb[2] = {F(r_b0, 1, c_b0, 2), F(r_b1, 2, c_b1, 6)};
  jello_bc_module mb = M(fb, 2);

  jello_vm* vm = jello_vm_create();
  assert(vm);
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, &ma, &out) == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  assert(vm->call_frames_len == 3 && vm->call_frames[1].try_stack_len == 1);

  uint64_t bridges = vm->jit_bridges;
  jello_exec_status st = jello_vm_exec_numeric(vm, &mb, &out);
  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_UNCAUGHT_THROW);
  assert(jello_vm_last_trap_func_index(vm) == 1 && jello_vm_last_trap_pc(vm) == 3);
  if(jit_on()) assert(vm->jit_bridges == bridges + 1);
  assert(vm->live_try_scopes == 0);
  jello_vm_destroy(vm);
  printf("test_rerun_after_trap_with_live_try: PASS\n");
}

/* f0 -> f1 (declined) -> f2 (compiled) -> f1 ..., summing n..1: every level
 * crosses. */
typedef struct alternating {
  jello_type_id r0[2], r1[6], r2[5];
  jello_insn c0[3], c1[10], c2[8];
  jello_bc_function f[3];
  jello_bc_module m;
} alternating;

static void build_alternating(alternating* a, uint32_t n) {
  jello_type_id r0[2] = {T_I32, T_I32};
  jello_insn c0[] = {I(JOP_CONST_I32, 0, 0, 0, n), I(JOP_CALL, 1, 0, 1, 1), I(JOP_RET, 1, 0, 0, 0)};
  jello_type_id r1[6] = {T_I32, T_BOOL, T_I32, T_I32, T_I32, T_DYN};
  jello_insn c1[] = {DECLINE(5), DECLINE(5), I(JOP_EQ_I32_IMM, 1, 0, 0, 0),
                     I(JOP_JMP_IF, 1, 0, 0, 4), I(JOP_SUB_I32_IMM, 2, 0, 0, 1), I(JOP_CALL, 3, 2, 1, 2),
                     I(JOP_ADD_I32, 4, 0, 3, 0), I(JOP_RET, 4, 0, 0, 0), I(JOP_CONST_I32, 4, 0, 0, 0),
                     I(JOP_RET, 4, 0, 0, 0)};
  jello_type_id r2[5] = {T_I32, T_BOOL, T_I32, T_I32, T_I32};
  jello_insn c2[] = {I(JOP_EQ_I32_IMM, 1, 0, 0, 0), I(JOP_JMP_IF, 1, 0, 0, 4), I(JOP_SUB_I32_IMM, 2, 0, 0, 1),
                     I(JOP_CALL, 3, 2, 1, 1), I(JOP_ADD_I32, 4, 0, 3, 0), I(JOP_RET, 4, 0, 0, 0),
                     I(JOP_CONST_I32, 4, 0, 0, 0), I(JOP_RET, 4, 0, 0, 0)};
  memcpy(a->r0, r0, sizeof(r0)); memcpy(a->r1, r1, sizeof(r1)); memcpy(a->r2, r2, sizeof(r2));
  memcpy(a->c0, c0, sizeof(c0)); memcpy(a->c1, c1, sizeof(c1)); memcpy(a->c2, c2, sizeof(c2));
  a->f[0] = F(a->r0, 2, a->c0, 3);
  a->f[1] = F(a->r1, 6, a->c1, 10);
  a->f[2] = F(a->r2, 5, a->c2, 8);
  a->m = M(a->f, 3);
}

/* macOS gives a secondary thread 512 KiB of stack, so entries must stop at
 * the stack floor and the rest run interpreted. */
typedef struct deep_run {
  int32_t result;
  uint64_t enters;
} deep_run;

static void* run_deep(void* arg) {
  deep_run* run = (deep_run*)arg;
  alternating a;
  build_alternating(&a, 20000);
  jello_vm* vm = jello_vm_create();
  assert(vm);
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, &a.m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  run->result = out.as.i32;
  run->enters = vm->jit_enters;
  jello_vm_destroy(vm);
  return NULL;
}

static void test_deep_crossings_on_a_small_stack(void) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  assert(pthread_attr_setstacksize(&attr, 512u * 1024u) == 0);
  pthread_t t;
  deep_run run = {0, 0};
  assert(pthread_create(&t, &attr, run_deep, &run) == 0);
  pthread_join(t, NULL);
  pthread_attr_destroy(&attr);
  assert(run.result == 200010000);
  /* 10000 levels would need several MiB of C stack if every one crossed. */
  if(jit_on()) assert(run.enters > 0 && run.enters < 10000);
  printf("test_deep_crossings_on_a_small_stack: PASS (%llu entries)\n", (unsigned long long)run.enters);
}

/* The same alternation through Function values: f0 -> f1 (declined) ->
 * f2 (compiled) -> f1 ..., every call a CALLR. */
typedef struct alternating_r {
  jello_type_id r0[3], r1[7], r2[6];
  jello_insn c0[4], c1[11], c2[9];
  jello_bc_function f[3];
  jello_bc_module m;
} alternating_r;

static void build_alternating_callr(alternating_r* a, uint32_t n) {
  jello_type_id r0[3] = {T_I32, T_I32, T_FUN};
  jello_insn c0[] = {I(JOP_CONST_I32, 0, 0, 0, n), I(JOP_CONST_FUN, 2, 0, 0, 1), I(JOP_CALLR, 1, 2, 1, 0),
                     I(JOP_RET, 1, 0, 0, 0)};
  jello_type_id r1[7] = {T_I32, T_BOOL, T_I32, T_I32, T_I32, T_DYN, T_FUN};
  jello_insn c1[] = {DECLINE(5), DECLINE(5), I(JOP_EQ_I32_IMM, 1, 0, 0, 0),
                     I(JOP_JMP_IF, 1, 0, 0, 5), I(JOP_SUB_I32_IMM, 2, 0, 0, 1), I(JOP_CONST_FUN, 6, 0, 0, 2),
                     I(JOP_CALLR, 3, 6, 1, 2), I(JOP_ADD_I32, 4, 0, 3, 0), I(JOP_RET, 4, 0, 0, 0),
                     I(JOP_CONST_I32, 4, 0, 0, 0), I(JOP_RET, 4, 0, 0, 0)};
  jello_type_id r2[6] = {T_I32, T_BOOL, T_I32, T_I32, T_I32, T_FUN};
  jello_insn c2[] = {I(JOP_EQ_I32_IMM, 1, 0, 0, 0), I(JOP_JMP_IF, 1, 0, 0, 5), I(JOP_SUB_I32_IMM, 2, 0, 0, 1),
                     I(JOP_CONST_FUN, 5, 0, 0, 1), I(JOP_CALLR, 3, 5, 1, 2), I(JOP_ADD_I32, 4, 0, 3, 0),
                     I(JOP_RET, 4, 0, 0, 0), I(JOP_CONST_I32, 4, 0, 0, 0), I(JOP_RET, 4, 0, 0, 0)};
  memcpy(a->r0, r0, sizeof(r0)); memcpy(a->r1, r1, sizeof(r1)); memcpy(a->r2, r2, sizeof(r2));
  memcpy(a->c0, c0, sizeof(c0)); memcpy(a->c1, c1, sizeof(c1)); memcpy(a->c2, c2, sizeof(c2));
  a->f[0] = F(a->r0, 3, a->c0, 4);
  a->f[1] = F(a->r1, 7, a->c1, 11);
  a->f[2] = F(a->r2, 6, a->c2, 9);
  a->m = M(a->f, 3);
}

/* Run m with fuel 1..60 under the JIT and the interpreter: the same status,
 * and on a fuel trap the same function and pc. Returns the JIT entries. */
static uint64_t fuel_sweep(jello_vm* vm, const jello_bc_module* m, int32_t expect) {
  uint64_t enters = 0;
  for(uint64_t fuel = 1; fuel <= 60; fuel++) {
    jello_numeric_result out;
    jello_trap_code code[2];
    uint32_t fi[2], pc[2];
    jello_exec_status st[2];
    for(int interp = 0; interp < 2; interp++) {
      if(interp) jello_setenv("JELLO_JIT_DISABLE", "1");
      jello_vm_set_fuel(vm, fuel);
      st[interp] = jello_vm_exec_numeric(vm, m, &out);
      code[interp] = jello_vm_last_trap_code(vm);
      fi[interp] = jello_vm_last_trap_func_index(vm);
      pc[interp] = jello_vm_last_trap_pc(vm);
      if(interp) jello_unsetenv("JELLO_JIT_DISABLE");
    }
    assert(st[0] == st[1]);
    if(st[0] == JELLO_EXEC_TRAP) assert(code[0] == JELLO_TRAP_FUEL && code[1] == JELLO_TRAP_FUEL &&
                                        fi[0] == fi[1] && pc[0] == pc[1]);
    else assert(out.as.i32 == expect);
    enters = vm->jit_enters;
  }
  return enters;
}

/* Interpreted CALLs and CALLRs and the JIT's safepoints before them each
 * charge one unit, so fuel runs out at the same call whichever side makes
 * it. */
static void test_fuel_runs_out_at_the_same_call(void) {
  alternating a;
  build_alternating(&a, 50);
  alternating_r ar;
  build_alternating_callr(&ar, 50);
  jello_vm* vm = jello_vm_create();
  assert(vm);
  uint64_t enters = fuel_sweep(vm, &a.m, 1275);
  if(jit_on()) assert(enters > 0);
  uint64_t enters_r = fuel_sweep(vm, &ar.m, 1275);
  if(jit_on()) assert(enters_r > enters);

  /* THROW is charged on both sides too: f1(n) recurses inside a try region,
   * throws at the bottom, and every handler adds its n and rethrows, until
   * the interpreted f0 catches the sum. */
  jello_type_id r0[3] = {T_DYN, T_I32, T_I32};
  jello_insn c0[] = {DECLINE(0), DECLINE(0), I(JOP_CONST_I32, 1, 0, 0, 10),
                     I(JOP_TRY, 0, 0, 0, 3), I(JOP_CALL, 2, 1, 1, 1), I(JOP_ENDTRY, 0, 0, 0, 0),
                     I(JOP_RET, 2, 0, 0, 0), I(JOP_FROM_DYN_I32, 2, 0, 0, 0), I(JOP_RET, 2, 0, 0, 0)};
  jello_type_id r1[6] = {T_I32, T_BOOL, T_I32, T_I32, T_DYN, T_DYN};
  jello_insn c1[] = {I(JOP_TRY, 5, 0, 0, 9), I(JOP_EQ_I32_IMM, 1, 0, 0, 0), I(JOP_JMP_IF, 1, 0, 0, 4),
                     I(JOP_SUB_I32_IMM, 2, 0, 0, 1), I(JOP_CALL, 3, 2, 1, 1), I(JOP_ENDTRY, 0, 0, 0, 0),
                     I(JOP_RET, 3, 0, 0, 0), I(JOP_CONST_I32, 3, 0, 0, 0), I(JOP_TO_DYN, 4, 3, 0, 0),
                     I(JOP_THROW, 4, 0, 0, 0), I(JOP_FROM_DYN_I32, 3, 5, 0, 0), I(JOP_ADD_I32, 3, 3, 0, 0),
                     I(JOP_TO_DYN, 4, 3, 0, 0), I(JOP_THROW, 4, 0, 0, 0)};
  jello_bc_function ft[2] = {F(r0, 3, c0, 9), F(r1, 6, c1, 14)};
  ft[1].nargs = 1;
  jello_bc_module mt = M(ft, 2);
  uint64_t enters_t = fuel_sweep(vm, &mt, 55);
  if(jit_on()) assert(enters_t > enters_r);
  jello_vm_destroy(vm);
  printf("test_fuel_runs_out_at_the_same_call: PASS\n");
}

/* The validator only lets a Function register reach CALLR, so a wrong-kind
 * callee needs a hand-built module: f0 (interpreted, it declines) puts
 * Bytes in a Function register and passes it to f1, which runs as JIT code
 * and CALLRs it. Both sides must refuse it the same way. */
static void test_callr_refuses_a_non_function(void) {
  jello_type_id r0[4] = {T_FUN, T_I32, T_I32, T_DYN};
  jello_insn c0[] = {I(JOP_CONST_I32, 1, 0, 0, 4), I(JOP_BYTES_NEW, 0, 1, 0, 0), DECLINE(3),
                     DECLINE(3), I(JOP_CALL, 2, 0, 1, 1), I(JOP_RET, 2, 0, 0, 0)};
  jello_type_id r1[3] = {T_FUN, T_I32, T_I32};
  jello_insn c1[] = {I(JOP_CONST_I32, 1, 0, 0, 5), I(JOP_CALLR, 2, 0, 1, 1), I(JOP_RET, 2, 0, 0, 0)};
  jello_bc_function f[2] = {F(r0, 4, c0, 6), F(r1, 3, c1, 3)};
  f[1].nargs = 1;
  jello_bc_module m = M(f, 2);
  jello_vm* vm = jello_vm_create();
  assert(vm);
  for(int interp = 0; interp < 2; interp++) {
    if(interp) jello_setenv("JELLO_JIT_DISABLE", "1");
    jello_numeric_result out;
    assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_TRAP);
    assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_TYPE_MISMATCH);
    assert(jello_vm_last_trap_func_index(vm) == 1 && jello_vm_last_trap_pc(vm) == 1);
    if(interp) jello_unsetenv("JELLO_JIT_DISABLE");
    else if(jit_on()) assert(vm->jit_enters == 1);
  }
  jello_vm_destroy(vm);
  printf("test_callr_refuses_a_non_function: PASS\n");
}

/* A VM keeps its frame layouts across runs, and the JIT's per-function
 * index into them survives an interpreted run of another module that grows
 * (and moves) the layout array; interpreted calls use that index. */
static void test_layouts_survive_another_module(void) {
  alternating a;
  build_alternating(&a, 30);
  enum { N = 12 };
  jello_type_id rb[2] = {T_I32, T_I32};
  jello_insn cb[N][3];
  jello_bc_function fb[N] = {0};
  for(uint32_t i = 0; i < N; i++) {
    cb[i][0] = i ? I(JOP_ADD_I32_IMM, 0, 0, 0, 1) : I(JOP_CONST_I32, 0, 0, 0, 1);
    cb[i][1] = i + 1 < N ? I(JOP_CALL, 1, 0, 1, i + 1) : I(JOP_MOV, 1, 0, 0, 0);
    cb[i][2] = I(JOP_RET, 1, 0, 0, 0);
    fb[i] = F(rb, 2, cb[i], 3);
  }
  jello_bc_module mb = M(fb, N);
  jello_vm* vm = jello_vm_create();
  assert(vm);
  jello_numeric_result out;
  for(int run = 0; run < 3; run++) {
    if(run == 1) jello_setenv("JELLO_JIT_DISABLE", "1");
    assert(jello_vm_exec_numeric(vm, &a.m, &out) == JELLO_EXEC_OK && out.as.i32 == 465);
    assert(jello_vm_exec_numeric(vm, &mb, &out) == JELLO_EXEC_OK && out.as.i32 == N);
    if(run == 1) jello_unsetenv("JELLO_JIT_DISABLE");
  }
  jello_vm_destroy(vm);
  printf("test_layouts_survive_another_module: PASS\n");
}

/* Each function's entry-table slot matches what happened to it: its body
 * once compiled, the bridge once declined, the lazy stub until first
 * called. */
static void assert_entries_consistent(const jello_jit_module* jm) {
  for(uint32_t fi = 0; fi < jm->nfuncs; fi++) {
    const jello_jit_func_code* fc = &jm->funcs[fi];
    const uint8_t* want = jm->blob.mem + (fc->compiled ? fc->blob_off + fc->body_off
                                          : fc->tried ? jm->bridge_off : jm->lazy_off);
    assert(jm->entries[fi] == want);
  }
}

/* Functions compile when first called -- by JIT code, through CALLR, or
 * by the interpreter -- and only then. f0 (entry) CALLRs f1, which only a
 * JIT CALLR ever reaches; f2 is never called. */
static void test_functions_compile_when_first_called(void) {
  if(!jit_on()) {
    printf("test_functions_compile_when_first_called: SKIPPED (no JIT)\n");
    return;
  }
  jello_type_id r0[3] = {T_I32, T_FUN, T_I32};
  jello_insn c0[] = {I(JOP_CONST_I32, 0, 0, 0, 41), I(JOP_CONST_FUN, 1, 0, 0, 1), I(JOP_CALLR, 2, 1, 1, 0),
                     I(JOP_RET, 2, 0, 0, 0)};
  jello_type_id r1[2] = {T_I32, T_I32};
  jello_insn c1[] = {I(JOP_ADD_I32_IMM, 1, 0, 0, 1), I(JOP_RET, 1, 0, 0, 0)};
  jello_insn c2[] = {I(JOP_CONST_I32, 1, 0, 0, 0), I(JOP_RET, 1, 0, 0, 0)};
  jello_bc_function f[3] = {F(r0, 3, c0, 4), F(r1, 2, c1, 2), F(r1, 2, c2, 2)};
  jello_bc_module m = M(f, 3);
  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm && jm->ncompiled == 0);
  assert_entries_consistent(jm);
  jello_vm* vm = jello_vm_create();
  assert(vm);
  jello_numeric_result out;
  for(int run = 0; run < 2; run++) {
    assert(jello_vm_exec_numeric_with_jit(vm, jm, &m, &out) == JELLO_EXEC_OK && out.as.i32 == 42);
    assert(jm->funcs[0].compiled && jm->funcs[1].compiled && !jm->funcs[2].tried);
    assert(jm->ncompiled == 2);
    assert_entries_consistent(jm);
  }
  assert(vm->jit_bridges == 0);
  jello_vm_destroy(vm);
  jello_jit_module_destroy(jm);

  /* The mixed alternation: f1 declines, so its slot is the bridge. */
  alternating a;
  build_alternating(&a, 30);
  jm = jello_jit_module_create(&a.m);
  vm = jello_vm_create();
  assert(jm && vm);
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &a.m, &out) == JELLO_EXEC_OK && out.as.i32 == 465);
  assert(jm->funcs[1].tried && !jm->funcs[1].compiled && jm->funcs[2].compiled);
  assert_entries_consistent(jm);
  jello_vm_destroy(vm);
  jello_jit_module_destroy(jm);
  printf("test_functions_compile_when_first_called: PASS\n");
}

/* A lazily compiled function that outgrows its append window is emitted
 * again over the rest of the buffer; nothing is written past the window
 * (which would fault) and the result matches the interpreter. */
static void test_compile_outgrowing_its_window(void) {
  if(!jit_on()) {
    printf("test_compile_outgrowing_its_window: SKIPPED (no JIT)\n");
    return;
  }
  jit_append_window_bytes = 64u;
  alternating_r ar;
  build_alternating_callr(&ar, 40);
  jello_jit_module* jm = jello_jit_module_create(&ar.m);
  jello_vm* vm = jello_vm_create();
  assert(jm && vm);
  jello_numeric_result out;
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &ar.m, &out) == JELLO_EXEC_OK && out.as.i32 == 820);
  assert(jm->funcs[0].compiled && jm->funcs[2].compiled);
  assert_entries_consistent(jm);
  jit_append_window_bytes = 0;
  jello_vm_destroy(vm);
  jello_jit_module_destroy(jm);
  printf("test_compile_outgrowing_its_window: PASS\n");
}

/* JIT code spills through the interpreter's handlers, so its frames own
 * spill slots like interpreted ones. f1 (compiled) spills a Bytes value and
 * returns; f2 (compiled), in the frame f1 left, pops the same slot before
 * any push, which must read null, not f1's value. Every frame left behind
 * must hold no slots for the collector to scan. */
static void test_spill_slots_end_with_their_frame(void) {
  jello_type_id r0[3] = {T_I32, T_I32, T_BOOL};
  jello_insn c0[] = {I(JOP_CALL, 1, 0, 0, 1), I(JOP_CALL, 2, 0, 0, 2), I(JOP_JMP_IF, 2, 0, 0, 1),
                     I(JOP_CONST_I32, 1, 0, 0, 0), I(JOP_RET, 1, 0, 0, 0)};
  jello_type_id r1[3] = {T_I32, T_BYTES, T_I32};
  jello_insn c1[] = {I(JOP_CONST_I32, 0, 0, 0, 4), I(JOP_BYTES_NEW, 1, 0, 0, 0), I(JOP_SPILL_PUSH, 1, 0, 0, 3),
                     I(JOP_SPILL_POP, 1, 0, 0, 3), I(JOP_BYTES_LEN, 2, 1, 0, 0), I(JOP_RET, 2, 0, 0, 0)};
  jello_type_id r2[2] = {T_DYN, T_BOOL};
  jello_insn c2[] = {I(JOP_SPILL_POP, 0, 0, 0, 3), I(JOP_IS_NULL, 1, 0, 0, 0), I(JOP_RET, 1, 0, 0, 0)};
  jello_bc_function f[3] = {F(r0, 3, c0, 5), F(r1, 3, c1, 6), F(r2, 2, c2, 3)};
  jello_bc_module m = M(f, 3);
  jello_jit_module* jm = jit_on() ? jello_jit_module_create(&m) : NULL;
  jello_vm* vm = jello_vm_create();
  assert(vm);
  jello_numeric_result out;
  for(int run = 0; run < 2; run++) {
    jello_exec_status st = jm ? jello_vm_exec_numeric_with_jit(vm, jm, &m, &out) : jello_vm_exec_numeric(vm, &m, &out);
    assert(st == JELLO_EXEC_OK && out.as.i32 == 4);
    if(jm) assert(jm->funcs[0].compiled && jm->funcs[1].compiled && jm->funcs[2].compiled);
    for(uint32_t i = 0; i < vm->call_frames_cap; i++)
      assert(vm->call_frames[i].spill_slots == NULL && vm->call_frames[i].spill_cap == 0);
  }
  jello_vm_destroy(vm);
  if(jm) jello_jit_module_destroy(jm);
  printf("test_spill_slots_end_with_their_frame: PASS\n");
}

int main(void) {
  test_rerun_after_trap_with_live_try();
  test_fuel_runs_out_at_the_same_call();
  test_deep_crossings_on_a_small_stack();
  test_callr_refuses_a_non_function();
  test_layouts_survive_another_module();
  test_functions_compile_when_first_called();
  test_compile_outgrowing_its_window();
  test_spill_slots_end_with_their_frame();
  return 0;
}
