// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for GLOBAL_GET and GLOBAL_SET, on the running VM's globals; the validator
 * has checked index and type, so each is one slot copy. */

#include <jello/internal.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

static size_t global_size(const jello_bc_module* m, uint32_t index) {
  return jello_slot_size(m->types[m->global_types[index]].kind);
}

op_result op_global_get(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  memcpy(vm_reg_ptr(&fr->rf, ins->a), &ctx->vm->globals[ins->imm], global_size(ctx->m, ins->imm));
  return OP_CONTINUE;
}

op_result op_global_set(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  memcpy(&ctx->vm->globals[ins->imm], vm_reg_ptr(&fr->rf, ins->a), global_size(ctx->m, ins->imm));
  return OP_CONTINUE;
}
