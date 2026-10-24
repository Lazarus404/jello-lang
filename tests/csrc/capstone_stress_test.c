// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/jdll.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "watchdog.h"

#define N_ALLOC 4
#define RUN_MS 2500

static atomic_int g_stop;

static void* alloc_loop(void* argp) {
  jello_vm* vm = (jello_vm*)argp;
  jello_vm_running(vm, 1);
  while(!atomic_load(&g_stop)) {
    (void)jello_bytes_new(vm, 256);
  }
  jello_vm_running(vm, 0);
  return NULL;
}

static jello_bc_module* build_echo_module(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  static const char* names[] = {"test_native", "echo_i32"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0];
  atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;
  jello_type_entry* types = (jello_type_entry*)calloc(1, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  m->ntypes = 1;
  m->types = types;
  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0;
  imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_I32;
  imports[0].arity = 1;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_I32;
  jello_jdll_handle* h = jello_jdll_load("test_native");
  if(!h) abort();
  imports[0].resolved = jello_jdll_find_export(h, "echo_i32");
  if(!imports[0].resolved) abort();
  m->nimports = 1;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = h;
  m->jdll_handles = handles;
  m->njdll_handles = 1;
  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0; reg_types[1] = 0;
  jello_insn* insns = (jello_insn*)calloc(3, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 42;
  insns[1].op = (uint8_t)JOP_CALL_NATIVE; insns[1].a = 1; insns[1].b = 0; insns[1].c = 1; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_RET; insns[2].a = 1;
  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 2; funcs[0].cap_start = 2; funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 3; funcs[0].insns = insns;
  m->nfuncs = 1; m->funcs = funcs; m->entry = 0;
  return m;
}

static void free_echo(jello_bc_module* m) {
  free((void*)m->types);
  free((void*)m->atoms);
  free((void*)m->imports);
  jello_jdll_unload(((jello_jdll_handle**)m->jdll_handles)[0]);
  free(m->jdll_handles);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);
}

static void* jdll_cycle(void* argp) {
  jello_vm* vm = (jello_vm*)argp;
  int ok = 1;
  while(!atomic_load(&g_stop)) {
    jello_bc_module* m = build_echo_module();
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    if(jello_vm_exec_numeric(vm, m, &out) != JELLO_EXEC_OK || out.as.i32 != 42) ok = 0;
    free_echo(m);
    if(!ok) break;
  }
  return (void*)(intptr_t)ok;
}

static jello_bc_module* build_fuel_rec(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(1, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  m->ntypes = 1; m->types = types;
  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id));
  reg_types[0] = 0;
  jello_insn* insns = (jello_insn*)calloc(2, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CALL; insns[0].a = 0; insns[0].c = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;
  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 1; funcs[0].nargs = 0; funcs[0].cap_start = 1;
  funcs[0].reg_types = reg_types; funcs[0].ninsns = 2; funcs[0].insns = insns;
  m->nfuncs = 1; m->funcs = funcs; m->entry = 0;
  return m;
}

static void* fuel_loop(void* argp) {
  jello_vm* vm = (jello_vm*)argp;
  jello_bc_module* m = build_fuel_rec();
  int traps = 0;
  while(!atomic_load(&g_stop)) {
    jello_vm_set_fuel(vm, 64);
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric(vm, m, &out);
    if(st == JELLO_EXEC_TRAP && jello_vm_last_trap_code(vm) == JELLO_TRAP_FUEL) traps++;
  }
  free((void*)m->types);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);
  return (void*)(intptr_t)(traps > 0);
}

#if defined(__x86_64__) || defined(__aarch64__)

static const char* g_atoms[2] = {"main", "helper"};

static void build_named(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m, int32_t c) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  r0[0] = 0; r1[0] = 0;
  memset(e0, 0, 2 * sizeof(jello_insn));
  memset(e1, 0, 2 * sizeof(jello_insn));
  e0[0].op = (uint8_t)JOP_CALL; e0[0].a = 0; e0[0].imm = 1;
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 0;
  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = (uint32_t)c;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 0;
  memset(funcs, 0, 2 * sizeof(jello_bc_function));
  funcs[0].name_atom = 0; funcs[0].nregs = 1; funcs[0].cap_start = 1; funcs[0].reg_types = r0;
  funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].name_atom = 1; funcs[1].nregs = 1; funcs[1].cap_start = 1; funcs[1].reg_types = r1;
  funcs[1].ninsns = 2; funcs[1].insns = e1;
  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 2; m->funcs = funcs; m->entry = 0;
  m->atoms = g_atoms; m->natoms = 2;
}

typedef struct reload_pack {
  jello_vm* vm;
  jello_jit_module* jm;
  jello_bc_module* ma;
  jello_bc_module* mb;
} reload_pack;

static void* reload_loop(void* argp) {
  reload_pack* p = (reload_pack*)argp;
  const jello_bc_module* cur = p->ma;
  int ok = 1;
  while(!atomic_load(&g_stop)) {
    jello_bc_module* nxt = (cur == p->ma) ? p->mb : p->ma;
    jello_jit_reload_report report;
    int rc = jello_jit_module_reload(p->vm, p->jm, cur, nxt, 0, &report);
    jello_jit_reload_report_free(&report);
    if(rc != 1) continue;
    if(atomic_load(&g_stop)) break;
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    if(jello_vm_exec_numeric_with_jit(p->vm, p->jm, nxt, &out) != JELLO_EXEC_OK) {
      ok = 0;
      break;
    }
    cur = nxt;
  }
  return (void*)(intptr_t)ok;
}

#endif

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  start_watchdog("capstone_stress_test", 60);
  atomic_init(&g_stop, 0);

  jello_vm* owner = jello_vm_create();
  assert(owner);
  jello_vm* alloc_vms[N_ALLOC];
  pthread_t alloc_th[N_ALLOC];
  for(int i = 0; i < N_ALLOC; i++) {
    alloc_vms[i] = jello_vm_create_joined(owner);
    assert(alloc_vms[i]);
    assert(pthread_create(&alloc_th[i], NULL, alloc_loop, alloc_vms[i]) == 0);
  }

  jello_vm* jdll_vm = jello_vm_create_joined(owner);
  pthread_t jdll_th;
  assert(pthread_create(&jdll_th, NULL, jdll_cycle, jdll_vm) == 0);

  jello_vm* fuel_vm = jello_vm_create_joined(owner);
  pthread_t fuel_th;
  assert(pthread_create(&fuel_th, NULL, fuel_loop, fuel_vm) == 0);

#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry ta[1], tb[1];
  jello_type_id r0a[1], r1a[1], r0b[1], r1b[1];
  jello_insn e0a[2], e1a[2], e0b[2], e1b[2];
  jello_bc_function fa[2] = {0}, fb[2] = {0};
  jello_bc_module ma = {0}, mb = {0};
  build_named(ta, r0a, r1a, e0a, e1a, fa, &ma, 100);
  build_named(tb, r0b, r1b, e0b, e1b, fb, &mb, 200);
  jello_jit_module* jm = jello_jit_module_create(&ma);
  assert(jm);
  jello_vm* reload_vm = jello_vm_create_joined(owner);
  reload_pack rp = {reload_vm, jm, &ma, &mb};
  pthread_t reload_th;
  assert(pthread_create(&reload_th, NULL, reload_loop, &rp) == 0);
#endif

  usleep(RUN_MS * 1000);
  atomic_store(&g_stop, 1);

  for(int i = 0; i < N_ALLOC; i++) assert(pthread_join(alloc_th[i], NULL) == 0);
  void* jdll_ok = NULL;
  void* fuel_ok = NULL;
  assert(pthread_join(jdll_th, &jdll_ok) == 0);
  assert(pthread_join(fuel_th, &fuel_ok) == 0);
  assert((intptr_t)jdll_ok == 1);
  assert((intptr_t)fuel_ok == 1);
#if defined(__x86_64__) || defined(__aarch64__)
  void* reload_ok = NULL;
  assert(pthread_join(reload_th, &reload_ok) == 0);
  assert((intptr_t)reload_ok == 1);
  jello_jit_module_destroy(jm);
  jello_vm_destroy(reload_vm);
#endif

  jello_vm_destroy(fuel_vm);
  jello_vm_destroy(jdll_vm);
  for(int i = N_ALLOC - 1; i >= 0; i--) jello_vm_destroy(alloc_vms[i]);
  jello_vm_destroy(owner);
  printf("capstone_stress_test: PASS\n");
  return 0;
}
