// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * JIT module preparation, lazy compilation, and the interpreter's ways into JIT code. */

#include <jello/internal/jit_internal.h>
#include <jello/internal/platform.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** Bytes to reserve for `f`'s code. The JIR has at most two instructions per bytecode
 * instruction (a SAFEPOINT before some), so sizing needs no IR build. */
static size_t function_emit_budget(const jello_bc_function* f) {
  return 2u * (size_t)f->ninsns * JIT_EMIT_CALL_INSN_BUDGET + 512u;
}

static size_t module_emit_cap(const jello_bc_module* m) {
  size_t cap = 4096u;
  for(uint32_t fi = 0; fi < m->nfuncs; fi++) cap += function_emit_budget(&m->funcs[fi]);
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
}

_Atomic uint32_t jello_jit_module_prepare_count;

static int prepare(const jello_bc_module* m, jello_jit_module* out, int eager) {
  jello_jit_module_prepare_count++;
  memset(out, 0, sizeof(*out));
  if(!m || m->nfuncs == 0) return 0;

  out->nfuncs = m->nfuncs;
  out->m = m;
  out->funcs = (jello_jit_func_code*)calloc(m->nfuncs, sizeof(jello_jit_func_code));
  out->blob = jit_alloc_exec_buf(module_emit_cap(m));
  if(!out->funcs || !out->blob.mem) {
    jello_jit_module_free(out);
    return 0;
  }

  /* Every function starts at the lazy stub; compiling one points its entry at its body, or at
   * the bridge when it declines. The entry past the functions' is the bridge, for a call nested
   * too deep for the native stack. */
  out->entries = (const void**)calloc((size_t)m->nfuncs + 1u, sizeof(void*));
  if(!out->entries ||
     !jello_jit_emit_mixed_stubs(&out->blob, &out->enter_off, &out->bridge_off, &out->lazy_off)) {
    jello_jit_module_free(out);
    return 0;
  }
  for(uint32_t fi = 0; fi < m->nfuncs; fi++) out->entries[fi] = out->blob.mem + out->lazy_off;
  out->entries[m->nfuncs] = out->blob.mem + out->bridge_off;
  if(eager) {
    for(uint32_t fi = 0; fi < m->nfuncs; fi++) {
      if(!jit_compile_function_now(out, fi) && out->funcs[fi].decline_reason == JIT_DECLINE_OOM) {
        jello_jit_module_free(out);
        return 0;
      }
    }
    if(out->ncompiled == 0) {
      jello_jit_module_free(out);
      return 0;
    }
  }

  if(!jit_finalize_exec_buf(&out->blob)) {
    jello_jit_module_free(out);
    return 0;
  }
  out->prepared = 1;
  return 1;
}

int jello_jit_module_prepare(const jello_bc_module* m, jello_jit_module* out) {
  return prepare(m, out, 1);
}

int jello_jit_module_prepare_lazy(const jello_bc_module* m, jello_jit_module* out) {
  return prepare(m, out, 0);
}

size_t jit_append_window_bytes;

static int emit_appending(jello_jit_module* jm, uint32_t fi, size_t bound) {
  if(!jit_exec_buf_begin_append(&jm->blob, bound)) abort();
  int ok = jello_jit_emit_function_into(jm->m, fi, &jm->blob, &jm->funcs[fi], jm->entries);
  if(!jit_exec_buf_end_append(&jm->blob)) abort();
  return ok;
}

// Out of line so the interpreter's entry path, run on every crossing, keeps a small C frame.
__attribute__((noinline)) int jit_compile_function_now(jello_jit_module* jm, uint32_t fi) {
  jello_jit_func_code* fc = &jm->funcs[fi];
  if(fc->tried) return fc->compiled;
  int ok;
  if(!jm->blob.finalized) {
    ok = jello_jit_emit_function_into(jm->m, fi, &jm->blob, fc, jm->entries);
  } else {
    ok = emit_appending(jm, fi,
                        jit_append_window_bytes ? jit_append_window_bytes
                                                : 2u * function_emit_budget(&jm->m->funcs[fi]));
    if(!ok && fc->decline_reason == JIT_DECLINE_EMIT) ok = emit_appending(jm, fi, SIZE_MAX);
  }
  fc->tried = 1;
  if(ok) jm->ncompiled++;
  jm->entries[fi] = jm->blob.mem + (ok ? fc->blob_off + fc->body_off : jm->bridge_off);
  return ok;
}

/** jit_compile_function_now for `vm`, whose module's code other processes may be running: the
 * world stops while the buffer is writable, as a thread running code on the page an append opens
 * would fault, and while the entry table changes. */
static int compile_shared(jello_vm* vm, jello_jit_module* jm, uint32_t fi) {
  if(!vm->proc) return jit_compile_function_now(jm, fi);
  while(!vm_heap_stop_the_world(vm)) {
    if(jm->funcs[fi].tried) return jm->funcs[fi].compiled;
  }
  int ok = jit_compile_function_now(jm, fi);
  vm_heap_resume_the_world(vm);
  return ok;
}

/** Counts a call to `fc`, which processes on other threads may count at once; whether it has
 * reached JIT_COMPILE_AFTER_CALLS. */
static int count_call(jello_jit_func_code* fc) {
  return __atomic_add_fetch(&fc->calls, 1, __ATOMIC_RELAXED) >= JIT_COMPILE_AFTER_CALLS;
}

const void* jit_lazy_target(jello_vm* vm) {
  jello_jit_module* jm = vm->active_jit;
  uint32_t fi = vm->call_frames[vm->call_frames_len - 1u].func_index;
  jello_jit_func_code* fc = &jm->funcs[fi];
  if(!fc->tried && count_call(fc)) compile_shared(vm, jm, fi);
  return jm->blob.mem + (fc->compiled ? fc->blob_off + fc->body_off : jm->bridge_off);
}

void jello_jit_module_free(jello_jit_module* jm) {
  if(!jm) return;
  if(jm->funcs) {
    for(uint32_t i = 0; i < jm->nfuncs; i++) {
      free(jm->funcs[i].loop_anchor_pcs);
      free(jm->funcs[i].loop_anchor_offs);
    }
    free(jm->funcs);
  }
  jm->funcs = NULL;
  jm->nfuncs = 0;
  jm->prepared = 0;
  free(jm->entries);
  jm->entries = NULL;
  jm->ncompiled = 0;
  jit_free_exec_buf(&jm->blob);
}

jello_jit_module* jit_module_create(const jello_bc_module* m, int eager) {
  jello_jit_module* jm = (jello_jit_module*)calloc(1, sizeof(jello_jit_module));
  if(!jm) return NULL;
  if(!prepare(m, jm, eager)) {
    free(jm);
    return NULL;
  }
  return jm;
}

int jello_jit_available(void) { return 1; }

jello_jit_module* jello_jit_module_create(const jello_bc_module* m) {
  return jit_module_create(m, 0);
}

void jello_jit_module_destroy(jello_jit_module* jm) {
  if(!jm) return;
  jello_jit_module_free(jm);
  free(jm);
}

uint32_t jit_try_enter_count;

int jello_jit_try_enter(jello_vm* vm, jello_jit_module* jm, jello_numeric_result* out,
                        jello_exec_status* out_st) {
  if(!jm || !jm->prepared || !vm || !out || !out_st) return 0;
  if(vm->call_frames_len == 0) return 0;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  if(fr->pc != 0 || fr->has_caller != 0) return 0;
  uint32_t fi = fr->func_index;
  if(fi >= jm->nfuncs) return 0;
  if(!jm->funcs[fi].compiled && !compile_shared(vm, jm, fi)) return 0;

  jello_jit_entry_fn entry = (jello_jit_entry_fn)(uintptr_t)(jm->blob.mem + jm->funcs[fi].blob_off +
                                                             jm->funcs[fi].entry_off);
  /* Processes on other threads count too. */
  __atomic_add_fetch(&jit_try_enter_count, 1, __ATOMIC_RELAXED);
  *out_st = entry(vm, fr->rf.mem, out);
  return 1;
}

void jit_native_stack_here(jello_vm* vm) {
  vm->jit_stack_floor = UINTPTR_MAX;
  vm->native_stack_limit = 0;
  uintptr_t low;
  size_t size;
#if defined(__APPLE__)
  pthread_t self = pthread_self();
  size = pthread_get_stacksize_np(self);
  low = (uintptr_t)pthread_get_stackaddr_np(self) - size;
#elif defined(__linux__)
  pthread_attr_t attr;
  void* addr = NULL;
  if(pthread_getattr_np(pthread_self(), &attr) != 0) return;
  int got = pthread_attr_getstack(&attr, &addr, &size);
  pthread_attr_destroy(&attr);
  if(got != 0) return;
  low = (uintptr_t)addr;
#elif defined(_WIN32)
  ULONG_PTR lowest, highest;
  GetCurrentThreadStackLimits(&lowest, &highest);
  low = (uintptr_t)lowest;
  size = (size_t)(highest - lowest);
#else
#error "no native stack bounds for this system"
#endif
  size_t reserve = size / 4u < 64u * 1024u ? 64u * 1024u : size / 4u;
  vm->jit_stack_floor = low + reserve;
  vm->native_stack_limit = low + JIT_NATIVE_RESERVE;
}

/** Runs `fc`'s body on the topmost frame, whose registers are `frame_base`, through the enter
 * stub. Kept out of jello_jit_enter_pushed_frame, whose checks usually decline: on Windows x64 a
 * call to JIT code saves the registers the system's convention keeps and JIT code's does not,
 * and only an entry should pay for that. */
static __attribute__((noinline)) void enter_body(jello_vm* vm, const jello_jit_module* jm,
                                                 uint8_t* frame_base,
                                                 const jello_jit_func_code* fc) {
  jello_jit_enter_fn enter = (jello_jit_enter_fn)(uintptr_t)(jm->blob.mem + jm->enter_off);
  enter(vm, frame_base, NULL, jm->blob.mem + fc->blob_off + fc->body_off);
}

int jello_jit_enter_pushed_frame(jello_vm* vm) {
  jello_jit_module* jm = vm->active_jit;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  uint32_t fi = fr->func_index;
  if(fi >= jm->nfuncs) return 0;
  if((uintptr_t)__builtin_frame_address(0) < vm->jit_stack_floor) return 0;
  jello_jit_func_code* fc = &jm->funcs[fi];
  if(!fc->compiled) {
    if(fc->tried || !count_call(fc) || !compile_shared(vm, jm, fi)) return 0;
  }
  vm->jit_enters++;
  enter_body(vm, jm, fr->rf.mem, fc);
  return vm->trap_code == JELLO_TRAP_NONE ? 1 : 2;
}
