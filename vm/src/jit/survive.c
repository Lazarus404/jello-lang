// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Call-survive planning; picks the typed calls across which one I32 value stays in
 * JIT_REG_SURVIVE instead of being reloaded from its home. */

#include <jello/internal/jit_impl.h>
#include <stdlib.h>

/** Whether `op`'s a slot is an input rather than its destination. Register b and c slots are
 * always inputs. */
static int op_a_is_value_input(jello_jit_ir_op op) {
  switch(op) {
    case JIR_RET:
    case JIR_JMP_IF:
    case JIR_BYTES_SET_U8:
    case JIR_BYTES_WRITE:
    case JIR_BYTES_BLIT:
    case JIR_BITS_WRITE:
    case JIR_BYTES_FREEZE:
    case JIR_STRUCT_SET:
    case JIR_OBJ_SET_ATOM:
    case JIR_OBJ_SET:
    case JIR_ARRAY_SET:
    case JIR_GLOBAL_SET:
    case JIR_THROW: return 1;
    default: return 0;
  }
}

/** Ops that end a survive window like a branch; no hold is armed at or reaches across them. */
static int op_is_branch(jello_jit_ir_op op) {
  return op == JIR_JMP || op == JIR_JMP_IF || op == JIR_CALLR || op == JIR_TRY ||
         op == JIR_ENDTRY || op == JIR_THROW;
}

static int op_is_call(jello_jit_ir_op op) { return op == JIR_CALL_SELF || op == JIR_CALL_DIRECT; }

/** Whether `ins` writes vreg `v` through its a slot. */
static int insn_writes_vreg(const jello_jit_ir_insn* ins, uint8_t v) {
  jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
  unsigned mask = jello_jit_ir_op_reg_mask(op);
  return (mask & JELLO_JIR_REG_A) && !op_a_is_value_input(op) && ins->a == v;
}

/** Whether emitting `ins` reads vreg `v` through jit_reg_get_gpr. A call's arguments do not
 * count; the call copies them from their homes, so a hold used only as a later argument would
 * gain nothing. */
static int insn_reads_vreg(const jello_jit_ir_insn* ins, uint8_t v) {
  jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
  unsigned mask = jello_jit_ir_op_reg_mask(op);
  if((mask & JELLO_JIR_REG_A) && op_a_is_value_input(op) && ins->a == v) return 1;
  if(op_is_call(op) || op == JIR_CALL_NATIVE) return 0;
  if((mask & JELLO_JIR_REG_B) && ins->b == v) return 1;
  if((mask & JELLO_JIR_REG_C) && ins->c == v) return 1;
  return 0;
}

enum { JIT_SURVIVE_WINDOW = 64u };

/** Scans back from `call_idx` for the most recently defined I32 register that is neither an
 * argument of the call nor its result, stopping at a branch, a branch target or the window
 * edge. Returns 1 with the register in `*out_candidate` when found. */
static int find_candidate(const jello_jit_ir_function* ir, const jello_bc_module* m,
                          const jello_bc_function* f, const uint8_t* is_target, uint32_t call_idx,
                          uint8_t* out_candidate) {
  const jello_jit_ir_insn* call_ins = &ir->insns[call_idx];
  uint32_t arg_first = call_ins->b, arg_end = arg_first + call_ins->c;
  uint32_t floor = (call_idx > JIT_SURVIVE_WINDOW) ? call_idx - JIT_SURVIVE_WINDOW : 0;

  for(uint32_t j = call_idx; j > floor;) {
    j--;
    const jello_jit_ir_insn* ins = &ir->insns[j];
    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if(op_is_branch(op)) return 0;
    if(ins->bc_pc < f->ninsns && is_target[ins->bc_pc]) return 0;
    unsigned mask = jello_jit_ir_op_reg_mask(op);
    if((mask & JELLO_JIR_REG_A) && !op_a_is_value_input(op)) {
      uint8_t v = ins->a;
      if(v >= arg_first && v < arg_end) continue;
      if(v == call_ins->a) continue;
      if(vm_reg_kind(m, f, v) != JELLO_T_I32) continue;
      *out_candidate = v;
      return 1;
    }
  }
  return 0;
}

/** Scans forward from the call for the first instruction that reads `candidate`, stopping at a
 * write to it, a branch, a branch target, a JIR_SLOW or the window edge. Returns 1 with its
 * index in `*out_consumer` when found. */
static int find_consumer(const jello_jit_ir_function* ir, const jello_bc_function* f,
                         const uint8_t* is_target, uint32_t call_idx, uint8_t candidate,
                         uint32_t* out_consumer) {
  uint32_t ceil = call_idx + 1u + JIT_SURVIVE_WINDOW;
  if(ceil > ir->ninsns) ceil = ir->ninsns;

  for(uint32_t j = call_idx + 1; j < ceil; j++) {
    const jello_jit_ir_insn* ins = &ir->insns[j];
    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if(ins->bc_pc < f->ninsns && is_target[ins->bc_pc]) return 0;
    if(insn_reads_vreg(ins, candidate)) {
      *out_consumer = j;
      return 1;
    }
    if(op_is_branch(op)) return 0;
    /* An interpreter handler may write any register, unseen by the masks. */
    if(op == JIR_SLOW) return 0;
    if(insn_writes_vreg(ins, candidate)) return 0;
  }
  return 0;
}

int jit_survive_plan_build(const jello_jit_ir_function* ir, const jello_bc_module* m,
                           const jello_bc_function* f, const uint8_t* is_target,
                           jit_survive_plan* out_plan) {
  out_plan->armed = (uint8_t*)calloc(ir->ninsns ? ir->ninsns : 1u, 1);
  out_plan->candidate = (uint8_t*)calloc(ir->ninsns ? ir->ninsns : 1u, 1);
  out_plan->is_consumer = (uint8_t*)calloc(ir->ninsns ? ir->ninsns : 1u, 1);
  out_plan->nregs = ir->nregs;
  out_plan->any_armed = 0;
  if(!out_plan->armed || !out_plan->candidate || !out_plan->is_consumer) {
    jit_survive_plan_free(out_plan);
    return 0;
  }

  /* A call inside an armed call's [call + 1, consumer] span, consumer included, is not
   * considered, so two holds never overlap. */
  uint32_t busy_until = 0;
  int have_busy = 0;

  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    if(!op_is_call(op)) continue;
    if(have_busy && i <= busy_until) {
      jit_call_survive_decline_count++;
      continue;
    }

    uint8_t candidate = 0;
    uint32_t consumer = 0;
    if(find_candidate(ir, m, f, is_target, i, &candidate) &&
       find_consumer(ir, f, is_target, i, candidate, &consumer)) {
      out_plan->armed[i] = 1;
      out_plan->candidate[i] = candidate;
      out_plan->is_consumer[consumer] = 1;
      out_plan->any_armed = 1;
      busy_until = consumer;
      have_busy = 1;
      jit_call_survive_armed_count++;
    } else {
      jit_call_survive_decline_count++;
    }
  }
  return 1;
}

void jit_survive_plan_free(jit_survive_plan* plan) {
  free(plan->armed);
  free(plan->candidate);
  free(plan->is_consumer);
  plan->armed = NULL;
  plan->candidate = NULL;
  plan->is_consumer = NULL;
}
