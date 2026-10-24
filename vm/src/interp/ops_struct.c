// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for Struct ops; fields sit at fixed offsets set by the struct signature. */

#include <jello/internal.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

op_result op_struct_new(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_type_id tid = fr->f->reg_types[ins->a];
  uint32_t struct_sig_id = ctx->m->types[tid].as.strct.struct_sig_id;
  jello_struct* s = jello_struct_new(ctx->vm, ctx->m, struct_sig_id);
  vm_store_ptr(&fr->rf, ins->a, s);
  return OP_CONTINUE;
}

op_result op_struct_get(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_struct* s = (jello_struct*)vm_load_ptr(&fr->rf, ins->b);
  if(!s) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  const jello_struct_sig* sig = &ctx->m->struct_sigs[s->h.type_id];
  uint32_t off = sig->field_offset[ins->imm];
  jello_type_kind fk = ctx->m->types[sig->field_type[ins->imm]].kind;
  memcpy(vm_reg_ptr(&fr->rf, ins->a), s->fields + off, jello_slot_size(fk));
  return OP_CONTINUE;
}

op_result op_struct_set(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_struct* s = (jello_struct*)vm_load_ptr(&fr->rf, ins->b);
  if(!s) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  const jello_struct_sig* sig = &ctx->m->struct_sigs[s->h.type_id];
  uint32_t off = sig->field_offset[ins->imm];
  jello_type_kind fk = ctx->m->types[sig->field_type[ins->imm]].kind;
  memcpy(s->fields + off, vm_reg_ptr(&fr->rf, ins->a), jello_slot_size(fk));
  return OP_CONTINUE;
}
