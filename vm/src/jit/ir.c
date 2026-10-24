// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Lowers a bytecode function into the JIR in one pass, classifying each CALL as self-recursive
 * or direct and placing safepoints where the interpreter checks for them. */

#include <jello/internal/jit_impl.h>
#include <stdlib.h>

typedef struct ir_builder {
  jello_jit_ir_insn* insns;
  uint32_t count;
  uint32_t cap;
} ir_builder;

static void ir_push(ir_builder* b, jello_jit_ir_op op, uint8_t a, uint8_t bb, uint8_t c,
                    uint32_t imm, uint32_t bc_pc) {
  if(b->count == b->cap) {
    uint32_t new_cap = b->cap ? b->cap * 2u : 16u;
    jello_jit_ir_insn* grown =
        (jello_jit_ir_insn*)realloc(b->insns, (size_t)new_cap * sizeof(jello_jit_ir_insn));
    if(!grown) abort();
    b->insns = grown;
    b->cap = new_cap;
  }
  jello_jit_ir_insn* ins = &b->insns[b->count++];
  ins->op = (uint16_t)op;
  ins->a = a;
  ins->b = bb;
  ins->c = c;
  ins->imm = imm;
  ins->bc_pc = bc_pc;
}

static jello_jir_bin arith_operator(jello_op op) {
  switch(op) {
    case JOP_ADD_I32:
    case JOP_ADD_I64:
    case JOP_ADD_F16:
    case JOP_ADD_F32:
    case JOP_ADD_F64: return JIR_BIN_ADD;
    case JOP_SUB_I32:
    case JOP_SUB_I64:
    case JOP_SUB_F16:
    case JOP_SUB_F32:
    case JOP_SUB_F64: return JIR_BIN_SUB;
    case JOP_MUL_I32:
    case JOP_MUL_I64:
    case JOP_MUL_F16:
    case JOP_MUL_F32:
    case JOP_MUL_F64: return JIR_BIN_MUL;
    case JOP_DIV_I32:
    case JOP_DIV_I64:
    case JOP_DIV_F32:
    case JOP_DIV_F64: return JIR_BIN_DIV;
    case JOP_MOD_I32:
    case JOP_MOD_I64: return JIR_BIN_MOD;
    case JOP_SHL_I32:
    case JOP_SHL_I64: return JIR_BIN_SHL;
    case JOP_SHR_I32:
    case JOP_SHR_I64: return JIR_BIN_SHR;
    case JOP_BITAND_I32:
    case JOP_BITAND_I64: return JIR_BIN_AND;
    case JOP_BITOR_I32:
    case JOP_BITOR_I64: return JIR_BIN_OR;
    case JOP_BITXOR_I32:
    case JOP_BITXOR_I64: return JIR_BIN_XOR;
    default: abort();
  }
}

static jello_jir_cmp cmp_operator(jello_op op) {
  switch(op) {
    case JOP_EQ_I32:
    case JOP_EQ_I64:
    case JOP_EQ_F32:
    case JOP_EQ_F64: return JIR_CMP_EQ;
    case JOP_LT_I32:
    case JOP_LT_I64:
    case JOP_LT_F32:
    case JOP_LT_F64: return JIR_CMP_LT;
    default: abort();
  }
}

static jello_jir_conv conv_kind_for(jello_op op) {
  switch(op) {
    case JOP_SEXT_I64: return JIR_CONV_SEXT_I64;
    case JOP_SEXT_I16: return JIR_CONV_SEXT_I16;
    case JOP_TRUNC_I8: return JIR_CONV_TRUNC_I8;
    case JOP_TRUNC_I16: return JIR_CONV_TRUNC_I16;
    case JOP_I32_FROM_I64: return JIR_CONV_I32_FROM_I64;
    case JOP_F64_FROM_I32: return JIR_CONV_F64_FROM_I32;
    case JOP_I32_FROM_F64: return JIR_CONV_I32_FROM_F64;
    case JOP_F64_FROM_I64: return JIR_CONV_F64_FROM_I64;
    case JOP_I64_FROM_F64: return JIR_CONV_I64_FROM_F64;
    case JOP_F32_FROM_I32: return JIR_CONV_F32_FROM_I32;
    case JOP_I32_FROM_F32: return JIR_CONV_I32_FROM_F32;
    case JOP_F64_FROM_F32: return JIR_CONV_F64_FROM_F32;
    case JOP_F32_FROM_F64: return JIR_CONV_F32_FROM_F64;
    case JOP_F32_FROM_I64: return JIR_CONV_F32_FROM_I64;
    case JOP_I64_FROM_F32: return JIR_CONV_I64_FROM_F32;
    case JOP_F16_FROM_F32: return JIR_CONV_F16_FROM_F32;
    case JOP_F32_FROM_F16: return JIR_CONV_F32_FROM_F16;
    case JOP_F16_FROM_I32: return JIR_CONV_F16_FROM_I32;
    case JOP_I32_FROM_F16: return JIR_CONV_I32_FROM_F16;
    default: abort();
  }
}

/** FROM_DYN_PTR's target kind is POLY in the op table; it is the destination register's own
 * type, Bytes, Struct or Object. */
static jello_type_kind from_dyn_target_kind(const jello_bc_module* m, const jello_bc_function* f,
                                            const jello_insn* ins) {
  uint8_t ak = jello_op_table[ins->op].a_kind;
  if(ak != JELLO_OPK_POLY) return (jello_type_kind)ak;
  return vm_reg_kind(m, f, ins->a);
}

static jello_jir_bytes_rw bytes_read_kind_for(jello_op op) {
  switch(op) {
    case JOP_BYTES_READ_U16_LE: return JIR_BRW_U16_LE;
    case JOP_BYTES_READ_U16_BE: return JIR_BRW_U16_BE;
    case JOP_BYTES_READ_U32_LE: return JIR_BRW_U32_LE;
    case JOP_BYTES_READ_U32_BE: return JIR_BRW_U32_BE;
    case JOP_BYTES_READ_I32_LE: return JIR_BRW_I32_LE;
    case JOP_BYTES_READ_I32_BE: return JIR_BRW_I32_BE;
    case JOP_BYTES_READ_F32_LE: return JIR_BRW_F32_LE;
    case JOP_BYTES_READ_F32_BE: return JIR_BRW_F32_BE;
    case JOP_BYTES_READ_I64_LE: return JIR_BRW_I64_LE;
    case JOP_BYTES_READ_I64_BE: return JIR_BRW_I64_BE;
    case JOP_BYTES_READ_F64_LE: return JIR_BRW_F64_LE;
    case JOP_BYTES_READ_F64_BE: return JIR_BRW_F64_BE;
    default: abort();
  }
}

static jello_jir_bytes_rw bytes_write_kind_for(jello_op op) {
  switch(op) {
    case JOP_BYTES_WRITE_U16_LE: return JIR_BRW_U16_LE;
    case JOP_BYTES_WRITE_U16_BE: return JIR_BRW_U16_BE;
    case JOP_BYTES_WRITE_U32_LE: return JIR_BRW_U32_LE;
    case JOP_BYTES_WRITE_U32_BE: return JIR_BRW_U32_BE;
    case JOP_BYTES_WRITE_I32_LE: return JIR_BRW_I32_LE;
    case JOP_BYTES_WRITE_I32_BE: return JIR_BRW_I32_BE;
    case JOP_BYTES_WRITE_F32_LE: return JIR_BRW_F32_LE;
    case JOP_BYTES_WRITE_F32_BE: return JIR_BRW_F32_BE;
    case JOP_BYTES_WRITE_I64_LE: return JIR_BRW_I64_LE;
    case JOP_BYTES_WRITE_I64_BE: return JIR_BRW_I64_BE;
    case JOP_BYTES_WRITE_F64_LE: return JIR_BRW_F64_LE;
    case JOP_BYTES_WRITE_F64_BE: return JIR_BRW_F64_BE;
    default: abort();
  }
}

static jello_jir_bin bytes_bitop_operator(jello_op op) {
  switch(op) {
    case JOP_BYTES_BITAND2: return JIR_BIN_AND;
    case JOP_BYTES_BITOR2: return JIR_BIN_OR;
    case JOP_BYTES_BITXOR2: return JIR_BIN_XOR;
    default: abort();
  }
}

int jello_jit_ir_slow_runs_handler(jello_op op) {
  switch(op) {
    case JOP_CALL_NATIVE:
    case JOP_CONST_FUN:
    case JOP_CLOSURE:
    case JOP_CONST_BYTES:
    case JOP_LIST_NIL:
    case JOP_LIST_CONS:
    case JOP_LIST_HEAD:
    case JOP_LIST_TAIL:
    case JOP_LIST_IS_NIL:
    case JOP_ARRAY_NEW:
    case JOP_ARRAY_LEN:
    case JOP_ARRAY_GET:
    case JOP_ARRAY_SET:
    case JOP_ARRAY_CONCAT2:
    case JOP_ARRAY_RESIZE:
    case JOP_ENUM_NEW:
    case JOP_DIV_U32:
    case JOP_MOD_U32:
    case JOP_DIV_U64:
    case JOP_MOD_U64:
    case JOP_LT_U32:
    case JOP_LT_U64:
    case JOP_ZEXT_I64:
    case JOP_FMA_F64:
    case JOP_LDEXP_F64:
    case JOP_COPYSIGN_F64:
    case JOP_GLOBAL_GET:
    case JOP_GLOBAL_SET:
    case JOP_BYTES_SLICE:
    case JOP_BYTES_CONCAT_MANY:
    case JOP_BYTES_JOIN:
    case JOP_OBJ_KEYS:
    case JOP_SPILL_PUSH:
    case JOP_SPILL_POP: return 1;
    default: return 0;
  }
}

unsigned jello_jit_ir_op_reg_mask(jello_jit_ir_op op) {
  switch(op) {
    case JIR_NOP:
    case JIR_SAFEPOINT:
    case JIR_JMP:
    case JIR_CONST_NULL:
    case JIR_SLOW:
    case JIR_ENDTRY:
      // JIR_SLOW's operands are the source instruction's, whose shapes vary.
      return 0;
    case JIR_RET:
    case JIR_JMP_IF:
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32:
    case JIR_CONST_I64:
    case JIR_CONST_F16:
    case JIR_CONST_F32:
    case JIR_CONST_F64:
    case JIR_CONST_BOOL:
    case JIR_CONST_ATOM:
    case JIR_STRUCT_NEW:
    case JIR_OBJ_NEW:
    case JIR_NULL_CHECK:
    case JIR_BYTES_FREEZE:
    case JIR_TRY:
    case JIR_THROW:
    case JIR_GLOBAL_GET:
    case JIR_GLOBAL_SET: return JELLO_JIR_REG_A;
    case JIR_OBJ_CLEAR: return JELLO_JIR_REG_B;
    case JIR_MOV:
    case JIR_NEG_I32:
    case JIR_NEG_I64:
    case JIR_NEG_F32:
    case JIR_NEG_F64:
    case JIR_NOT_BOOL:
    case JIR_ADD_I32_IMM:
    case JIR_SUB_I32_IMM:
    case JIR_MUL_I32_IMM:
    case JIR_EQ_I32_IMM:
    case JIR_LT_I32_IMM:
    case JIR_CONV:
    case JIR_TO_DYN:
    case JIR_FROM_DYN:
    case JIR_KINDOF:
    case JIR_IS_NULL:
    case JIR_IS_ENUM:
    case JIR_BYTES_NEW:
    case JIR_BYTES_LEN:
    case JIR_STRUCT_GET:
    case JIR_STRUCT_SET:
    case JIR_OBJ_HAS_ATOM:
    case JIR_OBJ_GET_ATOM:
    case JIR_OBJ_SET_ATOM:
    case JIR_OBJ_COPY:
    case JIR_ARRAY_LEN:
    case JIR_ENUM_TAG: return JELLO_JIR_REG_A | JELLO_JIR_REG_B;
    // `c` is a literal; the bit width, or a call's argument count.
    case JIR_BITS_WRITE:
    case JIR_BITS_READ:
    case JIR_CALL_SELF:
    case JIR_CALL_DIRECT:
    case JIR_CALL_NATIVE:
    case JIR_CALLR: return JELLO_JIR_REG_A | JELLO_JIR_REG_B;
    case JIR_BIN_I32:
    case JIR_BIN_I64:
    case JIR_BIN_F16:
    case JIR_BIN_F32:
    case JIR_BIN_F64:
    case JIR_CMP_I32:
    case JIR_CMP_I64:
    case JIR_CMP_F32:
    case JIR_CMP_F64:
    case JIR_PHYSEQ:
    case JIR_BYTES_GET_U8:
    case JIR_BYTES_SET_U8:
    case JIR_BYTES_EQ:
    case JIR_BYTES_CONCAT2:
    case JIR_BYTES_BITOP:
    case JIR_BYTES_READ:
    case JIR_BYTES_WRITE:
    case JIR_BYTES_BLIT:
    case JIR_OBJ_GET:
    case JIR_OBJ_SET:
    case JIR_OBJ_HAS:
    case JIR_OBJ_REMOVE:
    case JIR_ARRAY_GET:
    case JIR_ARRAY_SET:
    case JIR_ENUM_GET: return JELLO_JIR_REG_A | JELLO_JIR_REG_B | JELLO_JIR_REG_C;
    default: return 0;
  }
}

jello_jit_ir_function* jello_jit_ir_build(const jello_bc_module* m, uint32_t fi) {
  if(fi >= m->nfuncs) return NULL;
  const jello_bc_function* f = &m->funcs[fi];

  ir_builder b;
  b.insns = NULL;
  b.count = 0;
  b.cap = 0;
  uint32_t flags_acc = 0;

  for(uint32_t pc = 0; pc < f->ninsns; pc++) {
    const jello_insn* ins = &f->insns[pc];
    jello_op op = (jello_op)ins->op;
    flags_acc |= jello_op_table[op].flags;

    /* The points vm_poll_and_charge covers in the interpreter. TAILCALL and TAILCALLR make the
     * function decline, so the interpreter charges them. */
    int backward_branch = (op == JOP_JMP || op == JOP_JMP_IF) && (int32_t)ins->imm < 0;
    if(backward_branch || op == JOP_CALL || op == JOP_CALLR || op == JOP_THROW) {
      ir_push(&b, JIR_SAFEPOINT, 0, 0, 0, 0, pc);
    }

    switch(op) {
      case JOP_NOP: ir_push(&b, JIR_NOP, 0, 0, 0, 0, pc); break;
      case JOP_RET: ir_push(&b, JIR_RET, ins->a, 0, 0, 0, pc); break;
      case JOP_JMP: ir_push(&b, JIR_JMP, 0, 0, 0, ins->imm, pc); break;
      case JOP_JMP_IF: ir_push(&b, JIR_JMP_IF, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_MOV: ir_push(&b, JIR_MOV, ins->a, ins->b, 0, 0, pc); break;

      case JOP_CALL: {
        jello_jit_ir_op jop = (ins->imm == fi) ? JIR_CALL_SELF : JIR_CALL_DIRECT;
        ir_push(&b, jop, ins->a, ins->b, ins->c, ins->imm, pc);
        break;
      }
      case JOP_CALL_NATIVE:
        ir_push(&b, JIR_CALL_NATIVE, ins->a, ins->b, ins->c, ins->imm, pc);
        break;
      case JOP_CALLR: ir_push(&b, JIR_CALLR, ins->a, ins->b, ins->c, ins->imm, pc); break;
      case JOP_TRY: ir_push(&b, JIR_TRY, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_ENDTRY: ir_push(&b, JIR_ENDTRY, 0, 0, 0, 0, pc); break;
      case JOP_THROW: ir_push(&b, JIR_THROW, ins->a, 0, 0, 0, pc); break;

      case JOP_CONST_I8_IMM: ir_push(&b, JIR_CONST_I8_IMM, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_I32: ir_push(&b, JIR_CONST_I32, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_I64: ir_push(&b, JIR_CONST_I64, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_F16: ir_push(&b, JIR_CONST_F16, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_F32: ir_push(&b, JIR_CONST_F32, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_F64: ir_push(&b, JIR_CONST_F64, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_BOOL: ir_push(&b, JIR_CONST_BOOL, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_CONST_NULL: ir_push(&b, JIR_CONST_NULL, ins->a, 0, 0, 0, pc); break;
      case JOP_CONST_ATOM: ir_push(&b, JIR_CONST_ATOM, ins->a, 0, 0, ins->imm, pc); break;

      case JOP_ADD_I32:
      case JOP_SUB_I32:
      case JOP_MUL_I32:
      case JOP_DIV_I32:
      case JOP_MOD_I32:
      case JOP_SHL_I32:
      case JOP_SHR_I32:
      case JOP_BITAND_I32:
      case JOP_BITOR_I32:
      case JOP_BITXOR_I32:
        ir_push(&b, JIR_BIN_I32, ins->a, ins->b, ins->c, (uint32_t)arith_operator(op), pc);
        break;
      case JOP_ADD_I64:
      case JOP_SUB_I64:
      case JOP_MUL_I64:
      case JOP_DIV_I64:
      case JOP_MOD_I64:
      case JOP_SHL_I64:
      case JOP_SHR_I64:
      case JOP_BITAND_I64:
      case JOP_BITOR_I64:
      case JOP_BITXOR_I64:
        ir_push(&b, JIR_BIN_I64, ins->a, ins->b, ins->c, (uint32_t)arith_operator(op), pc);
        break;
      case JOP_ADD_F16:
      case JOP_SUB_F16:
      case JOP_MUL_F16:
        ir_push(&b, JIR_BIN_F16, ins->a, ins->b, ins->c, (uint32_t)arith_operator(op), pc);
        break;
      case JOP_ADD_F32:
      case JOP_SUB_F32:
      case JOP_MUL_F32:
      case JOP_DIV_F32:
        ir_push(&b, JIR_BIN_F32, ins->a, ins->b, ins->c, (uint32_t)arith_operator(op), pc);
        break;
      case JOP_ADD_F64:
      case JOP_SUB_F64:
      case JOP_MUL_F64:
      case JOP_DIV_F64:
        ir_push(&b, JIR_BIN_F64, ins->a, ins->b, ins->c, (uint32_t)arith_operator(op), pc);
        break;

      case JOP_ADD_I32_IMM: ir_push(&b, JIR_ADD_I32_IMM, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_SUB_I32_IMM: ir_push(&b, JIR_SUB_I32_IMM, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_MUL_I32_IMM: ir_push(&b, JIR_MUL_I32_IMM, ins->a, ins->b, 0, ins->imm, pc); break;

      case JOP_NEG_I32: ir_push(&b, JIR_NEG_I32, ins->a, ins->b, 0, 0, pc); break;
      case JOP_NEG_I64: ir_push(&b, JIR_NEG_I64, ins->a, ins->b, 0, 0, pc); break;
      case JOP_NEG_F32: ir_push(&b, JIR_NEG_F32, ins->a, ins->b, 0, 0, pc); break;
      case JOP_NEG_F64: ir_push(&b, JIR_NEG_F64, ins->a, ins->b, 0, 0, pc); break;
      case JOP_NOT_BOOL: ir_push(&b, JIR_NOT_BOOL, ins->a, ins->b, 0, 0, pc); break;

      case JOP_EQ_I32:
      case JOP_LT_I32:
        ir_push(&b, JIR_CMP_I32, ins->a, ins->b, ins->c, (uint32_t)cmp_operator(op), pc);
        break;
      case JOP_EQ_I64:
      case JOP_LT_I64:
        ir_push(&b, JIR_CMP_I64, ins->a, ins->b, ins->c, (uint32_t)cmp_operator(op), pc);
        break;
      case JOP_EQ_F32:
      case JOP_LT_F32:
        ir_push(&b, JIR_CMP_F32, ins->a, ins->b, ins->c, (uint32_t)cmp_operator(op), pc);
        break;
      case JOP_EQ_F64:
      case JOP_LT_F64:
        ir_push(&b, JIR_CMP_F64, ins->a, ins->b, ins->c, (uint32_t)cmp_operator(op), pc);
        break;
      case JOP_EQ_I32_IMM: ir_push(&b, JIR_EQ_I32_IMM, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_LT_I32_IMM: ir_push(&b, JIR_LT_I32_IMM, ins->a, ins->b, 0, ins->imm, pc); break;

      case JOP_SEXT_I64:
      case JOP_SEXT_I16:
      case JOP_TRUNC_I8:
      case JOP_TRUNC_I16:
      case JOP_I32_FROM_I64:
      case JOP_F64_FROM_I32:
      case JOP_I32_FROM_F64:
      case JOP_F64_FROM_I64:
      case JOP_I64_FROM_F64:
      case JOP_F32_FROM_I32:
      case JOP_I32_FROM_F32:
      case JOP_F64_FROM_F32:
      case JOP_F32_FROM_F64:
      case JOP_F32_FROM_I64:
      case JOP_I64_FROM_F32:
      case JOP_F16_FROM_F32:
      case JOP_F32_FROM_F16:
      case JOP_F16_FROM_I32:
      case JOP_I32_FROM_F16:
        ir_push(&b, JIR_CONV, ins->a, ins->b, 0, (uint32_t)conv_kind_for(op), pc);
        break;

      case JOP_TO_DYN: ir_push(&b, JIR_TO_DYN, ins->a, ins->b, 0, 0, pc); break;
      case JOP_FROM_DYN_I8:
      case JOP_FROM_DYN_I16:
      case JOP_FROM_DYN_I32:
      case JOP_FROM_DYN_I64:
      case JOP_FROM_DYN_F16:
      case JOP_FROM_DYN_F32:
      case JOP_FROM_DYN_F64:
      case JOP_FROM_DYN_BOOL:
      case JOP_FROM_DYN_ATOM:
      case JOP_FROM_DYN_PTR:
        ir_push(&b, JIR_FROM_DYN, ins->a, ins->b, 0, (uint32_t)from_dyn_target_kind(m, f, ins), pc);
        break;
      case JOP_KINDOF: ir_push(&b, JIR_KINDOF, ins->a, ins->b, 0, 0, pc); break;
      case JOP_PHYSEQ: ir_push(&b, JIR_PHYSEQ, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_IS_NULL: ir_push(&b, JIR_IS_NULL, ins->a, ins->b, 0, 0, pc); break;
      case JOP_NULL_CHECK: ir_push(&b, JIR_NULL_CHECK, ins->a, 0, 0, 0, pc); break;
      case JOP_IS_ENUM: ir_push(&b, JIR_IS_ENUM, ins->a, ins->b, 0, ins->imm, pc); break;

      case JOP_BYTES_NEW: ir_push(&b, JIR_BYTES_NEW, ins->a, ins->b, 0, 0, pc); break;
      case JOP_BYTES_LEN: ir_push(&b, JIR_BYTES_LEN, ins->a, ins->b, 0, 0, pc); break;
      case JOP_BYTES_GET_U8: ir_push(&b, JIR_BYTES_GET_U8, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_BYTES_SET_U8: ir_push(&b, JIR_BYTES_SET_U8, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_BYTES_EQ: ir_push(&b, JIR_BYTES_EQ, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_BYTES_CONCAT2: ir_push(&b, JIR_BYTES_CONCAT2, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_BYTES_BITAND2:
      case JOP_BYTES_BITOR2:
      case JOP_BYTES_BITXOR2:
        ir_push(&b, JIR_BYTES_BITOP, ins->a, ins->b, ins->c, (uint32_t)bytes_bitop_operator(op),
                pc);
        break;
      case JOP_BYTES_READ_U16_LE:
      case JOP_BYTES_READ_U16_BE:
      case JOP_BYTES_READ_U32_LE:
      case JOP_BYTES_READ_U32_BE:
      case JOP_BYTES_READ_I32_LE:
      case JOP_BYTES_READ_I32_BE:
      case JOP_BYTES_READ_F32_LE:
      case JOP_BYTES_READ_F32_BE:
      case JOP_BYTES_READ_I64_LE:
      case JOP_BYTES_READ_I64_BE:
      case JOP_BYTES_READ_F64_LE:
      case JOP_BYTES_READ_F64_BE:
        ir_push(&b, JIR_BYTES_READ, ins->a, ins->b, ins->c, (uint32_t)bytes_read_kind_for(op), pc);
        break;
      case JOP_BYTES_WRITE_U16_LE:
      case JOP_BYTES_WRITE_U16_BE:
      case JOP_BYTES_WRITE_U32_LE:
      case JOP_BYTES_WRITE_U32_BE:
      case JOP_BYTES_WRITE_I32_LE:
      case JOP_BYTES_WRITE_I32_BE:
      case JOP_BYTES_WRITE_F32_LE:
      case JOP_BYTES_WRITE_F32_BE:
      case JOP_BYTES_WRITE_I64_LE:
      case JOP_BYTES_WRITE_I64_BE:
      case JOP_BYTES_WRITE_F64_LE:
      case JOP_BYTES_WRITE_F64_BE:
        ir_push(&b, JIR_BYTES_WRITE, ins->a, ins->b, ins->c, (uint32_t)bytes_write_kind_for(op),
                pc);
        break;
      case JOP_BYTES_BLIT: ir_push(&b, JIR_BYTES_BLIT, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_BITS_WRITE: ir_push(&b, JIR_BITS_WRITE, ins->a, ins->b, ins->c, ins->imm, pc); break;
      case JOP_BYTES_FREEZE: ir_push(&b, JIR_BYTES_FREEZE, ins->a, 0, 0, 0, pc); break;
      case JOP_BITS_READ: ir_push(&b, JIR_BITS_READ, ins->a, ins->b, ins->c, ins->imm, pc); break;

      case JOP_STRUCT_NEW: ir_push(&b, JIR_STRUCT_NEW, ins->a, 0, 0, 0, pc); break;
      case JOP_STRUCT_GET: ir_push(&b, JIR_STRUCT_GET, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_STRUCT_SET: ir_push(&b, JIR_STRUCT_SET, ins->a, ins->b, 0, ins->imm, pc); break;

      case JOP_OBJ_NEW: ir_push(&b, JIR_OBJ_NEW, ins->a, 0, 0, 0, pc); break;
      case JOP_OBJ_HAS_ATOM: ir_push(&b, JIR_OBJ_HAS_ATOM, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_OBJ_GET_ATOM: ir_push(&b, JIR_OBJ_GET_ATOM, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_OBJ_SET_ATOM: ir_push(&b, JIR_OBJ_SET_ATOM, ins->a, ins->b, 0, ins->imm, pc); break;
      case JOP_OBJ_GET: ir_push(&b, JIR_OBJ_GET, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_OBJ_SET: ir_push(&b, JIR_OBJ_SET, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_OBJ_HAS: ir_push(&b, JIR_OBJ_HAS, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_OBJ_REMOVE: ir_push(&b, JIR_OBJ_REMOVE, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_OBJ_CLEAR: ir_push(&b, JIR_OBJ_CLEAR, 0, ins->b, 0, 0, pc); break;
      case JOP_OBJ_COPY: ir_push(&b, JIR_OBJ_COPY, ins->a, ins->b, 0, 0, pc); break;

      case JOP_ARRAY_LEN: ir_push(&b, JIR_ARRAY_LEN, ins->a, ins->b, 0, 0, pc); break;
      case JOP_ARRAY_GET: ir_push(&b, JIR_ARRAY_GET, ins->a, ins->b, ins->c, 0, pc); break;
      case JOP_ARRAY_SET: ir_push(&b, JIR_ARRAY_SET, ins->a, ins->b, ins->c, 0, pc); break;

      case JOP_ENUM_TAG: ir_push(&b, JIR_ENUM_TAG, ins->a, ins->b, 0, 0, pc); break;
      case JOP_ENUM_GET: ir_push(&b, JIR_ENUM_GET, ins->a, ins->b, ins->c, 0, pc); break;

      case JOP_GLOBAL_GET: ir_push(&b, JIR_GLOBAL_GET, ins->a, 0, 0, ins->imm, pc); break;
      case JOP_GLOBAL_SET: ir_push(&b, JIR_GLOBAL_SET, ins->a, 0, 0, ins->imm, pc); break;

      /* Everything else, verbatim; ops JIT code runs through their interpreter handler
       * (jello_jit_ir_slow_runs_handler), and ops that make the function decline. */
      default: ir_push(&b, JIR_SLOW, ins->a, ins->b, ins->c, ins->imm, pc); break;
    }
  }

  uint32_t nslow = 0;
  for(uint32_t i = 0; i < b.count; i++) {
    if(b.insns[i].op == (uint16_t)JIR_SLOW) nslow++;
  }

  uint32_t out_flags = JELLO_JIR_FLAG_NONE;
  if(!(flags_acc & (JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_UNIMPLEMENTED | JELLO_OPF_CAN_ALLOC))) {
    out_flags = JELLO_JIR_FLAG_NUMERIC_FAST;
  }

  jello_jit_ir_function* out = (jello_jit_ir_function*)malloc(sizeof(jello_jit_ir_function));
  if(!out) abort();
  out->nregs = f->nregs;
  out->ninsns = b.count;
  out->insns = b.insns;
  out->nslow = nslow;
  out->flags = out_flags;
  return out;
}

int jello_jit_ir_verify(const jello_jit_ir_function* f, const jello_bc_function* src) {
  for(uint32_t i = 0; i < f->ninsns; i++) {
    const jello_jit_ir_insn* ins = &f->insns[i];

    unsigned mask = jello_jit_ir_op_reg_mask((jello_jit_ir_op)ins->op);
    if((mask & JELLO_JIR_REG_A) && ins->a >= f->nregs) return 0;
    /* A call's B starts its C-register argument range, which may be empty. */
    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if(op == JIR_CALL_SELF || op == JIR_CALL_DIRECT || op == JIR_CALL_NATIVE) {
      if((uint32_t)ins->b + ins->c > f->nregs) return 0;
    } else if((mask & JELLO_JIR_REG_B) && ins->b >= f->nregs) {
      return 0;
    }
    if((mask & JELLO_JIR_REG_C) && ins->c >= f->nregs) return 0;
    if(op == JIR_CALLR && (uint64_t)ins->imm + ins->c > f->nregs) return 0;

    if(ins->bc_pc >= src->ninsns) return 0;

    if(op == JIR_TRY) {
      int64_t tgt = (int64_t)ins->bc_pc + 1 + (int32_t)ins->imm;
      if(tgt < 0 || tgt >= (int64_t)src->ninsns) return 0;
    }
  }
  return 1;
}

void jello_jit_ir_free(jello_jit_ir_function* f) {
  if(!f) return;
  free(f->insns);
  free(f);
}
