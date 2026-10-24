// SPDX-License-Identifier: BSD-3-Clause
// Processes spawned on main, on four schedulers, interpreted, as JIT code a run prepares for
// itself and as JIT code the host shares: a run spawning them returns leaving them running, as
// its VM keeps its processes; none runs while the host is busy elsewhere, though another the run
// spawned does, and jello_run_for runs them on the host's thread, returning once they have all
// finished, or once its time is up even while one never parks, which runs on at the next call.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jello/internal.h>
#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/platform.h>

enum { T_I32, T_BOOL, T_FUN, T_DYN, NTYPES };

/* func 0, the entry: returns 0.
 * func 1, a body counting to 2,000,000, then returning 7.
 * func 2, a body looping forever.
 * func 3, an entry spawning func 1 on main three times and once as an ordinary process, then
 * returning 0. */
static jello_bc_module* build_module(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(NTYPES, sizeof(jello_type_entry));
  types[T_I32].kind = JELLO_T_I32;
  types[T_BOOL].kind = JELLO_T_BOOL;
  types[T_FUN].kind = JELLO_T_FUNCTION;
  types[T_DYN].kind = JELLO_T_DYNAMIC;
  m->ntypes = NTYPES;
  m->types = types;

  jello_jdll_handle* vm_lib = jello_jdll_load("vm");
  assert(vm_lib);
  static const char* const names[2] = {"proc_spawn_on_main", "proc_spawn"};
  jello_import_sig* imports = (jello_import_sig*)calloc(2, sizeof(jello_import_sig));
  for(int i = 0; i < 2; i++) {
    imports[i].ret_kind = (uint8_t)JELLO_T_DYNAMIC;
    imports[i].arity = 1;
    imports[i].arg_kinds[0] = (uint8_t)JELLO_T_DYNAMIC;
    imports[i].resolved = jello_jdll_find_export(vm_lib, names[i]);
    assert(imports[i].resolved);
  }
  m->nimports = 2;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = vm_lib;
  m->jdll_handles = handles;
  m->njdll_handles = 1;
  jello_bc_function* funcs = (jello_bc_function*)calloc(4, sizeof(jello_bc_function));

  static const jello_type_id entry_types[1] = {T_I32};
  static jello_insn entry[2];
  entry[0].op = (uint8_t)JOP_CONST_I32; entry[0].a = 0; entry[0].imm = 0;
  entry[1].op = (uint8_t)JOP_RET; entry[1].a = 0;
  funcs[0] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, entry_types, 2, entry, 0, NULL};

  static const jello_type_id count_types[2] = {T_I32, T_BOOL};
  static jello_insn count[6];
  count[0].op = (uint8_t)JOP_CONST_I32; count[0].a = 0; count[0].imm = 0;
  count[1].op = (uint8_t)JOP_ADD_I32_IMM; count[1].a = 0; count[1].b = 0; count[1].imm = 1;
  count[2].op = (uint8_t)JOP_LT_I32_IMM; count[2].a = 1; count[2].b = 0; count[2].imm = 2000000;
  count[3].op = (uint8_t)JOP_JMP_IF; count[3].a = 1; count[3].imm = (uint32_t)(int32_t)-3; /* to 1 */
  count[4].op = (uint8_t)JOP_CONST_I32; count[4].a = 0; count[4].imm = 7;
  count[5].op = (uint8_t)JOP_RET; count[5].a = 0;
  funcs[1] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 2, 2, count_types, 6, count, 0, NULL};

  static const jello_type_id spin_types[1] = {T_I32};
  static jello_insn spin[3];
  spin[0].op = (uint8_t)JOP_CONST_I32; spin[0].a = 0; spin[0].imm = 0;
  spin[1].op = (uint8_t)JOP_JMP; spin[1].imm = (uint32_t)(int32_t)-1; /* to 1 */
  spin[2].op = (uint8_t)JOP_RET; spin[2].a = 0;
  funcs[2] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, spin_types, 3, spin, 0, NULL};

  static const jello_type_id spawner_types[4] = {T_FUN, T_DYN, T_DYN, T_I32};
  static jello_insn spawner[8];
  spawner[0].op = (uint8_t)JOP_CLOSURE; spawner[0].a = 0; spawner[0].c = 0; spawner[0].imm = 1;
  spawner[1].op = (uint8_t)JOP_TO_DYN; spawner[1].a = 1; spawner[1].b = 0;
  for(int i = 2; i < 6; i++) {
    spawner[i].op = (uint8_t)JOP_CALL_NATIVE; spawner[i].a = 2; spawner[i].b = 1; spawner[i].c = 1;
    spawner[i].imm = i == 5 ? 1u : 0u;
  }
  spawner[6].op = (uint8_t)JOP_CONST_I32; spawner[6].a = 3; spawner[6].imm = 0;
  spawner[7].op = (uint8_t)JOP_RET; spawner[7].a = 3;
  funcs[3] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 4, 4, spawner_types, 8, spawner, 0, NULL};

  m->nfuncs = 4;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static uint64_t now_ms(void) {
  return proc_now() / 1000000u;
}

/* Runs the spawner on `vm`, as JIT code from `jm` when it is not NULL, with the JIT turned off
 * when `jit` is 0, then checks what it left running. */
static void check(jello_vm* vm, jello_bc_module* m, jello_jit_module* jm, int jit,
                  const char* mode) {
  enum { PINNED = 3 };
  if(!jit) jello_setenv("JELLO_JIT_DISABLE", "1");
  m->entry = 3;
  jello_numeric_result out;
  assert(jello_vm_exec_numeric_with_jit(vm, jm, m, &out) == JELLO_EXEC_OK);
  m->entry = 0;
  jello_unsetenv("JELLO_JIT_DISABLE");
  proc_sched* s = vm->proc->sched;

  /* The ordinary process finishes on another scheduler, leaving the pinned ones. */
  jello_process* pinned[PINNED];
  int n = 0;
  for(uint64_t until = now_ms() + 5000;; usleep(1000)) {
    pthread_mutex_lock(s->lock);
    n = 0;
    int others = 0;
    for(jello_process* p = s->live; p; p = p->live_next) {
      if(!p->pinned) others++;
      else if(n < PINNED) pinned[n++] = p;
    }
    if(!others) {
      for(int i = 0; i < n; i++) proc_retain(pinned[i]);
    }
    pthread_mutex_unlock(s->lock);
    if(!others) break;
    assert(now_ms() < until && "another scheduler ran the ordinary process");
  }
  assert(n == PINNED && "the run left its pinned processes running");
  for(int i = 0; i < PINNED; i++) assert(!atomic_load(&pinned[i]->done) && "it waited for main");
  while(jello_run_for(vm, 5)) {}
  for(int i = 0; i < PINNED; i++) {
    assert(!pinned[i]->vm && proc_exit_of(pinned[i]) == PROC_NORMAL);
    assert(pinned[i]->result.kind == JELLO_NUM_I32 && pinned[i]->result.as.i32 == 7);
    proc_release(pinned[i]);
  }

  jello_trap_code trap;
  jello_process* spinner =
      proc_spawn(vm, jello_function_new(vm, 2, 0), NULL, PROC_SPAWN_ON_MAIN, &trap);
  assert(spinner);
  uint64_t start = now_ms();
  assert(jello_run_for(vm, 20) && "the spinner is still running");
  assert(now_ms() - start < 1000 && "the host had its thread back once its time was up");
  assert(jello_run_for(vm, 20) && spinner->vm->call_frames_len == 1);
  proc_stop(spinner, PROC_STOPPED);
  while(jello_run_for(vm, 5)) {}
  assert(!spinner->vm && proc_exit_of(spinner) != PROC_NORMAL &&
         spinner->trap == JELLO_TRAP_STOPPED);
  proc_release(spinner);
  printf("  %s: passed\n", mode);
}

int main(void) {
  jello_setenv("JELLO_SCHEDULERS", "4");
  jello_bc_module* m = build_module();
  jello_vm* vm = jello_vm_create();
  jello_vm_keep_processes(vm, 1);
  check(vm, m, NULL, 0, "interpreted");
  if(jello_jit_available()) {
    check(vm, m, NULL, 1, "the run's own JIT code");
    jello_jit_module jm;
    memset(&jm, 0, sizeof jm);
    if(jello_jit_module_prepare(m, &jm) == 1) {
      check(vm, m, &jm, 1, "shared JIT code");
      proc_end_all(vm);
      jello_jit_module_free(&jm);
    }
  }
  jello_vm_destroy(vm);
  printf("run_for_tests: passed\n");
  return 0;
}
