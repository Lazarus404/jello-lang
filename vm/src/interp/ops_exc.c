// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for exception ops, and the try-scope helpers JIT code shares. Each frame
 * counts its own try scopes, which the VM keeps on one stack; THROW unwinds to the innermost open
 * scope on any frame. */

#include <jello/internal.h>

#include <stdlib.h>

int vm_try_push(jello_vm* vm, uint32_t catch_pc, uint32_t payload_reg) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  if(vm->live_try_scopes == vm->try_scopes_cap) {
    uint32_t cap = vm->try_scopes_cap ? vm->try_scopes_cap * 2u : JELLO_TRY_STACK_MAX;
    try_scope* grown = (try_scope*)realloc(vm->try_scopes, (size_t)cap * sizeof(try_scope));
    if(!grown) {
      vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
      return 0;
    }
    vm->try_scopes = grown;
    vm->try_scopes_cap = cap;
  }
  try_scope* ts = &vm->try_scopes[vm->live_try_scopes++];
  fr->try_stack_len++;
  ts->catch_pc = catch_pc;
  ts->payload_reg = payload_reg;
  return 1;
}

void vm_try_pop(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  if(fr->try_stack_len > 0) {
    fr->try_stack_len--;
    vm->live_try_scopes--;
  }
}

op_result op_try(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
  uint32_t catch_pc = (uint32_t)((int32_t)fr->pc + (int32_t)ins->imm);
  return vm_try_push(ctx->vm, catch_pc, ins->a) ? OP_CONTINUE : OP_TRAP;
}

op_result op_endtry(exec_ctx* ctx, const jello_insn* ins) {
  (void)ins;
  vm_try_pop(ctx->vm);
  return OP_CONTINUE;
}

int vm_throw_find(jello_vm* vm, jello_value payload) {
  if(vm->live_try_scopes == 0) return 0;
  for(uint32_t i = vm->call_frames_len; i-- > 0;) {
    call_frame* fr = &vm->call_frames[i];
    if(fr->try_stack_len == 0) continue;
    fr->try_stack_len--;
    try_scope ts = vm->try_scopes[--vm->live_try_scopes];
    vm_store_val(&fr->rf, ts.payload_reg, payload);
    fr->pc = ts.catch_pc;
    vm->unwind_len = i + 1u;
    return 1;
  }
  return 0;
}

int vm_catch_in_segment(exec_ctx* ctx) {
  jello_vm* vm = ctx->vm;
  if(vm->trap_code != JELLO_TRAP_INTERNAL_UNWIND || vm->unwind_len <= ctx->stop_len) return 0;
  while(vm->call_frames_len > vm->unwind_len) vm_pop_frame(vm);
  vm->trap_code = JELLO_TRAP_NONE;
  return 1;
}

/* A catching frame at or below ctx->stop_len belongs to JIT code or an outer loop; this loop
 * returns, and each activation in between pops itself as the unwind passes. A handler may sit
 * at or before its THROW, so a THROW is a possible loop back-edge and charges fuel. */
op_result op_throw(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  if(vm_poll_and_charge(vm) != 0) return OP_TRAP;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  jello_value payload = vm_load_val(&fr->rf, ins->a);
  if(!vm_throw_find(vm, payload)) {
    vm->trap_code = JELLO_TRAP_UNCAUGHT_THROW;
    vm->thrown = payload;
    return OP_TRAP;
  }
  vm->trap_code = JELLO_TRAP_INTERNAL_UNWIND;
  if(vm_catch_in_segment(ctx)) return OP_CONTINUE;
  return OP_TRAP_LOCATED;
}
