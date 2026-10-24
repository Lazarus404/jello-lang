// SPDX-License-Identifier: BSD-3-Clause
// Processes, and the main process, waiting on sockets watched by the poller, interpreted and as JIT
// code: a write wakes the reader, readiness found meanwhile is kept for the next wait, and only
// new readiness ends a later one; a deadline ends a wait with nothing ready, a stop or a close
// ends it, a second waiter in one direction traps, opening again wakes a waiter closed, and the
// main process waiting alone is no deadlock. Then many readers on four schedulers.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jello/internal.h>
#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/platform.h>
#include <jello/internal/sockets.h>

enum { T_I32, NTYPES };
enum { NPAIRS = 16, WAIT_MS = 50, ROUNDS = 200 };

/* Connected loopback sockets, each pair's [0] reading what its [1] sends: sockets, as on Windows
 * the poller watches nothing else. */
static int pairs[NPAIRS][2];

static jello_insn ins(jello_op op, uint8_t a, uint8_t b, uint8_t c, int32_t imm) {
  jello_insn i = {0};
  i.op = (uint8_t)op;
  i.a = a;
  i.b = b;
  i.c = c;
  i.imm = (uint32_t)imm;
  return i;
}

/* A function waiting to read pair `i` for `ms` milliseconds, returning what it found. */
static jello_bc_function waiter(int i, int32_t ms) {
  static const jello_type_id types[4] = {T_I32, T_I32, T_I32, T_I32};
  jello_insn* code = (jello_insn*)calloc(5, sizeof(jello_insn));
  code[0] = ins(JOP_CONST_I32, 0, 0, 0, pairs[i][0]);
  code[1] = ins(JOP_CONST_I32, 1, 0, 0, PROC_FD_READ);
  code[2] = ins(JOP_CONST_I32, 2, 0, 0, ms);
  code[3] = ins(JOP_CALL_NATIVE, 3, 0, 3, 0);
  code[4] = ins(JOP_RET, 3, 0, 0, 0);
  return (jello_bc_function){JELLO_BC_NO_NAME, 0, 4, 4, types, 5, code, 0, NULL};
}

/* func 0, the entry: returns 0. func 1 + i waits for pair i without a deadline; func 1 + NPAIRS
 * waits for pair 0 for WAIT_MS. */
static jello_bc_module* build_module(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(NTYPES, sizeof(jello_type_entry));
  types[T_I32].kind = JELLO_T_I32;
  m->ntypes = NTYPES;
  m->types = types;

  jello_jdll_handle* vm_lib = jello_jdll_load("vm");
  assert(vm_lib);
  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].ret_kind = (uint8_t)JELLO_T_I32;
  imports[0].arity = 3;
  for(int i = 0; i < 3; i++) imports[0].arg_kinds[i] = (uint8_t)JELLO_T_I32;
  imports[0].resolved = jello_jdll_find_export(vm_lib, "proc_poll_wait");
  assert(imports[0].resolved);
  m->nimports = 1;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = vm_lib;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_bc_function* funcs = (jello_bc_function*)calloc(NPAIRS + 2, sizeof(jello_bc_function));
  static const jello_type_id entry_types[1] = {T_I32};
  jello_insn* entry = (jello_insn*)calloc(2, sizeof(jello_insn));
  entry[0] = ins(JOP_CONST_I32, 0, 0, 0, 0);
  entry[1] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[0] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, entry_types, 2, entry, 0, NULL};
  for(int i = 0; i < NPAIRS; i++) funcs[1 + i] = waiter(i, -1);
  funcs[1 + NPAIRS] = waiter(0, WAIT_MS);
  m->nfuncs = NPAIRS + 2;
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

/* Why `p` is parked, read as the poller's thread writes it. */
static uint8_t parked(jello_process* p) {
  pthread_mutex_lock(&p->mu);
  uint8_t why = p->parked;
  pthread_mutex_unlock(&p->mu);
  return why;
}

static void put(int i) {
  assert(jello_send(pairs[i][1], "x", 1) == 1);
}

static void drain(int i) {
  char c;
  assert(jello_recv(pairs[i][0], &c, 1) == 1);
  assert(jello_recv(pairs[i][0], &c, 1) == -1);
}

static double ms_since(uint64_t t0) {
  return (double)(proc_now() - t0) / 1e6;
}

static void watch(jello_vm* vm, int fd) {
  int watched = 0;
  assert(proc_poll_open(vm, fd, &watched) == JELLO_TRAP_NONE && watched);
}

static void open_all(jello_vm* vm) {
  for(int i = 0; i < NPAIRS; i++) {
    watch(vm, pairs[i][0]);
    watch(vm, pairs[i][1]);
  }
}

static void close_all(jello_vm* vm) {
  for(int i = 0; i < NPAIRS; i++) {
    assert(proc_poll_close(vm, pairs[i][0]) == JELLO_TRAP_NONE);
    assert(proc_poll_close(vm, pairs[i][1]) == JELLO_TRAP_NONE);
  }
}

/* Waits for pair `i`'s read end, readable or not within WAIT_MS. */
static uint32_t wait_read(jello_vm* vm, int i, int32_t ms) {
  uint32_t ready = 99;
  assert(proc_poll_wait(vm, pairs[i][0], PROC_FD_READ, proc_deadline(ms), &ready) == 0);
  return ready;
}

/* A write wakes each reader, parked meanwhile. */
static void readers(jello_vm* vm, int check_parked) {
  jello_process* ps[NPAIRS];
  for(int i = 0; i < NPAIRS; i++) ps[i] = spawn(vm, 1u + (uint32_t)i);
  proc_yield(vm);
  if(check_parked)
    for(int i = 0; i < NPAIRS; i++) assert(parked(ps[i]) == PROC_PARK_POLL);
  for(int i = NPAIRS - 1; i >= 0; i--) put(i);
  proc_wait_all(vm);
  for(int i = 0; i < NPAIRS; i++) {
    assert(proc_exit_of(ps[i]) == PROC_NORMAL && ps[i]->result.as.i32 == PROC_FD_READ);
    proc_release(ps[i]);
    drain(i);
  }
}

/* A host thread writing to pair 0 after WAIT_MS. */
static void* late_put(void* arg) {
  (void)arg;
  usleep(WAIT_MS * 1000u);
  put(0);
  return NULL;
}

static void check(jello_vm* vm, const char* mode) {
  open_all(vm);
  readers(vm, 1);

  /* Readiness found while no one waits is kept for the next wait; then only new readiness ends
   * one, here over many rounds, each a new edge. */
  put(0);
  usleep(WAIT_MS * 1000u / 5u); /* For the poller's thread to find it first. */
  assert(wait_read(vm, 0, -1) == PROC_FD_READ);
  drain(0);
  uint64_t t0 = proc_now();
  assert(wait_read(vm, 0, WAIT_MS) == 0 && ms_since(t0) >= WAIT_MS);
  for(int r = 0; r < ROUNDS; r++) {
    put(0);
    assert(wait_read(vm, 0, -1) == PROC_FD_READ);
    drain(0);
  }
  uint32_t ready = 0;
  assert(proc_poll_wait(vm, pairs[0][1], PROC_FD_WRITE, 0, &ready) == 0);
  assert(ready == PROC_FD_WRITE);

  /* A deadline ends another process's wait with nothing ready. */
  jello_process* p = spawn(vm, 1u + NPAIRS);
  assert(proc_join(vm, p) == JELLO_TRAP_NONE && proc_exit_of(p) == PROC_NORMAL);
  assert(p->result.as.i32 == 0);
  proc_release(p);

  /* A second reader traps; a stop ends the first's wait. */
  p = spawn(vm, 1);
  proc_yield(vm);
  assert(parked(p) == PROC_PARK_POLL);
  assert(proc_poll_wait(vm, pairs[0][0], PROC_FD_READ, 0, &ready) == JELLO_TRAP_TYPE_MISMATCH);
  assert(proc_stop(p, PROC_STOPPED));
  proc_wait_all(vm);
  assert(proc_exit_of(p) == PROC_STOPPED);
  proc_release(p);

  /* A close, or an open again, wakes a waiter closed; waits on, and closes of, what is not
   * watched trap, as does an open of no descriptor. */
  for(int again = 0; again < 2; again++) {
    p = spawn(vm, 1);
    proc_yield(vm);
    assert(parked(p) == PROC_PARK_POLL);
    if(again) watch(vm, pairs[0][0]);
    else assert(proc_poll_close(vm, pairs[0][0]) == JELLO_TRAP_NONE);
    proc_wait_all(vm);
    assert(proc_exit_of(p) == PROC_NORMAL && p->result.as.i32 == PROC_FD_CLOSED);
    proc_release(p);
    if(again) continue;
    assert(proc_poll_wait(vm, pairs[0][0], PROC_FD_READ, 0, &ready) == JELLO_TRAP_TYPE_MISMATCH);
    assert(proc_poll_close(vm, pairs[0][0]) == JELLO_TRAP_TYPE_MISMATCH);
    watch(vm, pairs[0][0]);
  }
  int watched = 1;
  assert(proc_poll_open(vm, -1, &watched) == JELLO_TRAP_TYPE_MISMATCH && !watched);

  /* The main process waiting with no other is woken from outside, not found deadlocked. */
  pthread_t writer;
  assert(pthread_create(&writer, NULL, late_put, NULL) == 0);
  assert(wait_read(vm, 0, -1) == PROC_FD_READ);
  pthread_join(writer, NULL);
  drain(0);
  close_all(vm);
  printf("  %s: passed\n", mode);
}

/* Runs `m`, then `fn` on its main process, interpreted and, where it can, as JIT code. */
static void run(jello_bc_module* m, void (*fn)(jello_vm*, const char*)) {
  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, m, &out) == JELLO_EXEC_OK);
  assert(proc_self(vm));
  fn(vm, "interpreted");
  jello_jit_module jm;
  memset(&jm, 0, sizeof jm);
  if(jello_jit_available() && jello_jit_module_prepare(m, &jm) == 1) {
    vm->active_jit = &jm;
    fn(vm, "JIT");
    vm->active_jit = NULL;
    jello_jit_module_free(&jm);
  }
  proc_end_all(vm);
  jello_vm_destroy(vm);
}

static void many(jello_vm* vm, const char* mode) {
  open_all(vm);
  for(int r = 0; r < 20; r++) readers(vm, 0);
  close_all(vm);
  printf("  %s on 4 schedulers: passed\n", mode);
}

/* Connects `fds` through a loopback listener: fds[1] sends at once, and fds[0] does not block. */
static void connect_pair(int fds[2]) {
  struct sockaddr_in at;
  memset(&at, 0, sizeof at);
  at.sin_family = AF_INET;
  at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof at;
  int listener = jello_sock_open(AF_INET, SOCK_STREAM);
  assert(listener >= 0 && jello_bind(listener, (struct sockaddr*)&at, len) == 0);
  assert(jello_listen(listener, 1) == 0);
  assert(jello_getsockname(listener, (struct sockaddr*)&at, &len) == 0);
  fds[1] = jello_sock_open(AF_INET, SOCK_STREAM);
  assert(fds[1] >= 0 && jello_connect(fds[1], (struct sockaddr*)&at, len) == 0);
  int on = 1;
  assert(setsockopt(JELLO_SOCKET(fds[1]), IPPROTO_TCP, TCP_NODELAY, (const char*)&on,
                    sizeof on) == 0);
  fds[0] = jello_accept(listener, NULL, NULL);
  assert(fds[0] >= 0 && jello_sock_nonblocking(fds[0]) == 0);
  jello_sock_close(listener);
}

int main(void) {
  for(int i = 0; i < NPAIRS; i++) connect_pair(pairs[i]);
  jello_bc_module* m = build_module();
  jello_setenv("JELLO_SCHEDULERS", "1");
  run(m, check);
  jello_setenv("JELLO_SCHEDULERS", "4");
  run(m, many);
  printf("poll_tests: passed\n");
  return 0;
}
