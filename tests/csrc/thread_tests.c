// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* -- Shared-heap threading + stop-the-world correctness --------------------
 * The part of threading that cannot be expressed as a .jlo.txt + shell-script
 * fixture: real OS threads, jello_vm_create_joined, and the collector
 * actually pausing more than one registered VM. Everything JDLL-related that
 * *can* go through jlo_tool's own asm/run harness (round-trips,
 * signature-mismatch rejection, std JDLL file I/O) does, in
 * tests/ci/jdll_fixtures.sh instead.
 *
 * Reaches into jello_vm internals directly (jello/internal.h), the same
 * way jlo_tool.c already does -- this is VM-adjacent test code, not an
 * embedder.
 */

#include <jello/internal.h>
#include <jello/jdll.h>

/* This file's own assert()s gate real side effects (pthread_create/
 * pthread_join's return codes) as well as pass/fail -- a test file that
 * relied on assert() alone would silently stop creating and joining
 * threads at all under a -DNDEBUG (Release) build, passing trivially by
 * doing nothing rather than actually exercising anything. #undef before
 * including <assert.h> makes assert() unconditional in this translation
 * unit regardless of the build's own global NDEBUG setting -- test code
 * must never depend on NDEBUG being unset, since a test suite that only
 * tests something in one build configuration is not a test suite. */
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <jello/internal/platform.h>
#include "watchdog.h"

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* -- fixture 1: two joined VMs, one allocating, one blocked in a native call.
 * Thread A drives real allocation pressure entirely at the C level (no
 * bytecode needed to prove the collector scans/reclaims correctly);
 * Thread B runs a tiny bytecode program whose only instruction of interest
 * is a CALL_NATIVE to a test .jdll's own sleep_ms export -- which
 * op_call_native (call_native.c) wraps in quiescent state automatically,
 * without this fixture ever calling jello_vm_blocking itself, proving the
 * automatic wrapping alone is sufficient.
 */

#define ALLOC_ROUNDS 24000
#define SLEEP_MS 60

typedef struct alloc_thread_arg {
  jello_vm* vm;
  uint32_t collections_seen;
} alloc_thread_arg;

static void* alloc_thread_main(void* argp) {
  alloc_thread_arg* arg = (alloc_thread_arg*)argp;
  jello_vm_running(arg->vm, 1);
  for(int i = 0; i < ALLOC_ROUNDS; i++) {
    /* Each Bytes object is garbage the instant this loop iterates again --
     * nothing roots it (this thread never pushes a call_frame at all), so
     * every one of these allocations is reclaimable by the very next
     * collection. Big enough (300 bytes) to cross several size classes and
     * force real threshold-driven collections within this loop,
     * not just a handful. */
    jello_bytes* b = jello_bytes_new(arg->vm, 300);
    (void)b;
  }
  jello_vm_running(arg->vm, 0);
  uint64_t live = 0;
  uint32_t collections = 0;
  double pause_ms = 0.0;
  jello_vm_gc_stats(arg->vm, &live, &collections, &pause_ms);
  arg->collections_seen = collections;
  return NULL;
}

/* A minimal hand-assembled module: one function, no args, that loads `ms`
 * as a literal, CALL_NATIVEs "sleep_ms" (imported from test_native) with
 * it, then returns the Bool result. Built directly as a jello_bc_module
 * here (bypassing jlo_tool's text format entirely) -- this file only needs
 * one fixed program, not a general-purpose assembler. */
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
  if(!h) { fprintf(stderr, "build_sleep_module: failed to load test_native.jdll (check JELLO_JDLL_PATH)\n"); abort(); }
  const jello_jdll_export* exp = jello_jdll_find_export(h, "sleep_ms");
  if(!exp) { fprintf(stderr, "build_sleep_module: test_native has no sleep_ms export\n"); abort(); }
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = h;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0; /* r0: I32 */
  reg_types[1] = 1; /* r1: Bool */

  /* 0: CONST_I32 r0 = ms
   * 1: CALL_NATIVE r1 = sleep_ms(r0)
   * 2: RET r1
   */
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

typedef struct blocking_thread_arg {
  jello_vm* vm;
  const jello_bc_module* m;
  int ok;
} blocking_thread_arg;

static void* blocking_thread_main(void* argp) {
  blocking_thread_arg* arg = (blocking_thread_arg*)argp;
  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
  arg->ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_BOOL && out.as.b == 1);
  return NULL;
}

static void test_shared_heap_alloc_and_blocking_call(void) {
  printf("== fixture: shared heap, one allocating thread + one blocking-native-call thread ==\n");

  jello_vm* vm_a = jello_vm_create();
  assert(vm_a);
  jello_vm* vm_b = jello_vm_create_joined(vm_a);
  assert(vm_b);

  jello_bc_module* m = build_sleep_module(SLEEP_MS);

  alloc_thread_arg a_arg = {vm_a, 0};
  blocking_thread_arg b_arg = {vm_b, m, 0};

  double t0 = now_ms();
  pthread_t ta, tb;
  assert(pthread_create(&ta, NULL, alloc_thread_main, &a_arg) == 0);
  assert(pthread_create(&tb, NULL, blocking_thread_main, &b_arg) == 0);
  assert(pthread_join(ta, NULL) == 0);
  assert(pthread_join(tb, NULL) == 0);
  double elapsed = now_ms() - t0;

  printf("  collections_seen=%u blocking_call_ok=%d elapsed_ms=%.1f\n",
         a_arg.collections_seen, b_arg.ok, elapsed);

  assert(b_arg.ok && "blocking native call must still return the correct value");
  assert(a_arg.collections_seen > 0 && "allocation loop must have triggered at least one real collection");
  /* Not `< SLEEP_MS + alloc time summed` -- the two threads' work must
   * overlap, not serialise behind one another. A generous multiple of
   * SLEEP_MS alone (not sleep + a full allocation-loop's worth of serial
   * collections) is what proves that. */
  assert(elapsed < (double)(SLEEP_MS * 8) && "threads must not have serialised behind one another");

  free((void*)m->types);
  free((void*)m->atoms);
  free((void*)m->imports);
  jello_jdll_unload(((jello_jdll_handle**)m->jdll_handles)[0]);
  free(m->jdll_handles);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);

  jello_vm_destroy(vm_b);
  jello_vm_destroy(vm_a);
  printf("  PASS\n");
}

/* -- fixture 2: the backward-JMP safepoint check actually fires -----------
 * A non-allocating, tight counting loop (JMP-based, no Bytes/Struct/Object
 * touched) on one thread, while another thread forces repeated
 * collections on the same shared heap. Without the safepoint check, the
 * collector's spin would never see this thread reach quiescent_depth > 0
 * and would hang, so a regression here trips the test's watchdog,
 * which aborts it.
 */

#define LOOP_ITERATIONS 1000000

static jello_bc_module* build_counting_loop_module(int32_t iterations) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(2, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  m->ntypes = 2;
  m->types = types;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0; /* r0: I32 counter */
  reg_types[1] = 1; /* r1: Bool loop condition */

  /* 0: CONST_I32 r0 = 0
   * 1: LT_I32_IMM r1 = r0 < iterations
   * 2: ADD_I32_IMM r0 = r0 + 1
   * 3: JMP_IF r1, -3   (back to 1)
   * 4: RET r0
   */
  jello_insn* insns = (jello_insn*)calloc(5, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_LT_I32_IMM; insns[1].a = 1; insns[1].b = 0; insns[1].imm = (uint32_t)iterations;
  insns[2].op = (uint8_t)JOP_ADD_I32_IMM; insns[2].a = 0; insns[2].b = 0; insns[2].imm = 1;
  insns[3].op = (uint8_t)JOP_JMP_IF; insns[3].a = 1; insns[3].imm = (uint32_t)(int32_t)-3;
  insns[4].op = (uint8_t)JOP_RET; insns[4].a = 0;

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 2;
  funcs[0].cap_start = 2;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 5;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static void free_counting_loop_module(jello_bc_module* m) {
  free((void*)m->types);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);
}

typedef struct loop_thread_arg {
  jello_vm* vm;
  const jello_bc_module* m;
  int ok;
} loop_thread_arg;

static void* loop_thread_main(void* argp) {
  loop_thread_arg* arg = (loop_thread_arg*)argp;
  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
  /* The module's own JMP_IF checks "r0 < iterations" *before* incrementing
   * r0 on that same pass, so the increment that makes the condition go
   * false still runs once before RET -- final r0 is iterations + 1, not
   * iterations. A test bytecode detail, not a VM correctness property. */
  arg->ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 == LOOP_ITERATIONS + 1);
  return NULL;
}

typedef struct collector_thread_arg {
  jello_vm* vm;
  atomic_int* stop; /* volatile is not a synchronisation primitive in C -- this
                      * genuinely needs to be atomic for a cross-thread flag,
                      * confirmed by ThreadSanitizer flagging the earlier
                      * volatile-int version as a real data race */
} collector_thread_arg;

static void* collector_thread_main(void* argp) {
  collector_thread_arg* arg = (collector_thread_arg*)argp;
  struct timespec pause = {0, 200 * 1000}; /* 200us: exercise the STW path
    * often without turning this into a pure mutex-contention benchmark --
    * this fixture is about the safepoint firing correctly, not throughput. */
  while(!atomic_load(arg->stop)) {
    jello_vm_running(arg->vm, 1);
    for(int i = 0; i < 200; i++) {
      jello_bytes* b = jello_bytes_new(arg->vm, 64);
      (void)b;
    }
    jello_gc_collect(arg->vm);
    jello_vm_running(arg->vm, 0);
    nanosleep(&pause, NULL);
  }
  return NULL;
}

static void test_safepoint_at_loop_backedge(void) {
  printf("== fixture: backward-JMP safepoint check under concurrent collection ==\n");

  jello_vm* vm_loop = jello_vm_create();
  assert(vm_loop);
  jello_vm* vm_collector = jello_vm_create_joined(vm_loop);
  assert(vm_collector);

  jello_bc_module* m = build_counting_loop_module(LOOP_ITERATIONS);

  atomic_int stop;
  atomic_init(&stop, 0);
  collector_thread_arg c_arg = {vm_collector, &stop};
  loop_thread_arg l_arg = {vm_loop, m, 0};

  pthread_t tc, tl;
  assert(pthread_create(&tc, NULL, collector_thread_main, &c_arg) == 0);
  assert(pthread_create(&tl, NULL, loop_thread_main, &l_arg) == 0);
  assert(pthread_join(tl, NULL) == 0);
  atomic_store(&stop, 1);
  assert(pthread_join(tc, NULL) == 0);

  printf("  loop_ok=%d\n", l_arg.ok);
  assert(l_arg.ok && "counting loop must finish with the correct result while collections run concurrently");

  free_counting_loop_module(m);

  jello_vm_destroy(vm_collector);
  jello_vm_destroy(vm_loop);
  printf("  PASS\n");
}

/* -- fixture 2b: compiled code parks at a pending stop-the-world ----------
 * The counting loop runs compiled for about 2^31 iterations, seconds of work. Once it is
 * running, another thread stops the world, which returns only when the loop's thread has
 * parked at a JIR_SAFEPOINT, and while it is stopped sets that VM's fuel to exhausted. The loop
 * must then end with a fuel trap: had the stop waited for the loop to finish, it would return
 * its count instead.
 */

typedef struct jit_loop_arg {
  jello_vm* vm;
  jello_jit_module* jm;
  const jello_bc_module* m;
  jello_exec_status st;
} jit_loop_arg;

static void* jit_loop_thread_main(void* argp) {
  jit_loop_arg* arg = (jit_loop_arg*)argp;
  jello_numeric_result out;
  arg->st = jello_vm_exec_numeric_with_jit(arg->vm, arg->jm, arg->m, &out);
  return NULL;
}

static void test_compiled_loop_parks_for_stop_the_world(void) {
  printf("== fixture: compiled loop parks at a pending stop-the-world ==\n");
  jello_vm* vm_loop = jello_vm_create();
  assert(vm_loop);
  jello_vm* vm_stopper = jello_vm_create_joined(vm_loop);
  assert(vm_stopper);

  jello_bc_module* m = build_counting_loop_module(INT32_MAX - 1);
  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(m, &jm) == 1);
  assert(jm.ncompiled == jm.nfuncs);

  jit_loop_arg arg = {vm_loop, &jm, m, JELLO_EXEC_OK};
  pthread_t tl;
  assert(pthread_create(&tl, NULL, jit_loop_thread_main, &arg) == 0);
  while(!atomic_load(&vm_loop->in_exec)) sched_yield();
  struct timespec settle = {0, 20 * 1000 * 1000};
  nanosleep(&settle, NULL);

  assert(vm_heap_stop_the_world(vm_stopper) == 1);
  vm_loop->fuel_limit = 1;
  vm_loop->fuel_remaining = 0;
  vm_heap_resume_the_world(vm_stopper);
  assert(pthread_join(tl, NULL) == 0);

  printf("  st=%d trap=%d\n", (int)arg.st, (int)jello_vm_last_trap_code(vm_loop));
  assert(arg.st == JELLO_EXEC_TRAP && jello_vm_last_trap_code(vm_loop) == JELLO_TRAP_FUEL &&
         "the stop must reach the compiled loop before it finishes");

  jello_jit_module_free(&jm);
  free_counting_loop_module(m);
  jello_vm_destroy(vm_stopper);
  jello_vm_destroy(vm_loop);
  printf("  PASS\n");
}

/* -- fixture 3: the std JDLL's file I/O, get_env, argv --------------------
 * jello-refined/jello/jdll-std is the real, shipped plugin; this is the
 * one place its exports are actually exercised end to end (round-trip and
 * signature-mismatch-rejection fixtures for the general JDLL mechanism
 * live in tests/ci/jdll_fixtures.sh via jlo_tool instead -- this fixture
 * is specific to std's own file/env/argv behaviour, which needs real
 * Bytes literals long enough that hand-writing them in jlo_tool's own
 * text grammar, byte by byte, would obscure more than it proves).
 */

typedef struct insn_buf {
  jello_insn* items;
  uint32_t len;
  uint32_t cap;
} insn_buf;

static void ib_push(insn_buf* b, jello_op op, uint8_t a, uint8_t bb, uint8_t cc, int32_t imm) {
  if(b->len == b->cap) {
    b->cap = b->cap ? b->cap * 2u : 16u;
    b->items = (jello_insn*)realloc(b->items, sizeof(jello_insn) * (size_t)b->cap);
  }
  jello_insn* in = &b->items[b->len++];
  in->op = (uint8_t)op;
  in->a = a;
  in->b = bb;
  in->c = cc;
  in->imm = (uint32_t)imm;
}

/* Emits BYTES_NEW dst,(len) then one CONST_I32+BYTES_SET_U8 pair per byte,
 * using idx_reg/byte_reg as scratch. */
static void emit_bytes_literal(insn_buf* b, uint8_t dst, uint8_t idx_reg, uint8_t byte_reg,
                                uint8_t len_reg, const char* s) {
  uint32_t len = (uint32_t)strlen(s);
  ib_push(b, JOP_CONST_I32, len_reg, 0, 0, (int32_t)len);
  ib_push(b, JOP_BYTES_NEW, dst, len_reg, 0, 0);
  for(uint32_t i = 0; i < len; i++) {
    ib_push(b, JOP_CONST_I32, idx_reg, 0, 0, (int32_t)i);
    ib_push(b, JOP_CONST_I32, byte_reg, 0, 0, (int32_t)(unsigned char)s[i]);
    /* BYTES_SET_U8: a=value(I32), b=dst(Bytes), c=index(I32) -- not the
     * dst/idx/value order the parallel BYTES_NEW/GET_U8 calls above might
     * suggest at a glance (ops_bytes.c's own op_bytes_set_u8). */
    ib_push(b, JOP_BYTES_SET_U8, byte_reg, dst, idx_reg, 0);
  }
}

/* Every build_std_*_module function below takes an *already-loaded*
 * std.jdll handle rather than calling jello_jdll_load itself: dlopen()ing
 * and dlclose()ing the same shared library repeatedly within one process
 * is exactly the pattern that trips ASan's ODR-violation detector on
 * Darwin (dyld re-runs an image's static initializers, including ASan's
 * own global-registration hooks, on some but not all reload cycles) --
 * loading each distinct library exactly once per process, like a real
 * embedder would, avoids the whole class of issue rather than working
 * around one specific symptom of it. main() below owns the one load/unload
 * pair for "std" that every std-related fixture shares.
 *
 * r0=path(Bytes) r1=data(Bytes) r2=I32 scratch r3=I32 scratch r4=I32 scratch(len)
 * r5=Bool(write_ok) r6=Bytes(read_back) r7=I32(read byte 0) r8=Bool(result) */
static jello_bc_module* build_std_file_roundtrip_module(jello_jdll_handle* h, const char* path, const char* data) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"std", "file_write", "file_read"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 3u);
  atoms[0] = names[0]; atoms[1] = names[1]; atoms[2] = names[2];
  m->natoms = 3;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(3, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BYTES;
  types[2].kind = JELLO_T_BOOL;
  m->ntypes = 3;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(2, sizeof(jello_import_sig));
  imports[0].library_atom = 0; imports[0].export_atom = 1; /* file_write */
  imports[0].ret_kind = (uint8_t)JELLO_T_BOOL; imports[0].arity = 2;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_BYTES; imports[0].arg_kinds[1] = (uint8_t)JELLO_T_BYTES;
  imports[1].library_atom = 0; imports[1].export_atom = 2; /* file_read */
  imports[1].ret_kind = (uint8_t)JELLO_T_BYTES; imports[1].arity = 1;
  imports[1].arg_kinds[0] = (uint8_t)JELLO_T_BYTES;

  const jello_jdll_export* exp_write = jello_jdll_find_export(h, "file_write");
  const jello_jdll_export* exp_read = jello_jdll_find_export(h, "file_read");
  if(!exp_write || !exp_read) { fprintf(stderr, "std.jdll missing file_write/file_read\n"); abort(); }
  imports[0].resolved = exp_write;
  imports[1].resolved = exp_read;
  m->nimports = 2;
  m->imports = imports;
  m->jdll_handles = NULL; /* not owned by this module -- main() unloads "std" once */
  m->njdll_handles = 0;

  /* r0=path r1=data r2..r4=I32 scratch r5=Bool write_ok r6=Bytes read_back
   * r7=I32 first-byte-of-readback r8=Bool result */
  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 9u);
  reg_types[0] = 1; reg_types[1] = 1; reg_types[2] = 0; reg_types[3] = 0; reg_types[4] = 0;
  reg_types[5] = 2; reg_types[6] = 1; reg_types[7] = 0; reg_types[8] = 2;

  insn_buf ib = {0};
  emit_bytes_literal(&ib, 0, 2, 3, 4, path);
  emit_bytes_literal(&ib, 1, 2, 3, 4, data);
  ib_push(&ib, JOP_CALL_NATIVE, 5, 0, 2, 0); /* r5 = file_write(r0, r1) */
  ib_push(&ib, JOP_CALL_NATIVE, 6, 0, 1, 1); /* r6 = file_read(r0) */
  ib_push(&ib, JOP_CONST_I32, 2, 0, 0, 0);
  ib_push(&ib, JOP_BYTES_GET_U8, 7, 6, 2, 0); /* r7 = read_back[0] */
  ib_push(&ib, JOP_EQ_I32_IMM, 8, 7, 0, (int32_t)(unsigned char)data[0]); /* r8 = (r7 == data[0]) */
  ib_push(&ib, JOP_RET, 8, 0, 0, 0);

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 9;
  funcs[0].cap_start = 9;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = ib.len;
  funcs[0].insns = ib.items;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static void free_std_module(jello_bc_module* m) {
  free((void*)m->types);
  free((void*)m->atoms);
  free((void*)m->imports);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);
}

static void test_std_jdll_file_roundtrip(jello_jdll_handle* std_h) {
  printf("== fixture: std JDLL file_write/file_read round-trip ==\n");
  /* A temporary directory from the environment, since Windows has no /tmp. */
  const char* dir = getenv("TMPDIR");
  if(!dir || !*dir) dir = getenv("TEMP");
  if(!dir || !*dir) dir = getenv("TMP");
  if(!dir || !*dir) dir = "/tmp";
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/jello_thread_tests_std.tmp", dir);
  const char* data = "hello, thread tests";

  jello_vm* vm = jello_vm_create();
  jello_bc_module* m = build_std_file_roundtrip_module(std_h, path, data);

  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(vm, m, &out);
  int ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_BOOL && out.as.b == 1);
  printf("  round_trip_ok=%d\n", ok);
  assert(ok && "file_write then file_read must round-trip the same first byte");

  remove(path);
  free_std_module(m);
  jello_vm_destroy(vm);
  printf("  PASS\n");
}

/* r0=name(Bytes) r1..r3=I32 scratch r4=Bytes(env value) r5=I32(first byte)
 * r6=Bool(result) */
static jello_bc_module* build_std_get_env_module(jello_jdll_handle* h, const char* name, char expected_first_char) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"std", "get_env"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0]; atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(3, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BYTES;
  types[2].kind = JELLO_T_BOOL;
  m->ntypes = 3;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0; imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_BYTES; imports[0].arity = 1;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_BYTES;

  const jello_jdll_export* exp = jello_jdll_find_export(h, "get_env");
  if(!exp) { fprintf(stderr, "std.jdll missing get_env\n"); abort(); }
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  m->jdll_handles = NULL;
  m->njdll_handles = 0;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 7u);
  reg_types[0] = 1; reg_types[1] = 0; reg_types[2] = 0; reg_types[3] = 0;
  reg_types[4] = 1; reg_types[5] = 0; reg_types[6] = 2;

  insn_buf ib = {0};
  emit_bytes_literal(&ib, 0, 1, 2, 3, name);
  ib_push(&ib, JOP_CALL_NATIVE, 4, 0, 1, 0); /* r4 = get_env(r0) */
  ib_push(&ib, JOP_CONST_I32, 1, 0, 0, 0);
  ib_push(&ib, JOP_BYTES_GET_U8, 5, 4, 1, 0); /* r5 = r4[0] */
  ib_push(&ib, JOP_EQ_I32_IMM, 6, 5, 0, (int32_t)(unsigned char)expected_first_char);
  ib_push(&ib, JOP_RET, 6, 0, 0, 0);

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 7;
  funcs[0].cap_start = 7;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = ib.len;
  funcs[0].insns = ib.items;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static jello_bc_module* build_std_argv_count_module(jello_jdll_handle* h) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"std", "argv_count"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0]; atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(1, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  m->ntypes = 1;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0; imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_I32; imports[0].arity = 0;

  const jello_jdll_export* exp = jello_jdll_find_export(h, "argv_count");
  if(!exp) { fprintf(stderr, "std.jdll missing argv_count\n"); abort(); }
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  m->jdll_handles = NULL;
  m->njdll_handles = 0;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 1u);
  reg_types[0] = 0;

  jello_insn* insns = (jello_insn*)calloc(2, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CALL_NATIVE; insns[0].a = 0; insns[0].b = 0; insns[0].c = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 1;
  funcs[0].cap_start = 1;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 2;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

/* The VM's register-frame layout cache once used an invalidation rule
 * that compared a jello_bc_module pointer to decide "is this still the
 * same module" -- unsound once a module is freed and a *different* module
 * happens to be allocated at the exact same, now-reused address (ordinary,
 * correct C behaviour for any allocator, not a bug in malloc), which read
 * as "same module, keep the cache" and served one module's own register
 * kinds for a completely different module's registers. This one iteration
 * already reproduced that bug intermittently (its own failure depends on
 * the allocator's history, not this test's own logic) -- the loop below
 * repeats the exact free-then-immediately-allocate-same-shape pattern that
 * triggers it enough times that a real regression here fails reliably,
 * not roughly one time in thirty. */
static void test_std_jdll_get_env_and_argv(jello_jdll_handle* std_h) {
  printf("== fixture: std JDLL get_env + argv ==\n");
  const char* env_value = "checked";
  jello_setenv("JELLO_THREAD_TEST_VAR", env_value);

  jello_vm* vm = jello_vm_create();
  for(int iter = 0; iter < 20; iter++) {
    jello_bc_module* m_env = build_std_get_env_module(std_h, "JELLO_THREAD_TEST_VAR", env_value[0]);
    jello_numeric_result out;
    jello_exec_status st = jello_vm_exec_numeric(vm, m_env, &out);
    int env_ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_BOOL && out.as.b == 1);
    if(iter == 0) printf("  get_env_ok=%d\n", env_ok);
    assert(env_ok && "get_env must return the value this test just set via setenv");
    free_std_module(m_env);

    jello_bc_module* m_argv = build_std_argv_count_module(std_h);
    st = jello_vm_exec_numeric(vm, m_argv, &out);
    int argv_ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 >= 1);
    if(iter == 0) printf("  argv_count=%d argv_ok=%d\n", (st == JELLO_EXEC_OK) ? out.as.i32 : -1, argv_ok);
    assert(argv_ok && "argv_count must be at least 1 (argv[0], the process's own path) -- "
                       "a failure here on iteration > 0 is the frame_layout aliasing bug "
                       "this loop exists to catch reliably, not a fluke");
    free_std_module(m_argv);
  }
  jello_vm_destroy(vm);
  printf("  PASS\n");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  start_watchdog("thread_tests", 45);
  test_shared_heap_alloc_and_blocking_call();
  test_safepoint_at_loop_backedge();
  test_compiled_loop_parks_for_stop_the_world();

  /* "std" is loaded exactly once for the whole process, shared by both
   * fixtures below -- see build_std_file_roundtrip_module's own comment
   * for why repeated dlopen/dlclose of the same library is worth avoiding
   * outright rather than working around. */
  jello_jdll_handle* std_h = jello_jdll_load("std");
  if(std_h) {
    test_std_jdll_file_roundtrip(std_h);
    test_std_jdll_get_env_and_argv(std_h);
    jello_jdll_unload(std_h);
  } else {
    fprintf(stderr, "std JDLL fixtures: std.jdll not found on JELLO_JDLL_PATH\n");
    return 1;
  }

  printf("thread_tests: all fixtures passed\n");
  return 0;
}
