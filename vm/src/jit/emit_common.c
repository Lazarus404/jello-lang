// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Executable buffers with their append and patch windows, the trap setters, and the op policy
 * and helpers both JIT backends share. */

#include <jello/internal/jit_internal.h>
#include <jello/internal/platform.h>
#include <jello/internal/vm_internal.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif

/** Bytes to sync after writing `n`; `n` rounded up to whole pages, at most `cap`. */
static size_t jit_icache_span(size_t n, size_t cap) {
  size_t page = jello_page_size();
  if(page < 64) page = 64;
  size_t span = (n + page - 1u) & ~(page - 1u);
  if(span > cap) span = cap;
  return span;
}

/** Makes code just written at `mem` visible to instruction fetch, which on arm64 is not coherent
 * with data writes. Windows requires FlushInstructionCache of code that writes code on every
 * architecture; Darwin's sys_icache_invalidate includes the barriers; elsewhere
 * __builtin___clear_cache is followed by dsb and isb on arm64. */
static void jit_sync_icache(void* mem, size_t n) {
  if(!mem || n == 0) return;
#if defined(_WIN32)
  FlushInstructionCache(GetCurrentProcess(), mem, n);
#elif defined(__APPLE__) && defined(__aarch64__)
  sys_icache_invalidate(mem, n);
#else
  __builtin___clear_cache((char*)mem, (char*)mem + n);
#endif
#if defined(__aarch64__) && !defined(__APPLE__) && !defined(_WIN32)
  __asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
#endif
}

jit_exec_buf jit_alloc_exec_buf(size_t cap) {
  jit_exec_buf buf;
  buf.mem = NULL;
  buf.cap = cap;
  buf.len = 0;
  buf.finalized = 0;
  buf.overflowed = 0;
  buf.limit = cap;
  buf.append_from = 0;
  buf.append_end = 0;

  buf.mem = (uint8_t*)jello_pages_map(cap);
  return buf;
}

int jit_exec_buf_write(jit_exec_buf* buf, const void* bytes, size_t n) {
  if(!buf->mem || buf->finalized || buf->len + n > buf->limit) {
    buf->overflowed = 1;
    return 0;
  }
  memcpy(buf->mem + buf->len, bytes, n);
  buf->len += n;
  return 1;
}

int jit_finalize_exec_buf(jit_exec_buf* buf) {
  if(!buf->mem) return 0;
  if(!jello_pages_protect(buf->mem, buf->cap, JELLO_PAGES_RX)) return 0;
  jit_sync_icache(buf->mem, jit_icache_span(buf->len, buf->cap));
  buf->finalized = 1;
  return 1;
}

static size_t page_floor(size_t n) { return n & ~(jello_page_size() - 1u); }

int jit_exec_buf_begin_append(jit_exec_buf* buf, size_t bound) {
  if(!buf->mem || !buf->finalized || buf->len >= buf->cap) return 0;
  size_t from = page_floor(buf->len);
  size_t limit = buf->cap - buf->len > bound ? buf->len + bound : buf->cap;
  size_t end = page_floor(limit + jello_page_size() - 1u);
  if(end > buf->cap) end = buf->cap;
  if(!jello_pages_protect(buf->mem + from, end - from, JELLO_PAGES_RW)) return 0;
  buf->append_from = buf->len;
  buf->append_end = end;
  buf->limit = limit;
  buf->finalized = 0;
  return 1;
}

int jit_exec_buf_end_append(jit_exec_buf* buf) {
  size_t from = page_floor(buf->append_from);
  if(!jello_pages_protect(buf->mem + from, buf->append_end - from, JELLO_PAGES_RX)) return 0;
  if(buf->len > buf->append_from)
    jit_sync_icache(buf->mem + buf->append_from, buf->len - buf->append_from);
  buf->limit = buf->cap;
  buf->finalized = 1;
  return 1;
}

void jit_free_exec_buf(jit_exec_buf* buf) {
  if(!buf->mem) return;
#if defined(__APPLE__) && defined(__x86_64__)
  /* Rosetta keeps its translations across munmap, so a buffer mapped later
   * at the same address could run the freed code; taking execute away first
   * drops them. */
  int dropped = jello_pages_protect(buf->mem, buf->cap, JELLO_PAGES_RW);
  assert(dropped);
  (void)dropped;
#endif
  jello_pages_unmap(buf->mem, buf->cap);
  buf->mem = NULL;
}

void jit_trap_arithmetic(jello_vm* vm) { vm->trap_code = JELLO_TRAP_ARITHMETIC; }

void jit_trap_stack_overflow(jello_vm* vm) { vm->trap_code = JELLO_TRAP_STACK_OVERFLOW; }

void jit_trap_null_deref(jello_vm* vm) { vm->trap_code = JELLO_TRAP_NULL_DEREF; }

void jit_trap_bounds(jello_vm* vm) { vm->trap_code = JELLO_TRAP_BOUNDS; }

void jit_trap_type_mismatch(jello_vm* vm) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; }

void jit_trap_limit(jello_vm* vm) { vm->trap_code = JELLO_TRAP_LIMIT; }

/** The op policy both backends share, so they decline the same functions. An allowed op a
 * backend cannot emit fails in its emitter and declines as EMIT. */
static int jir_op_allowed(jello_jit_ir_op op, int allow_calls) {
  switch(op) {
    case JIR_NOP:
    case JIR_RET:
    case JIR_MOV:
    case JIR_JMP:
    case JIR_JMP_IF:
    case JIR_SAFEPOINT:
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32:
    case JIR_CONST_I64:
    case JIR_CONST_F16:
    case JIR_CONST_F32:
    case JIR_CONST_F64:
    case JIR_CONST_BOOL:
    case JIR_BIN_I32:
    case JIR_BIN_I64:
    case JIR_BIN_F16:
    case JIR_BIN_F32:
    case JIR_BIN_F64:
    case JIR_ADD_I32_IMM:
    case JIR_SUB_I32_IMM:
    case JIR_MUL_I32_IMM:
    case JIR_NEG_I32:
    case JIR_NEG_I64:
    case JIR_NEG_F32:
    case JIR_NEG_F64:
    case JIR_NOT_BOOL:
    case JIR_CMP_I32:
    case JIR_CMP_I64:
    case JIR_CMP_F32:
    case JIR_CMP_F64:
    case JIR_EQ_I32_IMM:
    case JIR_LT_I32_IMM:
    case JIR_CONV:
    case JIR_BYTES_NEW:
    case JIR_BYTES_LEN:
    case JIR_BYTES_GET_U8:
    case JIR_BYTES_SET_U8:
    case JIR_BYTES_EQ:
    case JIR_BYTES_CONCAT2:
    case JIR_BYTES_BITOP:
    case JIR_BYTES_READ:
    case JIR_BYTES_WRITE:
    case JIR_BYTES_BLIT:
    case JIR_BITS_WRITE:
    case JIR_BITS_READ:
    case JIR_BYTES_FREEZE:
    case JIR_STRUCT_NEW:
    case JIR_STRUCT_GET:
    case JIR_STRUCT_SET:
    case JIR_CONST_NULL:
    case JIR_CONST_ATOM:
    case JIR_TO_DYN:
    case JIR_FROM_DYN:
    case JIR_KINDOF:
    case JIR_PHYSEQ:
    case JIR_IS_NULL:
    case JIR_NULL_CHECK:
    case JIR_IS_ENUM:
    case JIR_OBJ_NEW:
    case JIR_OBJ_HAS_ATOM:
    case JIR_OBJ_GET_ATOM:
    case JIR_OBJ_SET_ATOM:
    case JIR_OBJ_GET:
    case JIR_OBJ_SET:
    case JIR_OBJ_HAS:
    case JIR_OBJ_REMOVE:
    case JIR_OBJ_CLEAR:
    case JIR_OBJ_COPY:
    case JIR_ARRAY_LEN:
    case JIR_ARRAY_GET:
    case JIR_ARRAY_SET:
    case JIR_GLOBAL_GET:
    case JIR_GLOBAL_SET:
    case JIR_ENUM_TAG:
    case JIR_ENUM_GET:
    case JIR_TRY:
    case JIR_ENDTRY:
    case JIR_THROW: return 1;
    case JIR_CALL_SELF:
    case JIR_CALL_DIRECT:
    case JIR_CALLR: return allow_calls;
    case JIR_SLOW:
    case JIR_CALL_NATIVE:
      return 1; // JIR_SLOW only when jello_jit_ir_slow_runs_handler allows its source op
    default: return 0;
  }
}

int jit_resolve_target_bc(uint32_t bc_pc, int32_t imm, uint32_t ninsns, uint32_t* out) {
  int64_t t = (int64_t)(int32_t)bc_pc + 1 + (int64_t)imm;
  if(t < 0 || t >= (int64_t)ninsns) return 0;
  *out = (uint32_t)t;
  return 1;
}

int jit_ir_shape_ok(const jello_jit_ir_function* ir, const jello_bc_module* m,
                    const jello_bc_function* f, uint32_t fi, jello_type_kind* out_ret_kind,
                    int allow_calls, uint32_t* bad_pc) {
  int saw_ret = 0;
  jello_type_kind ret_kind = JELLO_T_VOID;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    *bad_pc = ir->insns[i].bc_pc;
    if(!jir_op_allowed(op, allow_calls)) return 0;
    if(op == JIR_SLOW && !jello_jit_ir_slow_runs_handler((jello_op)f->insns[ir->insns[i].bc_pc].op))
      return 0;
    if(op == JIR_CALL_SELF || op == JIR_CALL_DIRECT) {
      uint32_t callee = (op == JIR_CALL_SELF) ? fi : ir->insns[i].imm;
      if(callee >= m->nfuncs) return 0;
    }
    if(op == JIR_JMP || op == JIR_JMP_IF || op == JIR_TRY) {
      uint32_t tgt = 0;
      if(!jit_resolve_target_bc(ir->insns[i].bc_pc, (int32_t)ir->insns[i].imm, f->ninsns, &tgt))
        return 0;
    }
    if(op == JIR_RET) {
      jello_type_kind k = vm_reg_kind(m, f, ir->insns[i].a);
      if(k == JELLO_T_F16) return 0;
      if(saw_ret && k != ret_kind) return 0;
      ret_kind = k;
      saw_ret = 1;
    }
  }
  *bad_pc = UINT32_MAX;
  *out_ret_kind = ret_kind;
  return 1;
}

int jit_scan_targets(const jello_jit_ir_function* ir, uint32_t ninsns, uint8_t* is_target,
                     uint8_t* is_loop_anchor, uint32_t* out_nloop_anchors, uint32_t** out_catch_pcs,
                     uint32_t* out_ncatch) {
  uint32_t nloop_anchors = 0;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    if(op != JIR_JMP && op != JIR_JMP_IF && op != JIR_TRY) continue;
    uint32_t tgt = 0; // in range; jit_ir_shape_ok checked it
    (void)jit_resolve_target_bc(ir->insns[i].bc_pc, (int32_t)ir->insns[i].imm, ninsns, &tgt);
    is_target[tgt] |= (uint8_t)(op == JIR_TRY ? 3u : 1u);
    if(op == JIR_TRY) continue;
    if(tgt <= ir->insns[i].bc_pc && !is_loop_anchor[tgt]) {
      is_loop_anchor[tgt] = 1;
      nloop_anchors++;
    }
  }
  *out_nloop_anchors = nloop_anchors;

  uint32_t ncatch = 0;
  for(uint32_t pc = 0; pc < ninsns; pc++) ncatch += (is_target[pc] >> 1) & 1u;
  uint32_t* catch_pcs = (uint32_t*)malloc(sizeof(uint32_t) * (ncatch ? ncatch : 1u));
  if(!catch_pcs) return 0;
  for(uint32_t pc = 0, w = 0; pc < ninsns; pc++) {
    if(is_target[pc] & 2u) catch_pcs[w++] = pc;
  }
  *out_catch_pcs = catch_pcs;
  *out_ncatch = ncatch;
  return 1;
}

int jit_list_push(void* items_ptr, size_t* count, size_t* cap, const void* item, size_t elem) {
  uint8_t* items;
  memcpy(&items, items_ptr, sizeof(items));
  if(*count == *cap) {
    size_t grown = *cap ? *cap * 2u : 16u;
    uint8_t* p = (uint8_t*)realloc(items, grown * elem);
    if(!p) return 0;
    items = p;
    *cap = grown;
    memcpy(items_ptr, &items, sizeof(items));
  }
  memcpy(items + *count * elem, item, elem);
  (*count)++;
  return 1;
}

int jit_decline(jello_jit_func_code* fc, jello_jit_decline why, const jello_bc_function* f,
                uint32_t pc) {
  fc->compiled = 0;
  fc->decline_reason = (uint8_t)why;
  fc->decline_pc = pc;
  fc->decline_op = (uint16_t)(pc < f->ninsns ? f->insns[pc].op : 0u);
  return 0;
}
