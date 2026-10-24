// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The interpreter's main loop, which runs the hottest opcodes inline and the rest through
 * op_dispatch, and the jello_vm_exec_numeric entry points. */

#include <jello/internal.h>
#include <jello/internal/proc_internal.h>
#include <assert.h>
#include <stdlib.h>

jello_exec_status vm_exec_loop(exec_ctx* ctx) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;

  for(;;) {
    call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
    const jello_bc_function* f = fr->f;
    const jello_insn* ins = &f->insns[fr->pc++];
    reg_frame* rf = &fr->rf;

    switch((jello_op)ins->op) {
      case JOP_NOP: break;

      case JOP_RET: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint8_t has_caller = fr->has_caller;
        uint32_t caller_dst = fr->caller_dst;

        if(!has_caller) {
          jello_numeric_result* out = ctx->out;
          switch(k) {
            case JELLO_T_BOOL:
              out->kind = JELLO_NUM_BOOL;
              out->as.b = (int)vm_load_u32(rf, ins->a);
              break;
            case JELLO_T_I32:
              out->kind = JELLO_NUM_I32;
              out->as.i32 = (int32_t)vm_load_u32(rf, ins->a);
              break;
            case JELLO_T_I64:
              out->kind = JELLO_NUM_I64;
              out->as.i64 = vm_load_i64(rf, ins->a);
              break;
            case JELLO_T_F32:
              out->kind = JELLO_NUM_F32;
              out->as.f32 = vm_load_f32(rf, ins->a);
              break;
            case JELLO_T_F64:
              out->kind = JELLO_NUM_F64;
              out->as.f64 = vm_load_f64(rf, ins->a);
              break;
            /* A top-level RET returns a Bytes, Struct or Object as its pointer,
             * which may be NULL, and a Dynamic as its tagged word. */
            case JELLO_T_BYTES:
            case JELLO_T_STRUCT:
            case JELLO_T_OBJECT:
              out->kind = JELLO_NUM_PTR;
              out->as.ptr = vm_load_ptr(rf, ins->a);
              break;
            case JELLO_T_DYNAMIC:
              out->kind = JELLO_NUM_DYN;
              out->as.dyn = vm_load_val(rf, ins->a);
              break;
            default: out->kind = JELLO_NUM_VOID; break;
          }
          vm_pop_frame(vm);
          return JELLO_EXEC_OK;
        }

        /* Read the result before the pop releases the frame. Slots are 4 or
         * 8 bytes, so a fixed-width load and store do the copy. */
        uint64_t tmp;
        int is_wide = vm_reg_is_wide(rf, ins->a);
        if(is_wide) tmp = *(const uint64_t*)vm_reg_ptr(rf, ins->a);
        else tmp = *(const uint32_t*)vm_reg_ptr(rf, ins->a);
        vm_pop_frame(vm);
        call_frame* caller = &vm->call_frames[vm->call_frames_len - 1u];
        uint8_t* dst = vm_reg_ptr(&caller->rf, caller_dst);
        if(is_wide) *(uint64_t*)dst = tmp;
        else *(uint32_t*)dst = (uint32_t)tmp;
        if(vm->call_frames_len == ctx->stop_len) return JELLO_EXEC_OK;
        break;
      }

      /* A backward jump is a loop back-edge. JMP_IF charges whether or not it is taken, as JIT
       * code does. */
      case JOP_JMP:
        if((int32_t)ins->imm < 0) {
          if(vm_poll_and_charge(vm) != 0) {
            vm->trap_func_index = fr->func_index;
            vm->trap_pc = fr->pc - 1u;
            return JELLO_EXEC_TRAP;
          }
        }
        fr->pc = (uint32_t)((int32_t)fr->pc + (int32_t)ins->imm);
        break;

      case JOP_JMP_IF:
        if((int32_t)ins->imm < 0 && vm_poll_and_charge(vm) != 0) {
          vm->trap_func_index = fr->func_index;
          vm->trap_pc = fr->pc - 1u;
          return JELLO_EXEC_TRAP;
        }
        if(vm_load_u32(rf, ins->a) != 0u) fr->pc = (uint32_t)((int32_t)fr->pc + (int32_t)ins->imm);
        break;

      case JOP_MOV: {
        if(vm_reg_is_wide(rf, ins->a)) vm_store_i64(rf, ins->a, vm_load_i64(rf, ins->b));
        else vm_store_u32(rf, ins->a, vm_load_u32(rf, ins->b));
        break;
      }

      case JOP_CALL: {
        if(vm_poll_and_charge(vm) != 0) {
          vm->trap_func_index = fr->func_index;
          vm->trap_pc = fr->pc - 1u;
          return JELLO_EXEC_TRAP;
        }
        if(!vm_push_frame(vm, m, ins->imm, ins->a, ins->b, ins->c, 1)) {
          vm->trap_func_index = fr->func_index;
          vm->trap_pc = fr->pc - 1u;
          return JELLO_EXEC_TRAP;
        }
#if defined(JELLO_ENABLE_JIT) && (defined(__x86_64__) || defined(__aarch64__))
        /* Run a compiled callee as JIT code. Its trap, already located, ends
         * this loop unless a TRY in this loop's frames catches it. */
        if(vm->active_jit && jello_jit_enter_pushed_frame(vm) == 2 && !vm_catch_in_segment(ctx)) {
          vm->trap_trace.site = vm->call_frames[vm->call_frames_len - 1u].pc;
          return JELLO_EXEC_TRAP;
        }
#endif
        break;
      }

      case JOP_CONST_I8_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        vm_store_u32_masked(rf, ins->a, ins->imm, k);
        break;
      }
      case JOP_CONST_I32: vm_store_u32(rf, ins->a, ins->imm); break;
      case JOP_CONST_I64: vm_store_i64(rf, ins->a, m->const_i64[ins->imm]); break;
      case JOP_CONST_F16: vm_store_u32(rf, ins->a, ins->imm & 0xFFFFu); break;
      case JOP_CONST_F32: vm_store_f32(rf, ins->a, vm_f32_from_bits(ins->imm)); break;
      case JOP_CONST_F64: vm_store_f64(rf, ins->a, m->const_f64[ins->imm]); break;
      case JOP_CONST_BOOL: vm_store_u32(rf, ins->a, ins->imm ? 1u : 0u); break;

      /* I32 arithmetic is unsigned so overflow wraps (signed overflow is undefined in C); the
       * masked store narrows the result to an I8 or I16 destination. */
      case JOP_ADD_I32: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        uint32_t b = vm_load_u32(rf, ins->c);
        vm_store_u32_masked(rf, ins->a, a + b, k);
        break;
      }
      case JOP_SUB_I32: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        uint32_t b = vm_load_u32(rf, ins->c);
        vm_store_u32_masked(rf, ins->a, a - b, k);
        break;
      }
      case JOP_MUL_I32: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        uint32_t b = vm_load_u32(rf, ins->c);
        vm_store_u32_masked(rf, ins->a, a * b, k);
        break;
      }
      case JOP_ADD_I32_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        vm_store_u32_masked(rf, ins->a, a + (uint32_t)ins->imm, k);
        break;
      }
      case JOP_SUB_I32_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        vm_store_u32_masked(rf, ins->a, a - (uint32_t)ins->imm, k);
        break;
      }
      case JOP_MUL_I32_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        vm_store_u32_masked(rf, ins->a, a * (uint32_t)ins->imm, k);
        break;
      }

      case JOP_ADD_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a + b);
        break;
      }
      case JOP_SUB_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a - b);
        break;
      }
      case JOP_MUL_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a * b);
        break;
      }
      case JOP_DIV_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a / b);
        break;
      }
      case JOP_NEG_F64: {
        double x = vm_load_f64(rf, ins->b);
        vm_store_f64(rf, ins->a, -x);
        break;
      }
      case JOP_F64_FROM_I32: {
        int32_t x = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_f64(rf, ins->a, (double)x);
        break;
      }
      case JOP_NOT_BOOL: {
        uint32_t x = vm_load_u32(rf, ins->b);
        vm_store_u32(rf, ins->a, x ? 0u : 1u);
        break;
      }

      case JOP_EQ_I32: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        int32_t b = (int32_t)vm_load_u32(rf, ins->c);
        vm_store_u32(rf, ins->a, (a == b) ? 1u : 0u);
        break;
      }
      case JOP_LT_I32: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        int32_t b = (int32_t)vm_load_u32(rf, ins->c);
        vm_store_u32(rf, ins->a, (a < b) ? 1u : 0u);
        break;
      }
      case JOP_EQ_I32_IMM: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32(rf, ins->a, (a == (int32_t)ins->imm) ? 1u : 0u);
        break;
      }
      case JOP_LT_I32_IMM: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32(rf, ins->a, (a < (int32_t)ins->imm) ? 1u : 0u);
        break;
      }
      case JOP_EQ_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_u32(rf, ins->a, (a == b) ? 1u : 0u);
        break;
      }
      case JOP_LT_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_u32(rf, ins->a, (a < b) ? 1u : 0u);
        break;
      }

      default: {
        op_result r = op_dispatch(ctx, ins);
        if(r == OP_TRAP_LOCATED) return JELLO_EXEC_TRAP;
        if(r == OP_TRAP) {
          /* Cold-path handlers leave locating a trap to this loop; fr->pc is already past the
           * trapping instruction. */
          vm->trap_func_index = fr->func_index;
          vm->trap_pc = fr->pc - 1u;
          return JELLO_EXEC_TRAP;
        }
        break;
      }
    }
  }
}

/** Runs `vm`'s top frame, which has no caller, to its return or a trap, as JIT code from `jm`
 * when that is prepared and has compiled it, and otherwise interpreted. A trap leaves the
 * interpreter's frames pushed; their call sites complete the trace. */
static jello_exec_status exec_top(jello_vm* vm, const jello_bc_module* m, jello_jit_module* jm,
                                  jello_numeric_result* out) {
  uint32_t base = vm->call_frames_len - 1u;
  jello_exec_status st = JELLO_EXEC_OK;
  int ran = 0;
#if defined(JELLO_ENABLE_JIT) && (defined(__x86_64__) || defined(__aarch64__))
  ran = jm && jello_jit_try_enter(vm, jm, out, &st);
#endif
  if(!ran) {
    exec_ctx ctx = {vm, m, out, 0};
    st = vm_exec_loop(&ctx);
  }
  assert(vm->trap_code != JELLO_TRAP_INTERNAL_UNWIND && "a caught throw escaped its catcher");
  if(st == JELLO_EXEC_TRAP) vm_trace_frames(vm, base);
  return st;
}

/** Stops and frees the processes `vm` has started, if any, keeping the object in `out` alive
 * while they finish. */
static void end_processes(jello_vm* vm, const jello_numeric_result* out) {
  if(!vm->proc) return;
  void* obj = NULL;
  if(out->kind == JELLO_NUM_PTR) obj = out->as.ptr;
  if(out->kind == JELLO_NUM_DYN && jello_is_ptr(out->as.dyn)) obj = jello_as_ptr(out->as.dyn);
  if(obj) jello_gc_push_root(vm, obj);
  proc_end_all(vm);
  if(obj) jello_gc_pop_roots(vm, 1);
}

void vm_end_kept(jello_vm* vm) {
  proc_end_all(vm);
#if defined(JELLO_ENABLE_JIT) && (defined(__x86_64__) || defined(__aarch64__))
  if(vm->kept_jit) {
    jello_jit_module_free(vm->kept_jit);
    free(vm->kept_jit);
    vm->kept_jit = NULL;
  }
#endif
}

/** Runs module `m` from its entry function on `vm` and stores the entry's result in `out`.
 * Unless JELLO_JIT_DISABLE is set and non-empty (JIT builds only), JIT code runs from
 * `persistent_jm`, or when that is NULL from a jello_jit_module prepared lazily
 * and freed for this call. */
static jello_exec_status vm_exec_numeric_impl(jello_vm* vm, const jello_bc_module* m,
                                              jello_numeric_result* out,
                                              jello_jit_module* persistent_jm) {
  /* Park at a pending stop-the-world (GC or reload) once in_exec is set; the collector waits
   * only for threads in_exec, so checking first, a stop could begin between the two and an idle
   * thread start running a module that a reload is patching. */
  atomic_store(&vm->in_exec, 1);
  vm_safepoint_check(vm);

  vm_end_kept(vm); /* Processes spawned outside a run share the layouts reset frees. */
  vm_frame_reset(vm);
  out->kind = JELLO_NUM_VOID;

  vm_enter_module(vm, m);

  if(!vm_push_frame(vm, m, jello_bc_get_entry(m), 0, 0, 0, 0)) {
    /* With no frame pushed, the trap is located at the entry function. */
    vm->trap_func_index = jello_bc_get_entry(m);
    vm->trap_pc = 0;
    atomic_store(&vm->in_exec, 0);
    return JELLO_EXEC_TRAP;
  }

  vm_warm_jit_layouts(vm, m);

#if defined(JELLO_ENABLE_JIT) && (defined(__x86_64__) || defined(__aarch64__))
  /* This path runs only on a host matching the linked JIT backend (x86_64, natively or under
   * Rosetta, or arm64). The entry function runs as JIT code when it compiled; otherwise the
   * interpreter runs it and enters compiled callees as it reaches them. */
  const char* jit_off = getenv("JELLO_JIT_DISABLE");
  if(!(jit_off && jit_off[0] != '\0')) {
    jello_jit_module local_jm;
    int owns_jm = persistent_jm == NULL;
    int keep = vm->keep_processes;
    jello_jit_module* jm = persistent_jm;
    if(owns_jm) {
      /* Processes left running keep the module, so it outlives this call. */
      jm = keep ? (jello_jit_module*)malloc(sizeof(*jm)) : &local_jm;
      if(!jm) abort();
      memset(jm, 0, sizeof(*jm));
      jello_jit_module_prepare_lazy(m, jm);
    }
    if(jm->prepared) {
      vm->active_jit = jm;
      jit_native_stack_here(vm);
    }
    jello_exec_status st = exec_top(vm, m, jm, out);
    if(keep) proc_leave_to_host(vm);
    else end_processes(vm, out);
    vm->active_jit = NULL;
    if(owns_jm && keep) vm->kept_jit = jm;
    else if(owns_jm) jello_jit_module_free(jm);
    atomic_store(&vm->in_exec, 0);
    return st;
  }
#endif

  jello_exec_status st = exec_top(vm, m, NULL, out);
  if(!vm->keep_processes) end_processes(vm, out);
  else proc_leave_to_host(vm);
  atomic_store(&vm->in_exec, 0);
  return st;
}

jello_exec_status vm_exec_function(jello_vm* vm, const jello_function* fn, const uint64_t* args,
                                   uint32_t nargs, jello_numeric_result* out) {
  const jello_bc_module* m = vm->running_module;
  out->kind = JELLO_NUM_VOID;
  vm_clear_trap(vm);
  if(!vm_push_frame_raw(vm, m, fn->func_index, 0, 0, args, nargs, fn->captures, fn->ncaps)) {
    vm->trap_func_index = fn->func_index;
    vm->trap_pc = 0;
    return JELLO_EXEC_TRAP;
  }
  int in_exec = atomic_exchange(&vm->in_exec, 1);
  jello_exec_status st = exec_top(vm, m, vm->active_jit, out);
  atomic_store(&vm->in_exec, in_exec);
  return st;
}

jello_exec_status jello_vm_exec_numeric(jello_vm* vm, const jello_bc_module* m,
                                        jello_numeric_result* out) {
  return vm_exec_numeric_impl(vm, m, out, NULL);
}

jello_exec_status jello_vm_exec_numeric_with_jit(jello_vm* vm, jello_jit_module* jm,
                                                 const jello_bc_module* m,
                                                 jello_numeric_result* out) {
  return vm_exec_numeric_impl(vm, m, out, jm);
}

jello_trap_code jello_vm_last_trap_code(const jello_vm* vm) { return vm->trap_code; }
