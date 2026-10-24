// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * VM lifecycle and threads; creating and destroying VMs, attaching them to a shared heap,
 * quiescence and safepoints, stop-the-world, fuel, and the per-VM settings of jello.h. The
 * collector scans only call frames, never C stacks, so a parked thread needs no saved context. */

#include <jello/internal.h>
#include <jello/internal/platform.h>
#include <jello/internal/proc_internal.h>
#include <stdlib.h>

static jello_heap* heap_new(void) {
  jello_heap* heap = (jello_heap*)calloc(1, sizeof(jello_heap));
  if(!heap) abort();
  if(pthread_mutex_init(&heap->alloc_lock, NULL) != 0) abort();
  if(pthread_mutex_init(&heap->stw_lock, NULL) != 0) abort();
  if(pthread_cond_init(&heap->stw_resume_cv, NULL) != 0) abort();
  if(pthread_mutex_init(&heap->proc_lock, NULL) != 0) abort();
  atomic_init(&heap->stop_requested, 0);
  return heap;
}

/** Attaches `vm` to `heap`, counting it, for `by`, a VM on it that may be running. */
static void heap_register(jello_heap* heap, jello_vm* vm, jello_vm* by) {
  if(by) vm_heap_lock(by);
  else pthread_mutex_lock(&heap->alloc_lock);
  if(heap->threads_len == heap->threads_cap) {
    uint32_t ncap = heap->threads_cap ? heap->threads_cap * 2u : 4u;
    jello_vm** nt = (jello_vm**)realloc(heap->threads, sizeof(jello_vm*) * (size_t)ncap);
    if(!nt) abort();
    heap->threads = nt;
    heap->threads_cap = ncap;
  }
  vm->heap_index = heap->threads_len;
  heap->threads[heap->threads_len++] = vm;
  heap->refcount++;
  pthread_mutex_unlock(&heap->alloc_lock);
}

/** Detaches `vm` from `heap`. Returns 1 when it was the last VM, and the caller then frees the
 * heap. */
static int heap_unregister(jello_heap* heap, jello_vm* vm) {
  pthread_mutex_lock(&heap->alloc_lock);
  jello_vm* moved = heap->threads[--heap->threads_len];
  heap->threads[vm->heap_index] = moved;
  moved->heap_index = vm->heap_index;
  heap->refcount--;
  int last = (heap->refcount == 0);
  pthread_mutex_unlock(&heap->alloc_lock);
  return last;
}

/** Reserves a default frame stack whole, as address space only until pushes commit its pages, so
 * it never moves; JIT activations hold raw pointers into it across nested calls. NULL when the
 * reservation fails. */
static uint8_t* frame_stack_reserve(void) {
  return (uint8_t*)jello_pages_reserve(VM_DEFAULT_FRAME_STACK_BYTES);
}

jello_vm* jello_vm_create(void) {
  uint8_t* mem = frame_stack_reserve();
  if(!mem) return NULL;
  jello_vm* vm = (jello_vm*)calloc(1, sizeof(jello_vm));
  if(!vm) {
    jello_pages_unmap(mem, VM_DEFAULT_FRAME_STACK_BYTES);
    return NULL;
  }
  vm_frame_stack_attach(vm, mem, VM_DEFAULT_FRAME_STACK_BYTES, VM_DEFAULT_CALL_FRAMES);
  vm->heap = heap_new();
  vm->space = &vm->heap->space;
  heap_register(vm->heap, vm, NULL);
  jello_gc_init(vm);
  return vm;
}

jello_vm* vm_create_attached(jello_vm* existing, uint8_t* mem, uint32_t bytes, uint32_t call_frames,
                             int own_space) {
  if(!existing || !existing->heap) return NULL;
  jello_vm* vm = (jello_vm*)calloc(1, sizeof(jello_vm));
  if(!vm) return NULL;
  vm->heap = existing->heap;
  vm->space = own_space ? (jello_space*)calloc(1, sizeof(jello_space)) : &vm->heap->space;
  if(!vm->space) {
    free(vm);
    return NULL;
  }
  vm_frame_stack_attach(vm, mem, bytes, call_frames);
  heap_register(vm->heap, vm, existing);
  jello_gc_init(vm); /* Keeps a shared space's existing threshold. */
  return vm;
}

jello_vm* jello_vm_create_joined(jello_vm* existing) {
  if(!existing || !existing->heap) return NULL;
  uint8_t* mem = frame_stack_reserve();
  if(!mem) return NULL;
  jello_vm* vm =
      vm_create_attached(existing, mem, VM_DEFAULT_FRAME_STACK_BYTES, VM_DEFAULT_CALL_FRAMES, 0);
  if(!vm) jello_pages_unmap(mem, VM_DEFAULT_FRAME_STACK_BYTES);
  return vm;
}

void jello_vm_keep_processes(jello_vm* vm, int keep) { vm->keep_processes = keep != 0; }

int jello_run_for(jello_vm* vm, uint32_t ms) { return proc_run_for(vm, ms); }

void jello_vm_destroy(jello_vm* vm) {
  if(!vm) return;
  vm_end_kept(vm);
  uint8_t* mem = vm->first_segment.mem;
  vm_destroy_attached(vm);
  jello_pages_unmap(mem, VM_DEFAULT_FRAME_STACK_BYTES);
}

void vm_destroy_attached(jello_vm* vm) {
  vm_frame_reset(vm);
  vm_frame_segments_free(vm);
  jello_heap* heap = vm->heap;
  if(heap) {
    jello_space* own = vm_owns_space(vm) ? vm->space : NULL;
    /* Unregistered first, so that no collection reads its roots once its space is gone. */
    int last = heap_unregister(heap, vm);
    if(own) {
      vm_space_free(own); /* Nothing else reaches its objects once it has finished. */
      free(own);
    }
    if(last) {
      vm_space_free(&heap->space); /* Frees objects and free lists, not the heap itself. */
      pthread_mutex_destroy(&heap->alloc_lock);
      pthread_mutex_destroy(&heap->stw_lock);
      pthread_cond_destroy(&heap->stw_resume_cv);
      pthread_mutex_destroy(&heap->proc_lock);
      free(heap->threads);
      free(heap);
    }
  }
  if(vm->owns_globals) free(vm->globals);
  free(vm->call_frames);
  free(vm->try_scopes);
  free(vm->gc_roots);
  free(vm);
}

void vm_heap_lock(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  if(pthread_mutex_trylock(&heap->alloc_lock) == 0) return;
  vm_go_quiescent(vm);
  pthread_mutex_lock(&heap->alloc_lock);
  /* No stop-the-world is in its stopped window while the lock is held, so there is none to wait
   * for: a later one waits for the lock, then for this VM to reach a safepoint. */
  atomic_fetch_sub(&vm->quiescent_depth, 1);
}

void vm_go_quiescent(jello_vm* vm) { atomic_fetch_add(&vm->quiescent_depth, 1); }

void vm_wait_for_resume(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  if(!atomic_load(&heap->stop_requested)) return;
  pthread_mutex_lock(&heap->stw_lock);
  while(atomic_load(&heap->stop_requested)) {
    pthread_cond_wait(&heap->stw_resume_cv, &heap->stw_lock);
  }
  pthread_mutex_unlock(&heap->stw_lock);
}

void vm_leave_quiescent(jello_vm* vm) {
  /* Wait while still counted quiescent: decrementing first would let the stop-the-world spin
   * wait for this thread while it waits for the world to resume; a deadlock. A stop can begin
   * between the wait and the decrement, having seen this thread quiescent, so check again once
   * counted running, and if one has, go back to waiting. Both sides store, then load, so one of
   * them sees the other. */
  for(;;) {
    vm_wait_for_resume(vm);
    atomic_fetch_sub(&vm->quiescent_depth, 1);
    if(!atomic_load(&vm->heap->stop_requested)) return;
    atomic_fetch_add(&vm->quiescent_depth, 1);
  }
}

void vm_safepoint_check(jello_vm* vm) {
  if(!atomic_load(&vm->heap->stop_requested)) return;
  vm_go_quiescent(vm);
  vm_wait_for_resume(vm);
  vm_leave_quiescent(vm);
}

/** Starts `vm`'s next window of reductions: VM_REDUCTIONS, or one more than the fuel left when that
 * is fewer, so the charge past the fuel ends the window. */
static void reductions_refill(jello_vm* vm) {
  uint32_t window = VM_REDUCTIONS;
  if(vm->fuel_limit && vm->fuel_remaining < window - 1u) window = (uint32_t)vm->fuel_remaining + 1u;
  vm->reduction_window = window;
  vm->reductions = (int32_t)window;
}

/** The reductions `vm` has used in its window so far; one more than were left when it ran out. */
static uint32_t reductions_used(const jello_vm* vm) {
  return vm->reduction_window - (vm->reductions > 0 ? (uint32_t)vm->reductions : 0u);
}

/** Charges the fuel for the reductions used in `vm`'s window so far, which is all of them when it
 * has run out. A window holds at most one charge past the fuel, so the fuel is short only when the
 * last charge was that one. Returns 0, or nonzero with a JELLO_TRAP_FUEL trap. */
static int reductions_settle(jello_vm* vm) {
  if(!vm->fuel_limit) return 0;
  uint32_t used = reductions_used(vm);
  if(vm->fuel_remaining < used) {
    vm->fuel_remaining = 0;
    vm->trap_code = JELLO_TRAP_FUEL;
    return -1;
  }
  vm->fuel_remaining -= used;
  return 0;
}

int vm_reductions_out(jello_vm* vm) {
  int trapped = reductions_settle(vm);
  reductions_refill(vm);
  return vm->proc ? proc_window_end(vm, trapped) : trapped;
}

void jello_vm_set_fuel(jello_vm* vm, uint64_t fuel) {
  if(!vm) return;
  vm->fuel_limit = fuel;
  vm->fuel_remaining = fuel;
  reductions_refill(vm);
}

void vm_inherit_fuel(jello_vm* vm, const jello_vm* from) {
  uint32_t used = reductions_used(from);
  vm->fuel_limit = from->fuel_limit;
  vm->fuel_remaining = from->fuel_remaining > used ? from->fuel_remaining - used : 0u;
  reductions_refill(vm);
}

void jello_vm_set_program_args(jello_vm* vm, int32_t argc, const char* const* argv) {
  if(!vm) return;
  vm->heap->program_argc = argc;
  vm->heap->program_argv = argv;
}

void jello_vm_set_image(jello_vm* vm, const uint8_t* data, size_t size) {
  if(!vm) return;
  vm->heap->image = data;
  vm->heap->image_size = size;
}

void jello_vm_set_max_bytes_len(jello_vm* vm, uint32_t max_len) {
  if(!vm) return;
  vm->max_bytes_len = max_len;
}

uint32_t jello_vm_last_trap_pc(const jello_vm* vm) { return vm ? vm->trap_pc : 0; }

uint32_t jello_vm_last_trap_func_index(const jello_vm* vm) { return vm ? vm->trap_func_index : 0; }

uint32_t jello_vm_last_trap_callers(const jello_vm* vm, uint32_t* func_indexes, uint32_t* pcs,
                                    uint8_t* repeated, uint32_t max) {
  if(!vm) return 0;
  uint32_t n = vm->trap_trace.len < max ? vm->trap_trace.len : max;
  for(uint32_t i = 0; i < n; i++) {
    func_indexes[i] = vm->trap_trace.func_index[i];
    pcs[i] = vm->trap_trace.pc[i];
    if(repeated) repeated[i] = vm->trap_trace.repeated[i];
  }
  return n;
}

const char* jello_trap_code_name(jello_trap_code tc) {
  switch(tc) {
    case JELLO_TRAP_NONE: return "NONE";
    case JELLO_TRAP_ARITHMETIC: return "ARITHMETIC";
    case JELLO_TRAP_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case JELLO_TRAP_NULL_DEREF: return "NULL_DEREF";
    case JELLO_TRAP_BOUNDS: return "BOUNDS";
    case JELLO_TRAP_TYPE_MISMATCH: return "TYPE_MISMATCH";
    case JELLO_TRAP_FUEL: return "FUEL";
    case JELLO_TRAP_LIMIT: return "LIMIT";
    case JELLO_TRAP_UNCAUGHT_THROW: return "UNCAUGHT_THROW";
    case JELLO_TRAP_STOPPED: return "STOPPED";
    case JELLO_TRAP_DEADLOCK: return "DEADLOCK";
    case JELLO_TRAP_UNSENDABLE: return "UNSENDABLE";
    case JELLO_TRAP_SEALED: return "SEALED";
    default: return "UNKNOWN";
  }
}

jello_value jello_vm_last_thrown(const jello_vm* vm) { return vm ? vm->thrown : jello_make_null(); }

const char* jello_vm_last_trap_detail(const jello_vm* vm) { return vm ? vm->trap_detail : ""; }

void jello_vm_blocking(jello_vm* vm, int is_blocking) {
  if(!vm) abort();
  if(is_blocking) {
    vm_go_quiescent(vm);
  } else {
    if(atomic_load(&vm->quiescent_depth) == 0) abort(); /* Unbalanced enter and exit. */
    vm_leave_quiescent(vm);
  }
}

void jello_vm_running(jello_vm* vm, int is_running) {
  if(!vm) abort();
  if(is_running) {
    if(atomic_exchange(&vm->in_exec, VM_RUNNING_OUTSIDE)) abort(); /* Already running. */
    /* Counted running first, as a run is, so that no stop begins unseen before the check. */
    vm_safepoint_check(vm);
  } else if(atomic_exchange(&vm->in_exec, 0) != VM_RUNNING_OUTSIDE) {
    abort(); /* Not marked running. */
  }
}

int vm_heap_stop_the_world(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  int expected = 0;
  if(!atomic_compare_exchange_strong(&heap->stop_requested, &expected, 1)) {
    vm_go_quiescent(vm);
    vm_wait_for_resume(vm);
    vm_leave_quiescent(vm);
    return 0;
  }

  /* Held until resume, so no thread allocates, registers or unregisters while stopped. */
  pthread_mutex_lock(&heap->alloc_lock);
  for(uint32_t i = 0; i < heap->threads_len; i++) {
    jello_vm* t = heap->threads[i];
    if(t == vm) continue;
    while(atomic_load(&t->quiescent_depth) == 0 && atomic_load(&t->in_exec) != 0) {
      /* t is running and has not reached a safepoint; every loop passes one, interpreted
       * (vm_poll_and_charge) or compiled (JIR_SAFEPOINT), or allocates. */
    }
  }
  return 1;
}

void vm_heap_resume_the_world(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  pthread_mutex_unlock(&heap->alloc_lock);
  atomic_store(&heap->stop_requested, 0);
  pthread_mutex_lock(&heap->stw_lock);
  pthread_cond_broadcast(&heap->stw_resume_cv);
  pthread_mutex_unlock(&heap->stw_lock);
}
