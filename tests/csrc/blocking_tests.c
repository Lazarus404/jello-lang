// SPDX-License-Identifier: BSD-3-Clause
// Processes calling blocking exports, interpreted and as JIT code, on one scheduler: each parks
// while the blocking pool makes its call, so their calls overlap and the main process's does too;
// waiting for one is no deadlock, and one stopped meanwhile finishes stopped once its call returns,
// its result dropped.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jello/internal.h>
#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/platform.h>

enum { T_I32, T_BOOL, T_DYN, NTYPES };
enum { N_NAP, N_NAP_HANDLE, N_FINALIZED, NIMPORTS };
enum { NAP_MS = 100, NAPPERS = 8 };

static jello_insn ins(jello_op op, uint8_t a, uint8_t b, uint8_t c, int32_t imm) {
  jello_insn i = {0};
  i.op = (uint8_t)op;
  i.a = a;
  i.b = b;
  i.c = c;
  i.imm = (uint32_t)imm;
  return i;
}

static void import(jello_import_sig* sig, jello_jdll_handle* lib, const char* name, uint8_t ret,
                   uint8_t arity) {
  sig->ret_kind = ret;
  sig->arity = arity;
  sig->arg_kinds[0] = (uint8_t)JELLO_T_I32;
  sig->resolved = jello_jdll_find_export(lib, name);
  assert(sig->resolved);
}

/* func 0, the entry: returns 0.
 * func 1: naps, then returns 1.
 * func 2: naps, then returns the handle the nap made.
 * func 3: returns how many of those handles have been finalised. */
static jello_bc_module* build_module(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(NTYPES, sizeof(jello_type_entry));
  types[T_I32].kind = JELLO_T_I32;
  types[T_BOOL].kind = JELLO_T_BOOL;
  types[T_DYN].kind = JELLO_T_DYNAMIC;
  m->ntypes = NTYPES;
  m->types = types;

  jello_jdll_handle* lib = jello_jdll_load("test_blocking");
  assert(lib);
  jello_import_sig* imports = (jello_import_sig*)calloc(NIMPORTS, sizeof(jello_import_sig));
  import(&imports[N_NAP], lib, "nap_ms", (uint8_t)JELLO_T_BOOL, 1);
  import(&imports[N_NAP_HANDLE], lib, "nap_handle_ms", (uint8_t)JELLO_T_DYNAMIC, 1);
  import(&imports[N_FINALIZED], lib, "finalized", (uint8_t)JELLO_T_I32, 0);
  m->nimports = NIMPORTS;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = lib;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_bc_function* funcs = (jello_bc_function*)calloc(4, sizeof(jello_bc_function));
  static const jello_type_id i32_types[1] = {T_I32};
  jello_insn* entry = (jello_insn*)calloc(2, sizeof(jello_insn));
  entry[0] = ins(JOP_CONST_I32, 0, 0, 0, 0);
  entry[1] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[0] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, i32_types, 2, entry, 0, NULL};

  static const jello_type_id nap_types[2] = {T_I32, T_BOOL};
  jello_insn* napper = (jello_insn*)calloc(4, sizeof(jello_insn));
  napper[0] = ins(JOP_CONST_I32, 0, 0, 0, NAP_MS);
  napper[1] = ins(JOP_CALL_NATIVE, 1, 0, 1, N_NAP);
  napper[2] = ins(JOP_CONST_I32, 0, 0, 0, 1);
  napper[3] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[1] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 2, 2, nap_types, 4, napper, 0, NULL};

  static const jello_type_id handle_types[2] = {T_I32, T_DYN};
  jello_insn* holder = (jello_insn*)calloc(3, sizeof(jello_insn));
  holder[0] = ins(JOP_CONST_I32, 0, 0, 0, NAP_MS);
  holder[1] = ins(JOP_CALL_NATIVE, 1, 0, 1, N_NAP_HANDLE);
  holder[2] = ins(JOP_RET, 1, 0, 0, 0);
  funcs[2] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 2, 2, handle_types, 3, holder, 0, NULL};

  jello_insn* count = (jello_insn*)calloc(2, sizeof(jello_insn));
  count[0] = ins(JOP_CALL_NATIVE, 0, 0, 0, N_FINALIZED);
  count[1] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[3] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, i32_types, 2, count, 0, NULL};

  m->nfuncs = 4;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static jello_process* spawn(jello_vm* vm, uint32_t fi) {
  jello_function* body = jello_function_new(vm, fi, 0);
  jello_trap_code trap;
  jello_process* p = proc_spawn(vm, body, NULL, 0, &trap);
  assert(p);
  return p;
}

static double ms_since(uint64_t t0) {
  return (double)(proc_now() - t0) / 1e6;
}

/* The finalised count, as func 3 reads it on the main process. */
static int32_t finalized(jello_vm* vm) {
  jello_function* fn = jello_function_new(vm, 3, 0);
  jello_numeric_result out;
  assert(vm_exec_function(vm, fn, NULL, 0, &out) == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32);
  return out.as.i32;
}

/* Why `p` is parked, read as its pool thread writes it. */
static uint8_t parked(jello_process* p) {
  pthread_mutex_lock(&p->mu);
  uint8_t why = p->parked;
  pthread_mutex_unlock(&p->mu);
  return why;
}

/* A main process's own blocking call, made with no module code. */
static void main_nap(jdlo_ctx* c) {
  (void)c;
  usleep(2u * NAP_MS * 1000u);
}

static void check(jello_vm* vm, const char* mode) {
  /* Their naps overlap: one scheduler, yet nowhere near NAPPERS of them end to end. */
  jello_process* ps[NAPPERS];
  uint64_t t0 = proc_now();
  for(int i = 0; i < NAPPERS; i++) ps[i] = spawn(vm, 1);
  proc_wait_all(vm);
  double took = ms_since(t0);
  for(int i = 0; i < NAPPERS; i++) {
    assert(proc_exit_of(ps[i]) == PROC_NORMAL && ps[i]->result.as.i32 == 1);
    proc_release(ps[i]);
  }
  assert(took < NAPPERS * NAP_MS * 3 / 4 && "processes' blocking calls overlap");

  /* Joining a process in a blocking call waits for it; nothing has deadlocked. */
  jello_process* p = spawn(vm, 1);
  assert(proc_join(vm, p) == JELLO_TRAP_NONE && proc_exit_of(p) == PROC_NORMAL);
  proc_release(p);

  /* The main process's own blocking call lets the others run meanwhile. */
  t0 = proc_now();
  for(int i = 0; i < NAPPERS; i++) ps[i] = spawn(vm, 1);
  assert(proc_call_blocking(vm, main_nap, NULL) == JELLO_TRAP_NONE);
  proc_wait_all(vm);
  took = ms_since(t0);
  for(int i = 0; i < NAPPERS; i++) {
    assert(proc_exit_of(ps[i]) == PROC_NORMAL);
    proc_release(ps[i]);
  }
  assert(took < 4 * NAP_MS && "the main process's call overlaps the others'");

  /* Stopped in a blocking call, a process finishes stopped once the call returns, and the handle
   * it returned is finalised, never seen. */
  int32_t before = finalized(vm);
  p = spawn(vm, 2);
  proc_yield(vm);
  assert(parked(p) == PROC_PARK_NATIVE);
  assert(proc_stop(p, PROC_STOPPED) && !proc_finished(p) && parked(p) == PROC_PARK_NATIVE);
  proc_wait_all(vm);
  assert(proc_exit_of(p) == PROC_STOPPED);
  assert(finalized(vm) == before + 1);
  proc_release(p);
  printf("  %s: passed\n", mode);
}

int main(void) {
  jello_setenv("JELLO_SCHEDULERS", "1"); /* Overlap must come from the pool, not other schedulers. */
  jello_bc_module* m = build_module();
  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, m, &out) == JELLO_EXEC_OK);
  assert(proc_self(vm));
  check(vm, "interpreted");

  jello_jit_module jm;
  memset(&jm, 0, sizeof jm);
  if(jello_jit_available() && jello_jit_module_prepare(m, &jm) == 1) {
    vm->active_jit = &jm;
    check(vm, "JIT");
    vm->active_jit = NULL;
    jello_jit_module_free(&jm);
  }

  proc_end_all(vm);
  jello_vm_destroy(vm);

  /* A library built for an earlier ABI is refused, with what to do about it. */
  assert(!jello_jdll_load("test_old_abi"));
  assert(strstr(jello_jdll_last_error(), "was built for ABI 3; rebuild it"));
  printf("blocking_tests: passed\n");
  return 0;
}
