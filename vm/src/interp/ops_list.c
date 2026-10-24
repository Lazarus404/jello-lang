// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for List ops, and jello_list_cons. A list is a chain of cons cells holding
 * boxed heads; the empty list is the null pointer. */

#include <jello/internal.h>

jello_list* jello_list_cons(jello_vm* vm, uint32_t type_id, jello_value head, jello_list* tail) {
  jello_list* n = (jello_list*)jello_gc_alloc(vm, sizeof(jello_list));
  n->h.kind = (uint8_t)JELLO_OBJ_LIST;
  n->h.type_id = type_id;
  n->head = head;
  n->tail = tail;
  return n;
}

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

op_result op_list_nil(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  vm_store_ptr(&fr->rf, ins->a, NULL);
  return OP_CONTINUE;
}

op_result op_list_cons(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  call_frame* fr = current_frame(ctx);
  const jello_bc_function* f = fr->f;

  jello_value head = vm_box_from_typed(vm, m, f, &fr->rf, ins->b);
  if(vm_reg_kind(m, f, ins->b) == JELLO_T_DYNAMIC) head = vm_clone_numbox(vm, head);
  /* A fresh box is in no register, and allocating the cell may collect. */
  if(jello_is_ptr(head)) jello_gc_push_root(vm, jello_as_ptr(head));
  jello_list* tail = (jello_list*)vm_load_ptr(&fr->rf, ins->c);
  uint32_t type_id = f->reg_types[ins->a];
  jello_list* node = jello_list_cons(vm, type_id, head, tail);
  if(jello_is_ptr(head)) jello_gc_pop_roots(vm, 1);
  vm_store_ptr(&fr->rf, ins->a, node);
  return OP_CONTINUE;
}

op_result op_list_head(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  call_frame* fr = current_frame(ctx);
  const jello_bc_function* f = fr->f;

  jello_list* node = (jello_list*)vm_load_ptr(&fr->rf, ins->b);
  if(!node) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  if(!vm_store_from_boxed(vm, m, f, &fr->rf, ins->a, node->head)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_list_tail(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);

  jello_list* node = (jello_list*)vm_load_ptr(&fr->rf, ins->b);
  if(!node) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  vm_store_ptr(&fr->rf, ins->a, node->tail);
  return OP_CONTINUE;
}

op_result op_list_is_nil(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = vm_load_ptr(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(p == NULL));
  return OP_CONTINUE;
}
