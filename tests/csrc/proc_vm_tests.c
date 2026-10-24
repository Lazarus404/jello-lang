// SPDX-License-Identifier: BSD-3-Clause
// Processes running Jello code on their own slots under one scheduler, interpreted and as JIT
// code: results, preemption, collecting while others are switched out, stopping, fuel, the
// call-frame limit their native stacks imply, many processes, and ending with processes live.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jello/internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/platform.h>

enum { T_I32, T_BOOL, T_BYTES };

/* func 0, the entry: returns 0.
 * func 1, count(n): n == 0 ? 0 : count(n - 1) + 1, recursing n deep.
 * func 2, the body: count(DEPTH), with DEPTH its capture.
 * func 3, a body allocating 200,000 Bytes of 64 bytes, enough to collect, then returning 7.
 * func 4, a body holding a Bytes of 64 bytes, its byte 0 set to 0x5A, across 2,000,000 loop
 *   iterations that never allocate, then returning byte 0.
 * func 5, a body looping forever. */
static jello_bc_module* build_module(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(3, sizeof(jello_type_entry));
  types[T_I32].kind = JELLO_T_I32;
  types[T_BOOL].kind = JELLO_T_BOOL;
  types[T_BYTES].kind = JELLO_T_BYTES;
  m->ntypes = 3;
  m->types = types;
  jello_bc_function* funcs = (jello_bc_function*)calloc(6, sizeof(jello_bc_function));

  static const jello_type_id entry_types[1] = {T_I32};
  static jello_insn entry[2];
  entry[0].op = (uint8_t)JOP_CONST_I32; entry[0].a = 0; entry[0].imm = 0;
  entry[1].op = (uint8_t)JOP_RET; entry[1].a = 0;
  funcs[0] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, entry_types, 2, entry, 0, NULL};

  static const jello_type_id count_types[4] = {T_I32, T_BOOL, T_I32, T_I32};
  static jello_insn count[7];
  count[0].op = (uint8_t)JOP_EQ_I32_IMM; count[0].a = 1; count[0].b = 0; count[0].imm = 0;
  count[1].op = (uint8_t)JOP_JMP_IF; count[1].a = 1; count[1].imm = 4; /* to 6 */
  count[2].op = (uint8_t)JOP_SUB_I32_IMM; count[2].a = 2; count[2].b = 0; count[2].imm = 1;
  count[3].op = (uint8_t)JOP_CALL; count[3].a = 3; count[3].b = 2; count[3].c = 1; count[3].imm = 1;
  count[4].op = (uint8_t)JOP_ADD_I32_IMM; count[4].a = 3; count[4].b = 3; count[4].imm = 1;
  count[5].op = (uint8_t)JOP_RET; count[5].a = 3;
  count[6].op = (uint8_t)JOP_RET; count[6].a = 0;
  funcs[1] = (jello_bc_function){JELLO_BC_NO_NAME, 1, 4, 4, count_types, 7, count, 0, NULL};

  static const jello_type_id body_types[2] = {T_I32, T_I32};
  static jello_insn body[2];
  body[0].op = (uint8_t)JOP_CALL; body[0].a = 1; body[0].b = 0; body[0].c = 1; body[0].imm = 1;
  body[1].op = (uint8_t)JOP_RET; body[1].a = 1;
  funcs[2] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 2, 0, body_types, 2, body, 0, NULL};

  static const jello_type_id alloc_types[4] = {T_I32, T_I32, T_BYTES, T_BOOL};
  static jello_insn alloc[8];
  alloc[0].op = (uint8_t)JOP_CONST_I32; alloc[0].a = 0; alloc[0].imm = 0;
  alloc[1].op = (uint8_t)JOP_CONST_I32; alloc[1].a = 1; alloc[1].imm = 64;
  alloc[2].op = (uint8_t)JOP_BYTES_NEW; alloc[2].a = 2; alloc[2].b = 1;
  alloc[3].op = (uint8_t)JOP_ADD_I32_IMM; alloc[3].a = 0; alloc[3].b = 0; alloc[3].imm = 1;
  alloc[4].op = (uint8_t)JOP_LT_I32_IMM; alloc[4].a = 3; alloc[4].b = 0; alloc[4].imm = 200000;
  alloc[5].op = (uint8_t)JOP_JMP_IF; alloc[5].a = 3; alloc[5].imm = (uint32_t)(int32_t)-4; /* to 2 */
  alloc[6].op = (uint8_t)JOP_CONST_I32; alloc[6].a = 0; alloc[6].imm = 7;
  alloc[7].op = (uint8_t)JOP_RET; alloc[7].a = 0;
  funcs[3] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 4, 4, alloc_types, 8, alloc, 0, NULL};

  static const jello_type_id hold_types[6] = {T_I32, T_I32, T_BYTES, T_BOOL, T_I32, T_I32};
  static jello_insn hold[11];
  hold[0].op = (uint8_t)JOP_CONST_I32; hold[0].a = 1; hold[0].imm = 64;
  hold[1].op = (uint8_t)JOP_BYTES_NEW; hold[1].a = 2; hold[1].b = 1;
  hold[2].op = (uint8_t)JOP_CONST_I32; hold[2].a = 4; hold[2].imm = 0;
  hold[3].op = (uint8_t)JOP_CONST_I32; hold[3].a = 5; hold[3].imm = 0x5A;
  hold[4].op = (uint8_t)JOP_BYTES_SET_U8; hold[4].a = 5; hold[4].b = 2; hold[4].c = 4;
  hold[5].op = (uint8_t)JOP_CONST_I32; hold[5].a = 0; hold[5].imm = 0;
  hold[6].op = (uint8_t)JOP_ADD_I32_IMM; hold[6].a = 0; hold[6].b = 0; hold[6].imm = 1;
  hold[7].op = (uint8_t)JOP_LT_I32_IMM; hold[7].a = 3; hold[7].b = 0; hold[7].imm = 2000000;
  hold[8].op = (uint8_t)JOP_JMP_IF; hold[8].a = 3; hold[8].imm = (uint32_t)(int32_t)-3; /* to 6 */
  hold[9].op = (uint8_t)JOP_BYTES_GET_U8; hold[9].a = 0; hold[9].b = 2; hold[9].c = 4;
  hold[10].op = (uint8_t)JOP_RET; hold[10].a = 0;
  funcs[4] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 6, 6, hold_types, 11, hold, 0, NULL};

  static const jello_type_id spin_types[1] = {T_I32};
  static jello_insn spin[3];
  spin[0].op = (uint8_t)JOP_CONST_I32; spin[0].a = 0; spin[0].imm = 0;
  spin[1].op = (uint8_t)JOP_JMP; spin[1].imm = (uint32_t)(int32_t)-1; /* to 1 */
  spin[2].op = (uint8_t)JOP_RET; spin[2].a = 0;
  funcs[5] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, spin_types, 3, spin, 0, NULL};

  m->nfuncs = 6;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

/* A process running func `fi`, with `capture` as its one capture when it is count's body. */
static jello_process* spawn(jello_vm* vm, uint32_t fi, int32_t capture, proc_pool* pool) {
  jello_function* body = jello_function_new(vm, fi, fi == 2 ? 1 : 0);
  if(fi == 2) body->captures[0] = (uint64_t)(uint32_t)capture;
  jello_trap_code trap;
  jello_process* p = proc_spawn(vm, body, pool, 0, &trap);
  assert(p);
  return p;
}

/* Checks how `p` finished, then drops the test's reference to it. */
static void assert_returned(jello_process* p, int32_t want) {
  assert(!p->vm && proc_exit_of(p) == PROC_NORMAL);
  assert(p->result.kind == JELLO_NUM_I32 && p->result.as.i32 == want);
  proc_release(p);
}

static void assert_trapped(jello_process* p, jello_trap_code want) {
  assert(!p->vm && proc_exit_of(p) != PROC_NORMAL && p->trap == want);
  proc_release(p);
}

/* Processes take turns: each holds a Bytes across a loop that never allocates while others
 * collect their own spaces, so each is preempted with that Bytes live, and it must survive. The
 * main process's own root survives too. */
static void check_preempt_collect(jello_vm* vm) {
  jello_bytes* kept = jello_bytes_new(vm, 16);
  memset(kept->data, 0x5A, 16);
  jello_gc_push_root(vm, kept);
  uint32_t before = vm->heap->collections;
  jello_process* holders[4];
  jello_process* allocs[2];
  for(int i = 0; i < 4; i++) holders[i] = spawn(vm, 4, 0, NULL);
  for(int i = 0; i < 2; i++) allocs[i] = spawn(vm, 3, 0, NULL);
  uint64_t switches = proc_switches(vm->proc->sched);
  proc_wait_all(vm);
  assert(vm->heap->collections > before && "the processes collected");
  assert(proc_switches(vm->proc->sched) - switches > 6 * 10 && "the processes were preempted");
  for(int i = 0; i < 4; i++) assert_returned(holders[i], 0x5A);
  for(int i = 0; i < 2; i++) assert_returned(allocs[i], 7);
  for(int i = 0; i < 16; i++) assert(kept->data[i] == 0x5A);
  jello_gc_pop_roots(vm, 1);
}

/* A stopped process traps STOPPED where it resumes, even one that never finishes, and one stopped
 * before it starts never runs. */
static void check_stop(jello_vm* vm) {
  jello_process* spinner = spawn(vm, 5, 0, NULL);
  jello_process* counter = spawn(vm, 2, 100, NULL);
  proc_yield(vm);
  assert(spinner->vm->call_frames_len == 1 && "the spinner started");
  proc_stop(spinner, PROC_STOPPED);
  jello_process* unstarted = spawn(vm, 5, 0, NULL);
  proc_stop(unstarted, PROC_STOPPED);
  proc_wait_all(vm);
  assert_trapped(spinner, JELLO_TRAP_STOPPED);
  assert_trapped(unstarted, JELLO_TRAP_STOPPED);
  assert_returned(counter, 100);
}

/* A process gets the fuel its parent has left, less what the parent has used in its current
 * window, and a process of a parent with none left traps at once rather than running unlimited. */
static void check_fuel(jello_vm* vm) {
  jello_vm_set_fuel(vm, 1500);
  vm->reductions -= 1000;
  jello_process* p = spawn(vm, 3, 0, NULL);
  assert(p->vm->fuel_limit && p->vm->fuel_remaining == 500);
  vm->reductions = 0;
  jello_process* q = spawn(vm, 3, 0, NULL);
  jello_vm_set_fuel(vm, 0);
  proc_wait_all(vm);
  assert_trapped(p, JELLO_TRAP_FUEL);
  assert_trapped(q, JELLO_TRAP_FUEL);
}

/* With a native stack of under 96 KiB, at most 32 KiB above the reserve, recursion 5,000 deep
 * is more than JIT code, needing at least 16 bytes of native stack a level, can nest; its calls
 * past the limit carry on in the interpreter, so it returns all the same. */
static void check_native_limit(jello_vm* vm) {
  proc_pool small = proc_pool_make(96u * 1024u, PROC_FRAME_STACK_BYTES);
  jello_process* p = spawn(vm, 2, 5000, &small);
  proc_wait_all(vm);
  assert_returned(p, 5000);
  proc_pool_free(&small);
}

static void check(jello_vm* vm, const jello_bc_module* m, const char* mode) {
  check_preempt_collect(vm);
  check_stop(vm);
  check_fuel(vm);
  check_native_limit(vm);
  jello_process* deep[4] = {spawn(vm, 2, 500, NULL), spawn(vm, 2, 3000, NULL),
                            spawn(vm, 2, 100000, NULL), spawn(vm, 2, 1100000, NULL)};
  proc_wait_all(vm);
  assert_returned(deep[0], 500);
  assert_returned(deep[1], 3000);
  /* Past the slot's frame stack, in segments the process maps. */
  assert_returned(deep[2], 100000);
  /* Past the most call frames: a trap, not a crash. */
  assert_trapped(deep[3], JELLO_TRAP_STACK_OVERFLOW);
  jello_process* many[1000];
  for(int32_t i = 0; i < 1000; i++) many[i] = spawn(vm, 2, i, NULL);
  proc_wait_all(vm);
  for(int32_t i = 0; i < 1000; i++) assert_returned(many[i], i);
  /* Ending with processes still running stops them, and frees every slot and record, as does
   * starting a run, whose reset frees the layouts they share. */
  jello_process* spinners[6];
  for(int i = 0; i < 3; i++) spinners[i] = spawn(vm, 5, 0, NULL);
  proc_yield(vm);
  proc_end_all(vm);
  assert(!vm->proc);
  for(int i = 3; i < 6; i++) spinners[i] = spawn(vm, 5, 0, NULL);
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, m, &out) == JELLO_EXEC_OK && !vm->proc);
  for(int i = 0; i < 6; i++) assert_trapped(spinners[i], JELLO_TRAP_STOPPED);
  printf("  %s: passed\n", mode);
}

int main(void) {
  jello_setenv("JELLO_SCHEDULERS", "1"); /* These checks follow one scheduler's order. */
  jello_bc_module* m = build_module();
  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, m, &out) == JELLO_EXEC_OK);
  check(vm, m, "interpreted");

  jello_jit_module jm;
  memset(&jm, 0, sizeof jm);
  if(jello_jit_available() && jello_jit_module_prepare(m, &jm) == 1) {
    assert(jm.ncompiled == jm.nfuncs);
    vm->active_jit = &jm;
    uint32_t enters = jit_try_enter_count;
    check(vm, m, "JIT");
    assert(jit_try_enter_count > enters && "processes ran their bodies as JIT code");
    vm->active_jit = NULL;
    jello_jit_module_free(&jm);
  }

  jello_vm_destroy(vm);
  printf("proc_vm_tests: passed\n");
  return 0;
}
