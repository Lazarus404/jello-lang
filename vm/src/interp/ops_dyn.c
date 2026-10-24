// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for Dynamic, null, atom and function-value ops, indirect and tail calls,
 * spill slots and SWITCH_KIND. The validator checks each FROM_DYN_* destination's kind, so one
 * generic unbox serves them all. */

#include <jello/internal.h>
#include <stdlib.h>
#include <stdio.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

op_result op_to_dyn(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->b);
  vm_store_val(&fr->rf, ins->a, v);
  return OP_CONTINUE;
}

static op_result from_dyn_generic(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_load_val(&fr->rf, ins->b);
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_from_dyn_i8(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_i16(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_i32(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_i64(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_f16(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_f32(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_f64(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_bool(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_atom(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}
op_result op_from_dyn_ptr(exec_ctx* ctx, const jello_insn* ins) {
  return from_dyn_generic(ctx, ins);
}

op_result op_kindof(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_load_val(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, vm_kindof_dynamic(v));
  return OP_CONTINUE;
}

op_result op_physeq(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value vb = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->b);
  jello_value vc = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->c);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(vb == vc));
  return OP_CONTINUE;
}

/* CONST_NULL stores null in a Dynamic register (the null tag) or in a
 * reference register (the null pointer). */
op_result op_const_null(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  if(vm_reg_kind_fast(&fr->rf, ins->a) == JELLO_T_DYNAMIC)
    vm_store_val(&fr->rf, ins->a, jello_make_null());
  else vm_store_ptr(&fr->rf, ins->a, NULL);
  return OP_CONTINUE;
}

/** Whether register `r`, a Dynamic or a reference, holds null. */
static int reg_is_null(call_frame* fr, uint32_t r) {
  if(vm_reg_kind_fast(&fr->rf, r) == JELLO_T_DYNAMIC) return jello_is_null(vm_load_val(&fr->rf, r));
  return vm_load_ptr(&fr->rf, r) == NULL;
}

op_result op_is_null(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)reg_is_null(fr, ins->b));
  return OP_CONTINUE;
}

op_result op_is_enum(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  vm_store_u32(&fr->rf, ins->a, vm_dynamic_is_enum(vm_load_val(&fr->rf, ins->b), ins->imm));
  return OP_CONTINUE;
}

/* NULL_CHECK traps NULL_DEREF when register a holds null; the checked
 * conversion of a nullable value to its present type. */
op_result op_null_check(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  if(reg_is_null(fr, ins->a)) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  return OP_CONTINUE;
}

op_result op_const_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  vm_store_u32(&fr->rf, ins->a, ins->imm);
  return OP_CONTINUE;
}

/* CONST_FUN is CLOSURE with no captures, so CALLR and TAILCALLR handle every function value
 * alike; but a function without captures has one value, so that it equals itself (PHYSEQ). The
 * first CONST_FUN makes it, immortal so that every process shares it, and a thread that loses the
 * race to publish it takes the winner's. A CONST_FUN that stores the function as another type,
 * which unboxing a Dynamic tells apart, makes a value of its own, as every CONST_FUN does in a
 * module built without the loader's table. */
op_result op_const_fun(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_type_id type = fr->f->reg_types[ins->a];
  _Atomic(jello_function*)* slot = ctx->m->const_funs ? &ctx->m->const_funs[ins->imm] : NULL;
  jello_function* fn = slot ? atomic_load_explicit(slot, memory_order_acquire) : NULL;
  if(slot && !fn) {
    jello_function* made = vm_function_immortal(ins->imm, type);
    if(atomic_compare_exchange_strong_explicit(slot, &fn, made, memory_order_acq_rel,
                                               memory_order_acquire)) {
      fn = made;
    } else {
      vm_immortal_free(made);
    }
  }
  if(!fn || fn->h.type_id != type) {
    fn = jello_function_new(ctx->vm, ins->imm, 0);
    fn->h.type_id = type;
  }
  vm_store_ptr(&fr->rf, ins->a, fn);
  return OP_CONTINUE;
}

op_result op_closure(exec_ctx* ctx, const jello_insn* ins) {
  uint32_t ncaps = (uint32_t)ins->c;
  jello_function* fn = jello_function_new(ctx->vm, ins->imm, ncaps);
  call_frame* fr = current_frame(ctx);
  fn->h.type_id = fr->f->reg_types[ins->a];
  for(uint32_t i = 0; i < ncaps; i++) {
    uint32_t src = (uint32_t)ins->b + i;
    fn->captures[i] = vm_reg_is_wide(&fr->rf, src) ? (uint64_t)vm_load_i64(&fr->rf, src)
                                                   : (uint64_t)vm_load_u32(&fr->rf, src);
  }
  vm_store_ptr(&fr->rf, ins->a, fn);
  return OP_CONTINUE;
}

/* CALLR a=dst b=callee c=arg count imm=first arg register. A null callee traps NULL_DEREF. */
op_result op_callr(exec_ctx* ctx, const jello_insn* ins) {
  if(vm_poll_and_charge(ctx->vm) != 0) return OP_TRAP;
  call_frame* fr = current_frame(ctx);
  if(!vm_push_callr_frame(ctx->vm, ctx->m, vm_load_val(&fr->rf, ins->b), ins->a, (uint32_t)ins->imm,
                          (uint32_t)ins->c)) {
    return OP_TRAP;
  }
#if defined(JELLO_ENABLE_JIT) && (defined(__x86_64__) || defined(__aarch64__))
  /* As in CALL, a compiled callee runs as JIT code. */
  if(ctx->vm->active_jit && jello_jit_enter_pushed_frame(ctx->vm) == 2 &&
     !vm_catch_in_segment(ctx)) {
    ctx->vm->trap_trace.site = current_frame(ctx)->pc;
    return OP_TRAP_LOCATED;
  }
#endif
  return OP_CONTINUE;
}

/* TAILCALL b=first arg c=arg count imm=function, and TAILCALLR with CALLR's operands, replace the
 * current frame, so tail recursion runs in constant frame-stack space. The arguments, and
 * TAILCALLR's captures, are copied out before the frame that holds them is popped. Fuel is
 * charged before the pop, so a fuel trap is located at the tail call. */
op_result op_tailcall(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  if(vm_poll_and_charge(vm) != 0) return OP_TRAP;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  uint32_t arg_count = (uint32_t)ins->c;
  uint64_t argbuf[256];
  for(uint32_t i = 0; i < arg_count; i++) {
    uint32_t src = (uint32_t)ins->b + i;
    argbuf[i] = vm_reg_is_wide(&fr->rf, src) ? (uint64_t)vm_load_i64(&fr->rf, src)
                                             : (uint64_t)vm_load_u32(&fr->rf, src);
  }
  uint8_t has_caller = fr->has_caller;
  uint32_t caller_dst = fr->caller_dst;
  vm_pop_frame(vm);
  if(!vm_push_frame_raw(vm, ctx->m, ins->imm, caller_dst, has_caller, argbuf, arg_count, NULL, 0))
    return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_tailcallr(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  if(vm_poll_and_charge(vm) != 0) return OP_TRAP;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  const jello_function* fn = vm_callee_function(vm, vm_load_val(&fr->rf, ins->b));
  if(!fn) return OP_TRAP;
  uint32_t arg_count = (uint32_t)ins->c;
  uint32_t arg_first = (uint32_t)ins->imm;
  uint64_t argbuf[256];
  for(uint32_t i = 0; i < arg_count; i++) {
    uint32_t src = arg_first + i;
    argbuf[i] = vm_reg_is_wide(&fr->rf, src) ? (uint64_t)vm_load_i64(&fr->rf, src)
                                             : (uint64_t)vm_load_u32(&fr->rf, src);
  }
  uint32_t ncaps = fn->ncaps;
  uint64_t capbuf[256];
  for(uint32_t i = 0; i < ncaps; i++) capbuf[i] = fn->captures[i];
  uint32_t func_index = fn->func_index;
  uint8_t has_caller = fr->has_caller;
  uint32_t caller_dst = fr->caller_dst;
  vm_pop_frame(vm);
  if(!vm_push_frame_raw(vm, ctx->m, func_index, caller_dst, has_caller, argbuf, arg_count, capbuf,
                        ncaps))
    return OP_TRAP;
  return OP_CONTINUE;
}

/** Grows `fr`'s spill slots, filling new ones with null, until `slot` is in range. Returns 0 when
 * allocation fails. */
static int spill_ensure_cap(call_frame* fr, uint32_t slot) {
  if(slot < fr->spill_cap) return 1;
  uint32_t new_cap = fr->spill_cap ? fr->spill_cap * 2u : 8u;
  while(new_cap <= slot) new_cap *= 2u;
  jello_value* p = (jello_value*)realloc(fr->spill_slots, (size_t)new_cap * sizeof(jello_value));
  if(!p) return 0;
  for(uint32_t i = fr->spill_cap; i < new_cap; i++) p[i] = jello_make_null();
  fr->spill_slots = p;
  fr->spill_cap = new_cap;
  return 1;
}

op_result op_spill_push(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a);
  fr = current_frame(ctx);
  if(!spill_ensure_cap(fr, ins->imm)) {
    fprintf(stderr, "DIAG: spill_ensure_cap OOM slot=%u\n", ins->imm);
    fflush(stderr);
    abort();
  }
  fr->spill_slots[ins->imm] = v;
  return OP_CONTINUE;
}

op_result op_spill_pop(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = (ins->imm < fr->spill_cap) ? fr->spill_slots[ins->imm] : jello_make_null();
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

/* SWITCH_KIND a=subject b=row count imm=default. The b CASE_KIND rows that follow are a table;
 * each row's `a` is a kind code (vm_dynamic_static_kind) and its `imm` a jump offset. The first
 * row matching the subject's kind is taken, else the default; offsets are relative to the pc
 * after the table. Execution always jumps past the table, so CASE_KIND itself does nothing. */
op_result op_switch_kind(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_load_val(&fr->rf, ins->a);
  uint32_t kind = vm_dynamic_static_kind(v);
  uint32_t ncases = (uint32_t)ins->b;
  uint32_t table_pc = fr->pc;
  uint32_t after_table = table_pc + ncases;
  for(uint32_t i = 0; i < ncases; i++) {
    const jello_insn* row = &fr->f->insns[table_pc + i];
    if((uint32_t)row->a == kind) {
      fr->pc = (uint32_t)((int32_t)after_table + (int32_t)row->imm);
      return OP_CONTINUE;
    }
  }
  fr->pc = (uint32_t)((int32_t)after_table + (int32_t)ins->imm);
  return OP_CONTINUE;
}

op_result op_case_kind(exec_ctx* ctx, const jello_insn* ins) {
  (void)ctx;
  (void)ins;
  return OP_CONTINUE;
}
