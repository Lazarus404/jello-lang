// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The interpreter's frame stack; call frames whose registers are bump-allocated slices of one
 * VM-owned stack, freed in LIFO order, and a per-function cache of register layouts. */

#include <jello/internal.h>
#include <jello/internal/platform.h>
#include <stdlib.h>
#include <string.h>

/** The bytes of frame stack a VM first commits, on its first push, where committing costs. */
#define VM_FIRST_FRAME_STACK_BYTES (16u * 1024u)

/** Makes `seg` current, its place in the VM's frame_stack fields. */
static void load_segment(jello_vm* vm, frame_segment* seg) {
  vm->segment = seg;
  vm->frame_stack = seg->mem;
  vm->frame_stack_top = seg->top;
  vm->frame_stack_limit = seg->limit;
  vm->frame_stack_max = seg->bytes;
}

/** Keeps the current segment's place in it, for loading again. */
static void save_segment(jello_vm* vm) {
  vm->segment->top = vm->frame_stack_top;
  vm->segment->limit = vm->frame_stack_limit;
}

void vm_frame_stack_attach(jello_vm* vm, uint8_t* mem, uint32_t bytes, uint32_t call_frames) {
  vm->first_segment =
      (frame_segment){.mem = mem, .bytes = bytes, .limit = JELLO_PAGES_COMMIT_COSTS ? 0 : bytes};
  load_segment(vm, &vm->first_segment);
  vm->call_frames_limit = call_frames;
}

/** Commits the frame stack to at least `top` bytes, twice what it had at least; returns 0 when
 * `top` is past frame_stack_max or committing fails. */
static int grow_frame_stack(jello_vm* vm, uint64_t top) {
  if(top > vm->frame_stack_max) return 0;
  uint64_t want = (uint64_t)vm->frame_stack_limit * 2u;
  if(want < top) want = top;
  if(want < VM_FIRST_FRAME_STACK_BYTES) want = VM_FIRST_FRAME_STACK_BYTES;
  /* To the end of a page, so that all that is committed is used. */
  uint64_t end = jello_page_round_up((uint64_t)(uintptr_t)vm->frame_stack + want);
  uint64_t limit = end - (uint64_t)(uintptr_t)vm->frame_stack;
  if(limit > vm->frame_stack_max) limit = vm->frame_stack_max;
  uint32_t had = vm->frame_stack_limit;
  if(!jello_pages_commit(vm->frame_stack + had, (size_t)(limit - had))) return 0;
  vm->frame_stack_limit = (uint32_t)limit;
  return 1;
}

jello_type_kind vm_reg_kind(const jello_bc_module* m, const jello_bc_function* f, uint32_t r) {
  return m->types[f->reg_types[r]].kind;
}

int vm_func_has_pointer(const jello_bc_module* m, const jello_bc_function* f) {
  for(uint32_t r = 0; r < f->nregs; r++) {
    if(vm_kind_needs_gc_scan(vm_reg_kind(m, f, r))) return 1;
  }
  return 0;
}

static frame_layout* find_layout(jello_vm* vm, const jello_bc_function* f) {
  for(uint32_t i = 0; i < vm->frame_layouts_len; i++) {
    if(vm->frame_layouts[i].f == f) return &vm->frame_layouts[i];
  }
  return NULL;
}

void vm_share_layouts(jello_vm* vm, const jello_vm* from) {
  vm_free_layouts(vm);
  vm->frame_layouts = from->frame_layouts;
  vm->frame_layouts_len = from->frame_layouts_len;
  vm->frame_layouts_cap = from->frame_layouts_len;
  vm->jit_layout_by_func = from->jit_layout_by_func;
  vm->jit_layout_by_func_len = from->jit_layout_by_func_len;
  vm->layouts_shared = 1;
}

void vm_free_layouts(jello_vm* vm) {
  if(!vm->layouts_shared) {
    for(uint32_t i = 0; i < vm->frame_layouts_len; i++) {
      free(vm->frame_layouts[i].kind);
      free(vm->frame_layouts[i].wide);
      free(vm->frame_layouts[i].init);
    }
    free(vm->frame_layouts);
    free(vm->jit_layout_by_func);
  }
  vm->layouts_shared = 0;
  vm->frame_layouts = NULL;
  vm->frame_layouts_len = 0;
  vm->frame_layouts_cap = 0;
  vm->jit_layout_by_func = NULL;
  vm->jit_layout_by_func_len = 0;
}

static frame_layout* build_layout(jello_vm* vm, const jello_bc_module* m,
                                  const jello_bc_function* f) {
  if(vm->layouts_shared) abort(); /* Shared layouts are complete, and another VM's to grow. */
  uint8_t* kind = f->nregs ? (uint8_t*)malloc((size_t)f->nregs * sizeof(uint8_t)) : NULL;
  uint8_t* wide = f->nregs ? (uint8_t*)malloc((size_t)f->nregs * sizeof(uint8_t)) : NULL;
  for(uint32_t r = 0; r < f->nregs; r++) {
    jello_type_kind k = vm_reg_kind(m, f, r);
    kind[r] = (uint8_t)k;
    wide[r] = (uint8_t)(jello_slot_size(k) == 8u);
  }

  if(vm->frame_layouts_len == vm->frame_layouts_cap) {
    /* jit_layout_by_func points into the array, so follow it when it moves;
     * hold each entry as an index (+1, 0 = none) across the realloc. */
    for(uint32_t i = 0; i < vm->jit_layout_by_func_len; i++) {
      frame_layout* p = vm->jit_layout_by_func[i];
      vm->jit_layout_by_func[i] =
          (frame_layout*)(uintptr_t)(p ? (size_t)(p - vm->frame_layouts) + 1u : 0u);
    }
    vm->frame_layouts_cap = vm->frame_layouts_cap ? vm->frame_layouts_cap * 2u : 8u;
    vm->frame_layouts = (frame_layout*)realloc(vm->frame_layouts, (size_t)vm->frame_layouts_cap *
                                                                      sizeof(frame_layout));
    for(uint32_t i = 0; i < vm->jit_layout_by_func_len; i++) {
      size_t at = (size_t)(uintptr_t)vm->jit_layout_by_func[i];
      vm->jit_layout_by_func[i] = at ? vm->frame_layouts + (at - 1u) : NULL;
    }
  }
  frame_layout* fl = &vm->frame_layouts[vm->frame_layouts_len++];
  fl->f = f;
  fl->kind = kind;
  fl->wide = wide;
  fl->total = f->nregs * REG_SLOT_BYTES;
  fl->has_pointer = vm_func_has_pointer(m, f);
  fl->init = NULL;
  if(fl->has_pointer && fl->total) {
    fl->init = (uint8_t*)calloc(fl->total, 1);
    if(!fl->init) abort();
    reg_frame rf = {fl->init, kind, wide, fl->total};
    for(uint32_t r = 0; r < f->nregs; r++) {
      if(vm_reg_kind_fast(&rf, r) == JELLO_T_DYNAMIC) vm_store_val(&rf, r, jello_make_null());
    }
  }
  return fl;
}

static frame_layout* get_layout(jello_vm* vm, const jello_bc_module* m,
                                const jello_bc_function* f) {
  frame_layout* fl = find_layout(vm, f);
  return fl ? fl : build_layout(vm, m, f);
}

void vm_warm_jit_layouts(jello_vm* vm, const jello_bc_module* m) {
  free(vm->jit_layout_by_func);
  vm->jit_layout_by_func = NULL;
  vm->jit_layout_by_func_len = 0;
  if(!m || m->nfuncs == 0) return;
  vm->jit_layout_by_func = (frame_layout**)calloc(m->nfuncs, sizeof(frame_layout*));
  if(!vm->jit_layout_by_func) return;
  vm->jit_layout_by_func_len = m->nfuncs;
  for(uint32_t fi = 0; fi < m->nfuncs; fi++) (void)get_layout(vm, m, &m->funcs[fi]);
  for(uint32_t fi = 0; fi < m->nfuncs; fi++)
    vm->jit_layout_by_func[fi] = find_layout(vm, &m->funcs[fi]);
}

/** Zeroes call frame slots [`from`, `to`). Slots above call_frames_len stay clean; the JIT's
 * inline frame push sets only the fields it uses, so it relies on vm_pop_frame and this to
 * leave spill_slots NULL and pop_work 0. */
static void clear_frames(call_frame* frames, uint32_t from, uint32_t to) {
  memset(frames + from, 0, (size_t)(to - from) * sizeof(call_frame));
}

/** Ensures a free call_frame slot; returns 0 when allocation fails. */
static int grow_call_frames(jello_vm* vm) {
  if(vm->call_frames_len < vm->call_frames_cap) return 1;
  uint32_t new_cap = vm->call_frames_cap ? vm->call_frames_cap * 2u : VM_FIRST_CALL_FRAMES;
  call_frame* p = (call_frame*)realloc(vm->call_frames, (size_t)new_cap * sizeof(call_frame));
  if(!p) return 0;
  clear_frames(p, vm->call_frames_cap, new_cap);
  vm->call_frames = p;
  vm->call_frames_cap = new_cap;
  return 1;
}

/** Unmaps the segments from `seg` up, which no frame is in. */
static void free_segments(jello_vm* vm, frame_segment* seg) {
  while(seg) {
    frame_segment* above = seg->above;
    vm->segment_bytes -= seg->bytes;
    jello_pages_unmap(seg->mem, seg->bytes);
    free(seg);
    seg = above;
  }
}

/** The segment above the current one, of at least `total` bytes: the one left there before if it
 * is big enough, else one mapped twice the size of the current at least; NULL when none can be
 * had. */
static frame_segment* segment_above(jello_vm* vm, uint32_t total) {
  frame_segment* below = vm->segment;
  if(below->above && below->above->bytes >= total) return below->above;
  free_segments(vm, below->above);
  below->above = NULL;
  uint64_t page = (uint64_t)jello_page_size();
  uint64_t room = (VM_MAX_SEGMENT_BYTES - vm->segment_bytes) & ~(page - 1u);
  uint64_t bytes = jello_page_round_up(
      (uint64_t)vm->frame_stack_max * 2u > total ? (uint64_t)vm->frame_stack_max * 2u : total);
  if(bytes > room) bytes = room;
  if(bytes < total) return NULL;
  frame_segment* seg = (frame_segment*)calloc(1, sizeof(frame_segment));
  if(!seg) return NULL;
  seg->mem = (uint8_t*)jello_pages_reserve((size_t)bytes);
  if(!seg->mem) {
    free(seg);
    return NULL;
  }
  seg->bytes = (uint32_t)bytes;
  seg->limit = JELLO_PAGES_COMMIT_COSTS ? 0 : seg->bytes;
  seg->below = below;
  below->above = seg;
  vm->segment_bytes += seg->bytes;
  return seg;
}

int jit_try_grow_push(jello_vm* vm, uint64_t top) {
  if(!grow_call_frames(vm)) return 0;
  if(top <= vm->frame_stack_limit) return 1;
  if(top <= vm->frame_stack_max) return grow_frame_stack(vm, top);
  /* The frame starts a segment above, whose pop returns to this one. */
  uint32_t total = (uint32_t)(top - vm->frame_stack_top);
  frame_segment* seg = segment_above(vm, total);
  if(!seg) return 0;
  save_segment(vm);
  load_segment(vm, seg);
  if(total > vm->frame_stack_limit && !grow_frame_stack(vm, total)) {
    load_segment(vm, seg->below);
    return 0;
  }
  vm->call_frames[vm->call_frames_len].starts_segment = 1;
  return 1;
}

void vm_frame_segments_free(jello_vm* vm) {
  free_segments(vm, vm->first_segment.above);
  vm->first_segment.above = NULL;
}

int jit_call_push_numeric(jello_vm* vm, uint32_t callee_fi, uint32_t caller_dst, uint32_t arg_first,
                          uint32_t nargs) {
  if(!vm || !vm->running_module) return 0;
  return vm_push_frame(vm, vm->running_module, callee_fi, caller_dst, arg_first, nargs, 1);
}

uint8_t* jit_current_frame_mem(jello_vm* vm) {
  if(!vm || vm->call_frames_len == 0) return vm ? vm->frame_stack : NULL;
  return vm->call_frames[vm->call_frames_len - 1u].rf.mem;
}

/** Pushes a frame for function `func_index` but neither copies arguments nor counts it in
 * call_frames_len; vm_push_frame and vm_push_frame_raw do both. May realloc vm->call_frames.
 * Returns NULL, with trap_code JELLO_TRAP_STACK_OVERFLOW, when the frame stack or the call
 * frame limit is exhausted or an allocation fails. */
static call_frame* push_frame_shell(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                                    uint32_t caller_dst, uint8_t has_caller) {
  const jello_bc_function* f = &m->funcs[func_index];
  /* The per-function index, when warmed for this module, saves the linear find_layout scan. */
  frame_layout* fl =
      func_index < vm->jit_layout_by_func_len ? vm->jit_layout_by_func[func_index] : NULL;
  if(!fl || fl->f != f) fl = get_layout(vm, m, f);

  if(vm->call_frames_len >= vm->call_frames_limit ||
     !jit_try_grow_push(vm, (uint64_t)vm->frame_stack_top + fl->total)) {
    vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
    return NULL;
  }

  uint8_t* mem = vm->frame_stack + vm->frame_stack_top;
  vm->frame_stack_top += fl->total;

  call_frame* fr = &vm->call_frames[vm->call_frames_len];
  fr->f = f;
  fr->func_index = func_index;
  fr->pc = 0;
  fr->rf.mem = mem;
  fr->rf.kind = fl->kind;
  fr->rf.wide = fl->wide;
  fr->rf.size = fl->total;
  fr->has_caller = has_caller;
  fr->has_pointer = (uint8_t)fl->has_pointer;
  fr->caller_dst = caller_dst;
  fr->spill_slots = NULL;
  fr->spill_cap = 0;

  /* A frame with any reference or Dynamic register starts as the layout's init image, so an
   * unwritten reference is NULL (a NULL_DEREF trap, not stale data from an earlier call) and
   * an unwritten Dynamic register null. Purely numeric frames skip this. */
  if(fl->has_pointer) memcpy(mem, fl->init, fl->total);

  return fr;
}

int vm_push_frame(jello_vm* vm, const jello_bc_module* m, uint32_t func_index, uint32_t caller_dst,
                  uint32_t arg_first, uint32_t arg_count, uint8_t has_caller) {
  /* Find the caller only after push_frame_shell, which may realloc vm->call_frames. It is still
   * the last counted frame, since call_frames_len grows only at the end. */
  call_frame* fr = push_frame_shell(vm, m, func_index, caller_dst, has_caller);
  if(!fr) return 0;
  call_frame* caller = has_caller ? &vm->call_frames[vm->call_frames_len - 1u] : NULL;

  /* Validation makes each argument register's kind match the callee's, so the callee's widths
   * fit both sides. */
  for(uint32_t i = 0; i < arg_count; i++) {
    if(vm_reg_is_wide(&fr->rf, i)) {
      vm_store_i64(&fr->rf, i, vm_load_i64(&caller->rf, arg_first + i));
    } else {
      vm_store_u32(&fr->rf, i, vm_load_u32(&caller->rf, arg_first + i));
    }
  }

  vm->call_frames_len++;
  return 1;
}

int vm_push_frame_raw(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                      uint32_t caller_dst, uint8_t has_caller, const uint64_t* argbuf,
                      uint32_t arg_count, const uint64_t* capbuf, uint32_t ncaps) {
  call_frame* fr = push_frame_shell(vm, m, func_index, caller_dst, has_caller);
  if(!fr) return 0;

  for(uint32_t i = 0; i < arg_count; i++) {
    if(vm_reg_is_wide(&fr->rf, i)) vm_store_i64(&fr->rf, i, (int64_t)argbuf[i]);
    else vm_store_u32(&fr->rf, i, (uint32_t)argbuf[i]);
  }
  uint32_t cap_start = fr->f->cap_start;
  for(uint32_t i = 0; i < ncaps; i++) {
    if(vm_reg_is_wide(&fr->rf, cap_start + i))
      vm_store_i64(&fr->rf, cap_start + i, (int64_t)capbuf[i]);
    else vm_store_u32(&fr->rf, cap_start + i, (uint32_t)capbuf[i]);
  }

  vm->call_frames_len++;
  return 1;
}

const jello_function* vm_callee_function(jello_vm* vm, jello_value v) {
  const jello_function* fn = jello_is_ptr(v) ? (const jello_function*)jello_as_ptr(v) : NULL;
  if(!fn) {
    vm->trap_code =
        (jello_is_null(v) || jello_is_ptr(v)) ? JELLO_TRAP_NULL_DEREF : JELLO_TRAP_TYPE_MISMATCH;
    return NULL;
  }
  if(fn->h.kind != (uint8_t)JELLO_OBJ_FUNCTION) {
    vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return NULL;
  }
  return fn;
}

const jello_function* vm_push_callr_frame(jello_vm* vm, const jello_bc_module* m, jello_value v,
                                          uint32_t caller_dst, uint32_t arg_first,
                                          uint32_t arg_count) {
  const jello_function* fn = vm_callee_function(vm, v);
  if(!fn || !vm_push_frame(vm, m, fn->func_index, caller_dst, arg_first, arg_count, 1)) return NULL;
  if(fn->ncaps == 0) return fn;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  uint32_t cap_start = fr->f->cap_start;
  for(uint32_t i = 0; i < fn->ncaps; i++) {
    if(vm_reg_is_wide(&fr->rf, cap_start + i))
      vm_store_i64(&fr->rf, cap_start + i, (int64_t)fn->captures[i]);
    else vm_store_u32(&fr->rf, cap_start + i, (uint32_t)fn->captures[i]);
  }
  return fn;
}

/** Pops the top frame. JIT code (emit_ret_insn in emit_arm64.c and emit_x64.c) pops a frame
 * without pop_work or spill slots inline, adjusting only frame_stack_top and call_frames_len, so
 * other work added here must go there too or into that test. */
void vm_pop_frame(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  free(fr->spill_slots);
  fr->spill_slots = NULL;
  fr->spill_cap = 0;
  vm->frame_stack_top -= fr->rf.size;
  if(fr->pop_work) {
    vm->live_try_scopes -= fr->try_stack_len;
    if(fr->starts_segment) {
      save_segment(vm);
      load_segment(vm, vm->segment->below);
    }
    fr->pop_work = 0;
  }
  vm->call_frames_len--;
}

void vm_trace_frames(jello_vm* vm, uint32_t stop) {
  jello_trap_trace* t = &vm->trap_trace;
  uint32_t site = t->site;
  t->site = 0;
  if(vm->trap_code == JELLO_TRAP_INTERNAL_UNWIND || t->full) return;
  for(uint32_t i = vm->call_frames_len; i > stop; i--) {
    const call_frame* fr = &vm->call_frames[i - 1u];
    // Both are 1 + the call's pc: a caller's pc has moved past its call.
    uint32_t after = i == vm->call_frames_len ? site : fr->pc;
    if(after == 0) continue;
    uint32_t last = t->len - 1u;
    if(t->len && t->func_index[last] == fr->func_index && t->pc[last] == after - 1u) {
      t->repeated[last] = 1;
    } else if(t->len < JELLO_TRAP_CALLERS_MAX) {
      t->func_index[t->len] = fr->func_index;
      t->repeated[t->len] = 0;
      t->pc[t->len++] = after - 1u;
    } else {
      t->full = 1;
      return;
    }
  }
}

void vm_frame_unwind(jello_vm* vm) {
  /* A trap leaves its frames pushed; popping them frees their spill slots
   * and clears their try scopes, which the JIT's inline push relies on. */
  while(vm->call_frames_len > 0) vm_pop_frame(vm);
  vm->frame_stack_top = 0;
}

void vm_frame_reset(jello_vm* vm) {
  vm_frame_unwind(vm);
  vm_clear_trap(vm);
  vm_free_layouts(vm);
}
