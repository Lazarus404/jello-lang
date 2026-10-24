// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for Array ops; elements are stored at the slot size of their Array type's
 * element type (`as.unary.elem`). */

#include <jello/internal.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

static uint32_t array_elem_kind_and_size(exec_ctx* ctx, const jello_array* a,
                                         jello_type_kind* out_kind) {
  jello_type_id elem_tid = ctx->m->types[a->h.type_id].as.unary.elem;
  jello_type_kind ek = ctx->m->types[elem_tid].kind;
  if(out_kind) *out_kind = ek;
  return (uint32_t)jello_slot_size(ek);
}

op_result op_array_new(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_type_id tid = fr->f->reg_types[ins->a];
  uint32_t length = vm_load_u32(&fr->rf, ins->b);
  /* A length is an I32; a negative one reads as a huge unsigned value. */
  if(length > (uint32_t)INT32_MAX) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_array* arr = jello_array_new(ctx->vm, ctx->m, tid, length);
  vm_store_ptr(&fr->rf, ins->a, arr);
  return OP_CONTINUE;
}

op_result op_array_len(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_array* arr = (jello_array*)vm_load_ptr(&fr->rf, ins->b);
  if(!arr) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, arr->length);
  return OP_CONTINUE;
}

op_result op_array_get(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_array* arr = (jello_array*)vm_load_ptr(&fr->rf, ins->b);
  if(!arr) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= arr->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t elem_size = array_elem_kind_and_size(ctx, arr, NULL);
  memcpy(vm_reg_ptr(&fr->rf, ins->a), (const uint8_t*)arr->data + (size_t)idx * elem_size,
         elem_size);
  return OP_CONTINUE;
}

op_result op_array_set(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_array* arr = (jello_array*)vm_load_ptr(&fr->rf, ins->b);
  if(!arr) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= arr->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t elem_size = array_elem_kind_and_size(ctx, arr, NULL);
  memcpy((uint8_t*)arr->data + (size_t)idx * elem_size, vm_reg_ptr(&fr->rf, ins->a), elem_size);
  return OP_CONTINUE;
}

op_result op_array_resize(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_array* arr = (jello_array*)vm_load_ptr(&fr->rf, ins->b);
  if(!arr) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t new_len = vm_load_u32(&fr->rf, ins->c);
  if(new_len > (uint32_t)INT32_MAX) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_array_resize(ctx->vm, ctx->m, arr, new_len);
  return OP_CONTINUE;
}

op_result op_array_concat2(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_array* b = (jello_array*)vm_load_ptr(&fr->rf, ins->b);
  jello_array* c = (jello_array*)vm_load_ptr(&fr->rf, ins->c);
  if(!b || !c) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint64_t total = (uint64_t)b->length + (uint64_t)c->length;
  if(total > (uint64_t)INT32_MAX) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_array* dst = jello_array_new(ctx->vm, ctx->m, b->h.type_id, (uint32_t)total);
  uint32_t elem_size = array_elem_kind_and_size(ctx, dst, NULL);
  memcpy(dst->data, b->data, (size_t)b->length * elem_size);
  memcpy((uint8_t*)dst->data + (size_t)b->length * elem_size, c->data,
         (size_t)c->length * elem_size);
  vm_store_ptr(&fr->rf, ins->a, dst);
  return OP_CONTINUE;
}
