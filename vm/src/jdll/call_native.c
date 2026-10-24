// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * CALL_NATIVE: calls a `.jdll` export and stores its result, null for a Void one, or traps when
 * the export failed or returned the wrong kind. An export whose result is Dynamic may return
 * Bytes, which the Dynamic then holds, so that a blocking one, which cannot allocate, can return
 * Bytes or null. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

op_result op_call_native(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  const jello_import_sig* sig = &m->imports[ins->imm];
  const jello_jdll_export* exp = sig->resolved;
  if(!exp) {
    vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }

  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  jdlo_ctx jctx = {.vm = vm, .m = m, .caller_rf = &fr->rf, .ins = ins, .sig = sig};

  /* Quiescent while the export runs; safepoints cannot see time spent in native code, so a
   * collection on another thread would otherwise wait for the export to finish. A built-in
   * export allocates instead, and goes quiescent itself only to park. A blocking one runs on the
   * blocking pool while its process parks; a stop meanwhile drops its result and traps. */
  int builtin = vm_builtin_export(exp);
  if(builtin) {
    exp->fn(&jctx);
  } else if(exp->flags & JELLO_JDLL_BLOCKING) {
    jello_trap_code stopped = proc_call_blocking(vm, exp->fn, &jctx);
    if(stopped) {
      jdl_release_ret(&jctx);
      vm->trap_code = stopped;
      return OP_TRAP;
    }
  } else {
    proc_call_quiescent(vm, exp->fn, &jctx);
  }

  // A handle or bytes the call will not return are released here, since nothing else owns them.
  jello_type_kind declared = (jello_type_kind)sig->ret_kind;
  // A Void export need not call jdl_return_void.
  if(!jctx.returned && declared == JELLO_T_VOID) jdl_return_void(&jctx);
  int ok = !jctx.failed && jctx.returned &&
           (jctx.ret_kind_given == declared ||
            (declared == JELLO_T_DYNAMIC && jctx.ret_kind_given == JELLO_T_BYTES));
  if(!ok) {
    jdl_release_ret(&jctx);
    vm->trap_code = jctx.failed && jctx.trap ? jctx.trap : JELLO_TRAP_TYPE_MISMATCH;
    // jdl_fail has already said why, if it was given a message.
    if(!jctx.fail_said) {
      const char* what = jctx.failed     ? "failed"
                         : jctx.returned ? "returned the wrong kind of value"
                                         : "returned nothing";
      snprintf(vm->trap_detail, sizeof vm->trap_detail, "native %s %s", exp->name, what);
    }
    return OP_TRAP;
  }
  if(jctx.handle_class) {
    jctx.ret.dyn = jello_from_ptr(
        jello_handle_new(vm, jctx.handle_class, jctx.handle_payload, jctx.handle_bytes));
  }

  reg_frame* rf = &fr->rf;
  switch(jctx.ret_kind_given) {
    case JELLO_T_VOID: vm_store_val(rf, ins->a, jello_make_null()); break;
    case JELLO_T_BOOL:
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
    case JELLO_T_ATOM:
      vm_store_u32_masked(rf, ins->a, (uint32_t)jctx.ret.i32, jctx.ret_kind_given);
      break;
    case JELLO_T_I64: vm_store_i64(rf, ins->a, jctx.ret.i64); break;
    case JELLO_T_F16: vm_store_u32(rf, ins->a, (uint32_t)vm_f32_to_f16(jctx.ret.f32)); break;
    case JELLO_T_F32: vm_store_f32(rf, ins->a, jctx.ret.f32); break;
    case JELLO_T_F64: vm_store_f64(rf, ins->a, jctx.ret.f64); break;
    case JELLO_T_DYNAMIC: vm_store_val(rf, ins->a, jctx.ret.dyn); break;
    case JELLO_T_BYTES: {
      jello_bytes* b = jello_bytes_new(vm, jctx.ret_bytes_len);
      if(jctx.ret_bytes_len) memcpy(b->data, jctx.ret_bytes, jctx.ret_bytes_len);
      free(jctx.ret_bytes);
      if(declared == JELLO_T_DYNAMIC) vm_store_val(rf, ins->a, jello_from_ptr(b));
      else vm_store_ptr(rf, ins->a, b);
      break;
    }
    default: vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return OP_TRAP;
  }
  return OP_CONTINUE;
}
