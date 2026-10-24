// SPDX-License-Identifier: BSD-3-Clause
// Processes talking through the built-in vm library, interpreted and as JIT code: messages copied
// with their sharing and cycles, a ring of processes passing a token, stopping parked processes,
// deadlock, on one scheduler and on several, how processes finish, and the lifetime of process handles.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jello/internal.h>
#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/platform.h>

enum { T_I32, T_BOOL, T_BYTES, T_DYN, T_ARR, T_LIST, T_OBJ, NTYPES };
enum { N_RECEIVE, N_SEND, NIMPORTS };

static jello_insn* insns(int n) {
  return (jello_insn*)calloc((size_t)n, sizeof(jello_insn));
}

static jello_insn ins(jello_op op, uint8_t a, uint8_t b, uint8_t c, int32_t imm) {
  jello_insn i = {0};
  i.op = (uint8_t)op;
  i.a = a;
  i.b = b;
  i.c = c;
  i.imm = (uint32_t)imm;
  return i;
}

/* func 0, the entry: returns 0.
 * func 1, a ring member, capturing the next process and the laps: passes on each message it
 *   receives, laps times, then returns the laps.
 * func 2, receives forever.
 * func 3, throws a Bytes of 4 bytes, its byte 0 0x77.
 * func 4, returns 5. */
static jello_bc_module* build_module(void) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(NTYPES, sizeof(jello_type_entry));
  types[T_I32].kind = JELLO_T_I32;
  types[T_BOOL].kind = JELLO_T_BOOL;
  types[T_BYTES].kind = JELLO_T_BYTES;
  types[T_DYN].kind = JELLO_T_DYNAMIC;
  types[T_ARR].kind = JELLO_T_ARRAY;
  types[T_ARR].as.unary.elem = T_DYN;
  types[T_LIST].kind = JELLO_T_LIST;
  types[T_LIST].as.unary.elem = T_DYN;
  types[T_OBJ].kind = JELLO_T_OBJECT;
  m->ntypes = NTYPES;
  m->types = types;

  jello_jdll_handle* vm_lib = jello_jdll_load("vm");
  assert(vm_lib);
  jello_import_sig* imports = (jello_import_sig*)calloc(NIMPORTS, sizeof(jello_import_sig));
  imports[N_RECEIVE].ret_kind = (uint8_t)JELLO_T_DYNAMIC;
  imports[N_RECEIVE].resolved = jello_jdll_find_export(vm_lib, "proc_receive");
  imports[N_SEND].ret_kind = (uint8_t)JELLO_T_BOOL;
  imports[N_SEND].arity = 2;
  imports[N_SEND].arg_kinds[0] = (uint8_t)JELLO_T_DYNAMIC;
  imports[N_SEND].arg_kinds[1] = (uint8_t)JELLO_T_DYNAMIC;
  imports[N_SEND].resolved = jello_jdll_find_export(vm_lib, "proc_send");
  assert(imports[N_RECEIVE].resolved && imports[N_SEND].resolved);
  m->nimports = NIMPORTS;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = vm_lib;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_bc_function* funcs = (jello_bc_function*)calloc(5, sizeof(jello_bc_function));

  static const jello_type_id entry_types[1] = {T_I32};
  jello_insn* entry = insns(2);
  entry[0] = ins(JOP_CONST_I32, 0, 0, 0, 0);
  entry[1] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[0] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, entry_types, 2, entry, 0, NULL};

  /* r0 next, r1 laps (captures), r2 i, r3 next again, r4 the message, beside it to send; r5
   * whether it went, then the loop test. */
  static const jello_type_id ring_types[6] = {T_DYN, T_I32, T_I32, T_DYN, T_DYN, T_BOOL};
  jello_insn* ring = insns(8);
  ring[0] = ins(JOP_CONST_I32, 2, 0, 0, 0);
  ring[1] = ins(JOP_MOV, 3, 0, 0, 0);
  ring[2] = ins(JOP_CALL_NATIVE, 4, 0, 0, N_RECEIVE);
  ring[3] = ins(JOP_CALL_NATIVE, 5, 3, 2, N_SEND);
  ring[4] = ins(JOP_ADD_I32_IMM, 2, 2, 0, 1);
  ring[5] = ins(JOP_LT_I32, 5, 2, 1, 0);
  ring[6] = ins(JOP_JMP_IF, 5, 0, 0, -5); /* to 2 */
  ring[7] = ins(JOP_RET, 2, 0, 0, 0);
  funcs[1] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 6, 0, ring_types, 8, ring, 0, NULL};

  static const jello_type_id forever_types[1] = {T_DYN};
  jello_insn* forever = insns(3);
  forever[0] = ins(JOP_CALL_NATIVE, 0, 0, 0, N_RECEIVE);
  forever[1] = ins(JOP_JMP, 0, 0, 0, -2); /* to 0 */
  forever[2] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[2] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, forever_types, 3, forever, 0, NULL};

  static const jello_type_id throw_types[4] = {T_I32, T_BYTES, T_DYN, T_I32};
  jello_insn* thrower = insns(8);
  thrower[0] = ins(JOP_CONST_I32, 0, 0, 0, 4);
  thrower[1] = ins(JOP_BYTES_NEW, 1, 0, 0, 0);
  thrower[2] = ins(JOP_CONST_I32, 0, 0, 0, 0x77);
  thrower[3] = ins(JOP_CONST_I32, 3, 0, 0, 0);
  thrower[4] = ins(JOP_BYTES_SET_U8, 0, 1, 3, 0);
  thrower[5] = ins(JOP_TO_DYN, 2, 1, 0, 0);
  thrower[6] = ins(JOP_THROW, 2, 0, 0, 0);
  thrower[7] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[3] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 4, 4, throw_types, 8, thrower, 0, NULL};

  static const jello_type_id five_types[1] = {T_I32};
  jello_insn* five = insns(2);
  five[0] = ins(JOP_CONST_I32, 0, 0, 0, 5);
  five[1] = ins(JOP_RET, 0, 0, 0, 0);
  funcs[4] = (jello_bc_function){JELLO_BC_NO_NAME, 0, 1, 1, five_types, 2, five, 0, NULL};

  m->nfuncs = 5;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

/* A handle to `p` holding a reference, as the vm library returns one. */
static jello_value handle_of(jello_vm* vm, jello_process* p) {
  proc_retain(p);
  jello_handle* h = jello_handle_new(vm, &proc_handle_class, p, (uint32_t)sizeof(jello_process));
  return jello_from_ptr(h);
}

static jello_process* spawn(jello_vm* vm, uint32_t fi, uint32_t ncaps, const uint64_t* caps) {
  jello_function* body = jello_function_new(vm, fi, ncaps);
  if(ncaps) memcpy(body->captures, caps, sizeof(uint64_t) * ncaps);
  jello_trap_code trap;
  jello_process* p = proc_spawn(vm, body, NULL, 0, &trap);
  assert(p);
  return p;
}

static jello_value dyn_at(const jello_array* a, uint32_t i) {
  return ((const jello_value*)a->data)[i];
}

/* `v` packed to leave the process, which it may. */
static vm_parcel packed(jello_vm* vm, jello_value v) {
  vm_parcel pc;
  jello_trap_code code = vm_parcel_pack(vm, v, 0, &pc);
  assert(code == JELLO_TRAP_NONE && !pc.protos);
  (void)code;
  return pc;
}

/* `v` packed and taken back, as a message the process sends itself arrives. */
static jello_value sent_back(jello_vm* vm, jello_value v) {
  vm_parcel pc = packed(vm, v);
  return vm_parcel_take(vm, &pc);
}

/* A copy keeps its cycle and its sharing, copies Bytes and functions but shares immortals, and
 * is not the original. */
static void check_copy_shape(jello_vm* vm, const jello_bc_module* m) {
  jello_bytes* b = jello_bytes_new(vm, 8);
  jello_gc_push_root(vm, b);
  jello_array* inner = jello_array_new(vm, m, T_ARR, 1);
  ((jello_value*)inner->data)[0] = jello_from_ptr(b);
  jello_gc_push_root(vm, inner);
  jello_array* a = jello_array_new(vm, m, T_ARR, 4);
  jello_value* av = (jello_value*)a->data;
  av[0] = jello_from_ptr(b);
  av[1] = jello_from_ptr(a);
  av[2] = jello_from_ptr(inner);
  av[3] = jello_from_ptr(inner);
  jello_gc_push_root(vm, a);

  jello_array* c = (jello_array*)jello_as_ptr(sent_back(vm, jello_from_ptr(a)));
  assert(c != a && c->length == 4);
  void* cb = jello_as_ptr(dyn_at(c, 0));
  assert(cb != b && ((const jello_bytes*)cb)->length == 8 && "Bytes are copied");
  assert(jello_as_ptr(dyn_at(c, 1)) == c && "the cycle leads to the copy");
  jello_array* ci = (jello_array*)jello_as_ptr(dyn_at(c, 2));
  assert(ci != inner && jello_as_ptr(dyn_at(c, 3)) == ci && "sharing survives");
  assert(jello_as_ptr(dyn_at(ci, 0)) == cb);
  ((jello_value*)inner->data)[0] = jello_make_null();
  assert(jello_as_ptr(dyn_at(ci, 0)) == cb && "the copy does not see the original's writes");

  jello_object* o = jello_object_new(vm, T_OBJ);
  jello_gc_push_root(vm, o);
  jello_object_set(o, 3, jello_from_ptr(inner));
  vm_object_set_f64(vm, o, 5, 1.5);
  jello_value oc = sent_back(vm, jello_from_ptr(o));
  const jello_object* co = (const jello_object*)jello_as_ptr(oc);
  assert(co != o);
  jello_value got = jello_object_get(co, 3);
  assert(jello_as_ptr(got) != inner && ((const jello_array*)jello_as_ptr(got))->length == 1);
  vm_object_set_f64(vm, o, 5, 2.5);
  const jello_box_f64* box = (const jello_box_f64*)jello_as_ptr(jello_object_get(co, 5));
  assert(box->value == 1.5 && "a number the sender's object owned is not written through");

  jello_function* f = jello_function_new(vm, 1, 2);
  f->captures[0] = (uint64_t)jello_from_ptr(inner);
  f->captures[1] = 9;
  jello_gc_push_root(vm, f);
  jello_function* fc = (jello_function*)jello_as_ptr(sent_back(vm, jello_from_ptr(f)));
  assert(fc != f && fc->func_index == 1 && fc->captures[1] == 9);
  assert(jello_as_ptr((jello_value)fc->captures[0]) != inner && "captures are copied");
  jello_function* plain = jello_function_new(vm, 4, 0);
  jello_gc_push_root(vm, plain);
  assert(jello_as_ptr(sent_back(vm, jello_from_ptr(plain))) != plain);
  jello_function* immortal = vm_function_immortal(4, 0);
  jello_gc_push_root(vm, immortal);
  assert(jello_as_ptr(sent_back(vm, jello_from_ptr(immortal))) == immortal);
  jello_gc_collect(vm);
  assert(vm_is_immortal(immortal) && "a collection reaching an immortal leaves its mark alone");
  jello_gc_pop_roots(vm, 7);
  vm_immortal_free(immortal);
}

/* A copy for another process owns its Bytes and boxes, a slice's copy only its range and a
 * buffer's copy is a buffer; an immortal literal is shared. */
static void check_copy_out_flat(jello_vm* vm) {
  jello_bytes* b = jello_bytes_new_zeroed(vm, 64);
  b->data[10] = 7;
  jello_gc_push_root(vm, b);
  jello_bytes* slice = jello_bytes_slice(vm, b, 8, 4);
  jello_gc_push_root(vm, slice);
  jello_box_f64* box = jello_box_f64_new(vm, 2.5);
  jello_gc_push_root(vm, box);
  const jello_bytes* copy = (const jello_bytes*)jello_as_ptr(sent_back(vm, jello_from_ptr(slice)));
  assert(copy != slice && !copy->owner && copy->length == 4 && copy->data[2] == 7);
  jello_value v = sent_back(vm, jello_from_ptr(box));
  assert(jello_as_ptr(v) != box && jello_as_box_f64(v) == 2.5);
  jello_bytes* buffer = vm_buffer_new(vm, 8);
  jello_gc_push_root(vm, buffer);
  buffer->data[2] = 5;
  copy = (const jello_bytes*)jello_as_ptr(sent_back(vm, jello_from_ptr(buffer)));
  assert(copy != buffer && !copy->owner && copy->length == 8 && copy->data[2] == 5);
  assert(vm_bytes_is_buffer(copy) && "a buffer's copy stays one");
  jello_bytes* part = jello_bytes_slice(vm, buffer, 2, 3);
  assert(!part->owner && !vm_bytes_is_buffer(part) && part->data[0] == 5 &&
         "a buffer's slice is a copy");
  jello_gc_pop_roots(vm, 4);
  void* block = calloc(1, vm_bytes_immortal_size(2));
  jello_bytes* literal = vm_bytes_immortal_at(block, 2);
  memcpy(literal->storage, "hi", 2);
  vm_parcel pc = packed(vm, jello_from_ptr(literal));
  assert(jello_as_ptr(pc.value) == literal && !pc.objects);
  free(block);
}

static const jello_bytes* sent_bytes(jello_vm* vm, const jello_bytes* b) {
  return (const jello_bytes*)jello_as_ptr(sent_back(vm, jello_from_ptr((void*)b)));
}

/* A large Bytes's copy shares its bytes, a small one's, and a buffer's, copies them, and a frozen
 * buffer's shares them too; slicing takes no reference. Each shared binary is freed once its last
 * holder lets go, whether a collection, a dropped parcel or a finished process frees it. */
static void check_copy_shares_large(jello_vm* vm) {
  jello_gc_collect(vm);
  uint32_t live = vm_shared_binaries_live();
  jello_bytes* b = jello_bytes_new_zeroed(vm, 100);
  b->data[40] = 9;
  jello_gc_push_root(vm, b);
  assert(vm_shared_binaries_live() == live + 1 && b->shared);
  const jello_bytes* copy = sent_bytes(vm, b);
  assert(copy != b && copy->data == b->data && copy->owner == b->owner);
  jello_bytes* slice = jello_bytes_slice(vm, b, 10, VM_BYTES_SHARED_MIN);
  jello_gc_push_root(vm, slice);
  assert(slice->owner == b && "a slice holds the Bytes holding the reference");
  copy = sent_bytes(vm, slice);
  assert(copy->data == slice->data && copy->owner == b->owner && copy->data[30] == 9);
  copy = sent_bytes(vm, jello_bytes_slice(vm, b, 39, 3));
  assert(!copy->owner && copy->data[1] == 9 && "a small slice's copy has bytes of its own");

  jello_bytes* buffer = vm_buffer_new(vm, 100);
  jello_gc_push_root(vm, buffer);
  copy = sent_bytes(vm, buffer);
  assert(copy->data != buffer->data && vm_bytes_is_buffer(copy) && "a buffer is never shared");
  buffer->h.type_id = 0; /* As BYTES_FREEZE leaves it. */
  assert(sent_bytes(vm, buffer)->data == buffer->data);
  assert(vm_shared_binaries_live() == live + 3);

  vm_parcel pc = packed(vm, jello_from_ptr(b));
  jello_process* p = spawn(vm, 2, 0, NULL);
  proc_yield(vm);
  vm_parcel msg = packed(vm, jello_from_ptr(slice));
  proc_send(NULL, p, &msg);
  proc_yield(vm);
  assert(p->inbox_len == 0 && "the process took the message into its space");
  jello_gc_pop_roots(vm, 3);
  jello_gc_collect(vm);
  assert(vm_shared_binaries_live() == live + 1 && "the parcel and the process still hold b's");
  vm_parcel_free(&pc);
  assert(vm_shared_binaries_live() == live + 1);
  proc_stop(p, PROC_STOPPED);
  proc_wait_all(vm);
  proc_release(p);
  assert(vm_shared_binaries_live() == live && "a finished process lets go of what it held");
}

/* A long list copies without recursing; neither packing it nor reading a copy collects, and
 * both the copy read and the parcel taken arrive whole. */
static void check_copy_large(jello_vm* vm, const jello_bc_module* m) {
  jello_list* l = NULL;
  jello_gc_push_root(vm, NULL);
  for(int32_t i = 0; i < 100000; i++) {
    jello_array* cell = jello_array_new(vm, m, T_ARR, 1);
    jello_gc_push_root(vm, cell);
    l = jello_list_cons(vm, T_LIST, jello_from_ptr(cell), l);
    jello_gc_pop_roots(vm, 2);
    jello_gc_push_root(vm, l);
  }
  uint32_t before = 0, after = 0;
  jello_vm_gc_stats(vm, NULL, &before, NULL);
  vm_parcel pc = packed(vm, jello_from_ptr(l));
  jello_list* r = (jello_list*)jello_as_ptr(vm_parcel_read(vm, &pc));
  jello_vm_gc_stats(vm, NULL, &after, NULL);
  assert(after == before && "copying never collects");
  jello_list* t = (jello_list*)jello_as_ptr(vm_parcel_take(vm, &pc));
  for(int i = 0; i < 2; i++) {
    uint32_t n = 0;
    for(jello_list* at = i ? t : r, *orig = l; at; at = at->tail, orig = orig->tail, n++) {
      assert(at != orig && jello_as_ptr(at->head) != jello_as_ptr(orig->head));
      assert(((const jello_array*)jello_as_ptr(at->head))->length == 1);
    }
    assert(n == 100000);
  }
  assert(r != t && jello_as_ptr(r->head) != jello_as_ptr(t->head));
  jello_gc_pop_roots(vm, 1);
}

/* 1,000 processes in a ring pass a token round 5 times, each lap ending at the main process. */
static void check_ring(jello_vm* vm) {
  enum { N = 1000, LAPS = 5 };
  jello_process* ps[N];
  jello_value next = handle_of(vm, proc_self(vm));
  jello_gc_push_root(vm, jello_as_ptr(next));
  for(int i = N; i-- > 0;) {
    uint64_t caps[2] = {(uint64_t)next, LAPS};
    ps[i] = spawn(vm, 1, 2, caps);
    next = handle_of(vm, ps[i]);
    jello_gc_pop_roots(vm, 1);
    jello_gc_push_root(vm, jello_as_ptr(next));
  }
  jello_bytes* token = jello_bytes_new(vm, 1);
  token->data[0] = 0x5a;
  jello_gc_push_root(vm, token);
  for(int lap = 0; lap < LAPS; lap++) {
    vm_parcel msg = packed(vm, jello_from_ptr(token));
    proc_send(NULL, ps[0], &msg);
    jello_value got;
    assert(proc_receive(vm, &got) == JELLO_TRAP_NONE);
    const jello_bytes* back = (const jello_bytes*)jello_as_ptr(got);
    assert(back->length == 1 && back->data[0] == 0x5a && "each hop's copy arrives whole");
  }
  proc_wait_all(vm);
  for(int i = 0; i < N; i++) {
    assert(proc_exit_of(ps[i]) == PROC_NORMAL && ps[i]->result.as.i32 == LAPS);
    proc_release(ps[i]);
  }
  jello_gc_pop_roots(vm, 2);
}

/* A parked receiver stops, as does one with a message pending, and neither runs on. */
static void check_stop_parked(jello_vm* vm) {
  jello_process* idle = spawn(vm, 2, 0, NULL);
  jello_process* pending = spawn(vm, 2, 0, NULL);
  proc_yield(vm);
  assert(idle->parked == PROC_PARK_RECEIVE && pending->parked == PROC_PARK_RECEIVE);
  proc_stop(idle, PROC_STOPPED);
  vm_parcel msg = packed(vm, jello_make_null());
  proc_send(NULL, pending, &msg);
  proc_stop(pending, PROC_STOPPED);
  proc_wait_all(vm);
  assert(proc_exit_of(idle) == PROC_STOPPED && proc_exit_of(pending) == PROC_STOPPED);
  proc_release(idle);
  proc_release(pending);
}

/* Waiting for what no process can do traps DEADLOCK rather than hanging. */
static void check_deadlock(jello_vm* vm) {
  jello_value got;
  assert(proc_receive(vm, &got) == JELLO_TRAP_DEADLOCK);
  jello_process* idle = spawn(vm, 2, 0, NULL);
  assert(proc_join(vm, idle) == JELLO_TRAP_DEADLOCK);
  assert(idle->parked == PROC_PARK_RECEIVE && !idle->joiners && !proc_self(vm)->joining);
  assert(proc_join(vm, proc_self(vm)) == JELLO_TRAP_DEADLOCK);
  proc_stop(idle, PROC_STOPPED);
  assert(proc_join(vm, idle) == JELLO_TRAP_NONE && proc_exit_of(idle) == PROC_STOPPED);
  proc_release(idle);
}

/* Deadlock is found too once other schedulers' threads have started, all of them idle. */
static void check_deadlock_schedulers(jello_vm* vm) {
  jello_setenv("JELLO_SCHEDULERS", "4");
  jello_process* idle = spawn(vm, 2, 0, NULL);
  assert(proc_self(vm)->sched->started > 1);
  jello_value got;
  assert(proc_receive(vm, &got) == JELLO_TRAP_DEADLOCK);
  assert(proc_join(vm, idle) == JELLO_TRAP_DEADLOCK);
  proc_stop(idle, PROC_STOPPED);
  assert(proc_join(vm, idle) == JELLO_TRAP_NONE && proc_exit_of(idle) == PROC_STOPPED);
  proc_release(idle);
  proc_end_all(vm);
  jello_setenv("JELLO_SCHEDULERS", "1");
}

/* The ring on four schedulers, round after round: each process is woken by one that may run on
 * another scheduler, often while it is still parking, and some are stolen. */
static void check_ring_schedulers(jello_vm* vm) {
  jello_setenv("JELLO_SCHEDULERS", "4");
  for(int round = 0; round < 20; round++) check_ring(vm);
  assert(proc_self(vm)->sched->started > 1);
  proc_end_all(vm);
  jello_setenv("JELLO_SCHEDULERS", "1");
}

/* How processes finish, and a crash's value kept by its handle across collections. */
static void check_exits(jello_vm* vm) {
  jello_process* five = spawn(vm, 4, 0, NULL);
  jello_process* thrower = spawn(vm, 3, 0, NULL);
  jello_value h = handle_of(vm, thrower);
  jello_gc_push_root(vm, jello_as_ptr(h));
  proc_release(thrower);
  assert(proc_join(vm, five) == JELLO_TRAP_NONE && proc_exit_of(five) == PROC_NORMAL);
  assert(five->result.as.i32 == 5);
  assert(proc_join(vm, thrower) == JELLO_TRAP_NONE && !thrower->vm);
  assert(proc_exit_of(thrower) == PROC_CRASHED && thrower->trap == JELLO_TRAP_UNCAUGHT_THROW);
  jello_gc_collect(vm);
  jello_gc_collect(vm);
  const jello_bytes* thrown = (const jello_bytes*)jello_as_ptr(thrower->exit.value);
  assert(thrown->length == 4 && thrown->data[0] == 0x77);
  proc_release(five);
  jello_gc_pop_roots(vm, 1);
}

/* Timers end waits and sleeps, and what one has yet to send survives collections. */
static void check_timers(jello_vm* vm, const jello_bc_module* m) {
  jello_bytes* b = jello_bytes_new(vm, 4);
  b->data[0] = 0x55;
  jello_gc_push_root(vm, b);
  jello_array* a = jello_array_new(vm, m, T_ARR, 1);
  ((jello_value*)a->data)[0] = jello_from_ptr(b);
  jello_gc_pop_roots(vm, 1);
  uint64_t due = proc_deadline(5);
  vm_parcel msg = packed(vm, jello_from_ptr(a));
  proc_timer_release(proc_send_after(vm, due, &msg));
  jello_gc_collect(vm);
  jello_gc_collect(vm);
  jello_value got;
  assert(proc_receive(vm, &got) == JELLO_TRAP_NONE && proc_now() >= due);
  const jello_bytes* sent = (const jello_bytes*)jello_as_ptr(dyn_at(jello_as_ptr(got), 0));
  assert(sent->length == 4 && sent->data[0] == 0x55);

  due = proc_deadline(2);
  assert(proc_wait_message(vm, due) == JELLO_TRAP_NONE && proc_now() >= due);
  assert(!proc_self(vm)->inbox_len);
  jello_process* idle = spawn(vm, 2, 0, NULL);
  due = proc_deadline(2);
  assert(proc_await(vm, idle, due) == JELLO_TRAP_NONE && proc_now() >= due && !proc_finished(idle));
  assert(idle->parked == PROC_PARK_RECEIVE && !proc_self(vm)->sched->ntimers);
  due = proc_deadline(3);
  assert(proc_sleep(vm, due) == JELLO_TRAP_NONE && proc_now() >= due);
  msg = packed(vm, jello_make_i32(1));
  proc_timer* never = proc_send_after(vm, proc_deadline(60000), &msg);
  assert(proc_timer_cancel(vm, never) && !proc_timer_cancel(vm, never));
  proc_timer_release(never);
  proc_stop(idle, PROC_STOPPED);
  assert(proc_join(vm, idle) == JELLO_TRAP_NONE && proc_exit_of(idle) == PROC_STOPPED);
  proc_release(idle);
  /* Left pending, for proc_end_all to cancel rather than wait for. */
  msg = packed(vm, jello_make_i32(2));
  proc_timer_release(proc_send_after(vm, proc_deadline(60000), &msg));
}

/* Dropped handles free their records when collected, and a handle to the main process outlives
 * its scheduler. */
static void check_handles(jello_vm* vm) {
  uint32_t finalized = vm->space->finalized_handles;
  for(int i = 0; i < 10; i++) {
    jello_process* p = spawn(vm, 4, 0, NULL);
    (void)handle_of(vm, p);
    proc_release(p);
  }
  proc_wait_all(vm);
  jello_value main = handle_of(vm, proc_self(vm));
  jello_gc_push_root(vm, jello_as_ptr(main));
  jello_gc_collect(vm);
  assert(vm->space->finalized_handles - finalized >= 10);
  jello_process* idle = spawn(vm, 2, 0, NULL);
  proc_release(idle);
  proc_end_all(vm);
  const jello_process* m = (const jello_process*)((const jello_handle*)jello_as_ptr(main))->payload;
  assert(proc_exit_of(m) == PROC_NORMAL && !m->vm && !vm->proc);
  jello_gc_pop_roots(vm, 1);
  jello_gc_collect(vm);
}

static int holds;
static void hold(void* payload) { (void)payload; holds++; }
static void unhold(void* payload) { (void)payload; holds--; }
static const jello_jdll_handle_class HELD = {"held", unhold, hold};
static const jello_jdll_handle_class LOCAL = {.name = "local"};

/* A handle copied out for another process is a new handle holding the same payload, released
 * when collected; one whose class has no `retain` is refused, as is a
 * spawn whose body captures one, or copied as null when dropping. */
static void check_copy_handles(jello_vm* vm) {
  static int payload;
  holds = 1;
  jello_handle* held = jello_handle_new(vm, &HELD, &payload, 0);
  jello_gc_push_root(vm, held);
  jello_handle* local = jello_handle_new(vm, &LOCAL, &payload, 0);
  jello_gc_push_root(vm, local);
  const jello_handle* copy = (const jello_handle*)jello_as_ptr(sent_back(vm, jello_from_ptr(held)));
  assert(copy != held && copy->payload == &payload && copy->cls == &HELD && holds == 2);
  vm_parcel pc = packed(vm, jello_from_ptr(held));
  assert(holds == 3);
  vm_parcel_free(&pc);
  assert(holds == 2 && "a parcel freed finalises its handles");
  jello_value v = jello_from_ptr(local);
  assert(vm_parcel_pack(vm, v, 0, &pc) == JELLO_TRAP_UNSENDABLE);
  assert(jello_is_null(pc.value) && !pc.protos && !pc.objects);
  assert(vm_parcel_pack(vm, v, 1, &pc) == JELLO_TRAP_NONE && jello_is_null(pc.value));
  jello_function* body = jello_function_new(vm, 1, 2);
  body->captures[0] = (uint64_t)jello_from_ptr(local);
  jello_trap_code trap;
  assert(!proc_spawn(vm, body, NULL, 0, &trap) && trap == JELLO_TRAP_UNSENDABLE);
  assert(!proc_self(vm)->children);
  jello_gc_pop_roots(vm, 2);
  jello_gc_collect(vm);
  assert(holds == 0);
  proc_end_all(vm);
}

/* A VM runs a function again after one trapped, its frames unwound, as a finished process runs
 * its cleanups. */
static void check_rerun(jello_vm* vm) {
  jello_function* thrower = jello_function_new(vm, 3, 0);
  jello_gc_push_root(vm, thrower);
  jello_function* five = jello_function_new(vm, 4, 0);
  jello_gc_push_root(vm, five);
  jello_numeric_result r;
  assert(vm_exec_function(vm, thrower, NULL, 0, &r) == JELLO_EXEC_TRAP);
  assert(vm->trap_code == JELLO_TRAP_UNCAUGHT_THROW);
  vm_frame_unwind(vm);
  assert(!vm->call_frames_len && !vm->live_try_scopes);
  assert(vm_exec_function(vm, five, NULL, 0, &r) == JELLO_EXEC_OK && r.as.i32 == 5);
  jello_gc_pop_roots(vm, 2);
}

static void check(jello_vm* vm, const jello_bc_module* m, const char* mode) {
  check_rerun(vm);
  check_ring(vm);
  check_stop_parked(vm);
  check_deadlock(vm);
  check_exits(vm);
  check_timers(vm, m);
  check_handles(vm);
  check_copy_handles(vm);
  check_deadlock_schedulers(vm);
  check_ring_schedulers(vm);
  printf("  %s: passed\n", mode);
}

int main(void) {
  jello_setenv("JELLO_SCHEDULERS", "1"); /* These checks follow one scheduler's order. */
  jello_bc_module* m = build_module();
  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, m, &out) == JELLO_EXEC_OK);
  check_copy_shape(vm, m);
  check_copy_large(vm, m);
  check_copy_out_flat(vm);
  check_copy_shares_large(vm);
  check(vm, m, "interpreted");

  jello_jit_module jm;
  memset(&jm, 0, sizeof jm);
  if(jello_jit_available() && jello_jit_module_prepare(m, &jm) == 1) {
    vm->active_jit = &jm;
    uint32_t enters = jit_try_enter_count;
    check(vm, m, "JIT");
    assert(jit_try_enter_count > enters && "processes ran their bodies as JIT code");
    vm->active_jit = NULL;
    jello_jit_module_free(&jm);
  }

  jello_vm_destroy(vm);
  printf("proc_native_tests: passed\n");
  return 0;
}
