// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/jdll.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "watchdog.h"

#define N_THREADS 8
#define ALLOC_ROUNDS 10000
#define JDLL_CYCLES 50
#define NATIVE_CALLS 4000

typedef struct alloc_arg {
  jello_vm* vm;
  uint32_t collections_seen;
} alloc_arg;

static void* alloc_thread_main(void* argp) {
  alloc_arg* arg = (alloc_arg*)argp;
  jello_vm_running(arg->vm, 1);
  for(int i = 0; i < ALLOC_ROUNDS; i++) {
    (void)jello_bytes_new(arg->vm, 400);
  }
  jello_vm_running(arg->vm, 0);
  uint64_t live = 0;
  uint32_t collections = 0;
  double pause_ms = 0.0;
  jello_vm_gc_stats(arg->vm, &live, &collections, &pause_ms);
  arg->collections_seen = collections;
  return NULL;
}

static jello_bc_module* build_sleep_module(int ms) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  static const char* names[] = {"test_native", "sleep_ms"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0];
  atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(2, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  m->ntypes = 2;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0;
  imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_BOOL;
  imports[0].arity = 1;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_I32;
  jello_jdll_handle* h = jello_jdll_load("test_native");
  if(!h) { fprintf(stderr, "failed to load test_native.jdll\n"); abort(); }
  const jello_jdll_export* exp = jello_jdll_find_export(h, "sleep_ms");
  if(!exp) abort();
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = h;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0;
  reg_types[1] = 1;
  jello_insn* insns = (jello_insn*)calloc(3, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = (uint32_t)ms;
  insns[1].op = (uint8_t)JOP_CALL_NATIVE; insns[1].a = 1; insns[1].b = 0; insns[1].c = 1; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_RET; insns[2].a = 1;
  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 2;
  funcs[0].cap_start = 2;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 3;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
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
  if(!h) { fprintf(stderr, "failed to load test_native.jdll\n"); abort(); }
  const jello_jdll_export* exp = jello_jdll_find_export(h, "echo_i32");
  if(!exp) abort();
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = h;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0;
  reg_types[1] = 0;
  jello_insn* insns = (jello_insn*)calloc(3, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 42;
  insns[1].op = (uint8_t)JOP_CALL_NATIVE; insns[1].a = 1; insns[1].b = 0; insns[1].c = 1; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_RET; insns[2].a = 1;
  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 2;
  funcs[0].cap_start = 2;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 3;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static void free_import_module(jello_bc_module* m) {
  if(!m) return;
  free((void*)m->types);
  free((void*)m->atoms);
  free((void*)m->imports);
  if(m->jdll_handles) {
    jello_jdll_unload(((jello_jdll_handle**)m->jdll_handles)[0]);
    free(m->jdll_handles);
  }
  if(m->funcs) {
    free((void*)m->funcs[0].reg_types);
    free((void*)m->funcs[0].insns);
    free((void*)m->funcs);
  }
  free(m);
}

typedef struct exec_arg {
  jello_vm* vm;
  const jello_bc_module* m;
  int ok;
} exec_arg;

static void* exec_thread_main(void* argp) {
  exec_arg* arg = (exec_arg*)argp;
  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
  arg->ok = (st == JELLO_EXEC_OK);
  return NULL;
}

static void test_nway_concurrent_allocation(void) {
  printf("== N-way concurrent allocation ==\n");
  jello_vm* vms[N_THREADS];
  alloc_arg args[N_THREADS];
  pthread_t threads[N_THREADS];
  vms[0] = jello_vm_create();
  assert(vms[0]);
  for(int i = 1; i < N_THREADS; i++) {
    vms[i] = jello_vm_create_joined(vms[0]);
    assert(vms[i]);
  }
  for(int i = 0; i < N_THREADS; i++) {
    args[i].vm = vms[i];
    args[i].collections_seen = 0;
    assert(pthread_create(&threads[i], NULL, alloc_thread_main, &args[i]) == 0);
  }
  uint32_t total_col = 0;
  for(int i = 0; i < N_THREADS; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    total_col += args[i].collections_seen;
  }
  printf("  collections_seen_sum=%u\n", total_col);
  assert(total_col > 0);
  for(int i = N_THREADS - 1; i >= 0; i--) jello_vm_destroy(vms[i]);
  printf("  PASS\n");
}

static void test_nway_mixed_blocking(void) {
  printf("== N-way mixed alloc + blocking native ==\n");
  jello_vm* vms[N_THREADS];
  vms[0] = jello_vm_create();
  assert(vms[0]);
  for(int i = 1; i < N_THREADS; i++) {
    vms[i] = jello_vm_create_joined(vms[0]);
    assert(vms[i]);
  }
  jello_bc_module* sleep_m = build_sleep_module(40);
  pthread_t threads[N_THREADS];
  alloc_arg aargs[N_THREADS / 2];
  exec_arg eargs[N_THREADS / 2];
  int ai = 0, ei = 0;
  for(int i = 0; i < N_THREADS; i++) {
    if((i % 2) == 0) {
      aargs[ai].vm = vms[i];
      aargs[ai].collections_seen = 0;
      assert(pthread_create(&threads[i], NULL, alloc_thread_main, &aargs[ai]) == 0);
      ai++;
    } else {
      eargs[ei].vm = vms[i];
      eargs[ei].m = sleep_m;
      eargs[ei].ok = 0;
      assert(pthread_create(&threads[i], NULL, exec_thread_main, &eargs[ei]) == 0);
      ei++;
    }
  }
  for(int i = 0; i < N_THREADS; i++) assert(pthread_join(threads[i], NULL) == 0);
  for(int i = 0; i < ei; i++) assert(eargs[i].ok);
  uint32_t total_col = 0;
  for(int i = 0; i < ai; i++) total_col += aargs[i].collections_seen;
  assert(total_col > 0);
  free_import_module(sleep_m);
  for(int i = N_THREADS - 1; i >= 0; i--) jello_vm_destroy(vms[i]);
  printf("  PASS\n");
}

static void test_jdll_load_unload_cycling(void) {
  printf("== JDLL load/unload cycling ==\n");
  jello_vm* vm = jello_vm_create();
  assert(vm);
  for(int i = 0; i < JDLL_CYCLES; i++) {
    jello_bc_module* m = build_echo_module();
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric(vm, m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 42);
    free_import_module(m);
  }
  jello_vm_destroy(vm);
  printf("  PASS (%d cycles)\n", JDLL_CYCLES);
}

typedef struct native_arg {
  jello_vm* vm;
  const jello_bc_module* m;
  int ok;
} native_arg;

static void* native_throughput_main(void* argp) {
  native_arg* arg = (native_arg*)argp;
  int ok = 1;
  for(int i = 0; i < NATIVE_CALLS; i++) {
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
    if(st != JELLO_EXEC_OK || out.kind != JELLO_NUM_I32 || out.as.i32 != 42) {
      ok = 0;
      break;
    }
  }
  arg->ok = ok;
  return NULL;
}

static void test_native_call_throughput_under_gc(void) {
  printf("== native-call throughput under GC ==\n");
  jello_vm* vm_a = jello_vm_create();
  jello_vm* vm_b = jello_vm_create_joined(vm_a);
  assert(vm_a && vm_b);
  jello_bc_module* echo = build_echo_module();
  alloc_arg a_arg = {vm_a, 0};
  native_arg n_arg = {vm_b, echo, 0};
  pthread_t ta, tb;
  assert(pthread_create(&ta, NULL, alloc_thread_main, &a_arg) == 0);
  assert(pthread_create(&tb, NULL, native_throughput_main, &n_arg) == 0);
  assert(pthread_join(ta, NULL) == 0);
  assert(pthread_join(tb, NULL) == 0);
  assert(n_arg.ok);
  assert(a_arg.collections_seen > 0);
  free_import_module(echo);
  jello_vm_destroy(vm_b);
  jello_vm_destroy(vm_a);
  printf("  PASS\n");
}

#if defined(__x86_64__) || defined(__aarch64__)

static void build_named_const_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m,
    int32_t helper_const) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  r0[0] = 0; r1[0] = 0;
  memset(e0, 0, 2 * sizeof(jello_insn));
  memset(e1, 0, 2 * sizeof(jello_insn));
  e0[0].op = (uint8_t)JOP_CALL; e0[0].a = 0; e0[0].c = 0; e0[0].imm = 1;
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 0;
  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = (uint32_t)helper_const;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 0;
  funcs[0].name_atom = 0; funcs[0].nargs = 0; funcs[0].nregs = 1; funcs[0].cap_start = 1;
  funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].name_atom = 1; funcs[1].nargs = 0; funcs[1].nregs = 1; funcs[1].cap_start = 1;
  funcs[1].reg_types = r1; funcs[1].ninsns = 2; funcs[1].insns = e1;
  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 2; m->funcs = funcs; m->entry = 0;
}

static const char* g_reload_atoms[2] = {"main", "helper"};

typedef struct reload_arg {
  jello_vm* vm;
  jello_jit_module* jm;
  jello_bc_module* ma;
  jello_bc_module* mb;
  int ok;
} reload_arg;

static void* reload_thread_main(void* argp) {
  reload_arg* arg = (reload_arg*)argp;
  const jello_bc_module* cur = arg->ma;
  int ok = 1;
  for(int i = 0; i < 20; i++) {
    jello_bc_module* nxt = (cur == arg->ma) ? arg->mb : arg->ma;
    jello_jit_reload_report report;
    int rc = jello_jit_module_reload(arg->vm, arg->jm, cur, nxt, 0, &report);
    jello_jit_reload_report_free(&report);
    if(rc != 1) { ok = 0; break; }
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    if(jello_vm_exec_numeric_with_jit(arg->vm, arg->jm, nxt, &out) != JELLO_EXEC_OK) {
      ok = 0;
      break;
    }
    cur = nxt;
  }
  arg->ok = ok;
  return NULL;
}

static void test_reload_under_contention(void) {
  printf("== reload under N-way allocation contention ==\n");
  jello_vm* vms[N_THREADS];
  vms[0] = jello_vm_create();
  assert(vms[0]);
  for(int i = 1; i < N_THREADS; i++) {
    vms[i] = jello_vm_create_joined(vms[0]);
    assert(vms[i]);
  }
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[1], r1_b[1];
  jello_insn e0_a[2], e1_a[2], e0_b[2], e1_b[2];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0};
  jello_bc_module ma = {0}, mb = {0};
  build_named_const_module(types_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &ma, 100);
  ma.atoms = g_reload_atoms; ma.natoms = 2;
  build_named_const_module(types_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &mb, 200);
  mb.atoms = g_reload_atoms; mb.natoms = 2;
  jello_jit_module* jm = jello_jit_module_create(&ma);
  assert(jm != NULL);

  pthread_t threads[N_THREADS];
  alloc_arg aargs[N_THREADS - 1];
  reload_arg rarg = {vms[0], jm, &ma, &mb, 0};
  assert(pthread_create(&threads[0], NULL, reload_thread_main, &rarg) == 0);
  for(int i = 1; i < N_THREADS; i++) {
    aargs[i - 1].vm = vms[i];
    aargs[i - 1].collections_seen = 0;
    assert(pthread_create(&threads[i], NULL, alloc_thread_main, &aargs[i - 1]) == 0);
  }
  for(int i = 0; i < N_THREADS; i++) assert(pthread_join(threads[i], NULL) == 0);
  assert(rarg.ok);
  jello_jit_module_destroy(jm);
  for(int i = N_THREADS - 1; i >= 0; i--) jello_vm_destroy(vms[i]);
  printf("  PASS\n");
}

#else

static void test_reload_under_contention(void) {
  printf("== reload under contention SKIPPED (host is not x86_64 or aarch64) ==\n");
}

#endif

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  start_watchdog("thread_stress_tests", 90);
  test_nway_concurrent_allocation();
  test_nway_mixed_blocking();
  test_jdll_load_unload_cycling();
  test_native_call_throughput_under_gc();
  test_reload_under_contention();
  printf("thread_stress_tests: all fixtures passed\n");
  return 0;
}
