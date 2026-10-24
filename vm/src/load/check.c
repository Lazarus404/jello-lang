// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Bytecode validation; per-instruction operand and type checks against a function's typed
 * registers, then result types across calls and try-region nesting, so the interpreter and JIT
 * can trust a loaded module. */

#include <jello/internal.h>

#include <stdlib.h>

static jello_bc_result ok(void) {
  jello_bc_result r = {JELLO_BC_OK, "ok", 0};
  return r;
}

static jello_bc_result err(jello_bc_error e, const char* msg, size_t off) {
  jello_bc_result r = {e, msg, off};
  return r;
}

size_t jello_slot_size(jello_type_kind k) {
  switch(k) {
    case JELLO_T_VOID:
    case JELLO_T_BOOL:
    case JELLO_T_ATOM:
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
    case JELLO_T_F16:
    case JELLO_T_F32: return 4;
    case JELLO_T_I64:
    case JELLO_T_F64: return 8;
    case JELLO_T_DYNAMIC: return sizeof(uintptr_t);
    case JELLO_T_BYTES:
    case JELLO_T_LIST:
    case JELLO_T_ARRAY:
    case JELLO_T_STRUCT:
    case JELLO_T_OBJECT:
    case JELLO_T_FUNCTION:
    case JELLO_T_ABSTRACT:
    case JELLO_T_ENUM: return sizeof(void*);
  }
  return 0;
}

static jello_type_kind reg_kind(const jello_bc_module* m, const jello_type_id* reg_types,
                                uint32_t r) {
  return m->types[reg_types[r]].kind;
}

static int rk_ne(const jello_bc_module* m, const jello_type_id* reg_types, uint32_t r,
                 jello_type_kind want) {
  return reg_kind(m, reg_types, r) != want;
}

/** Checks whether register `r` holds an Array whose elements are of kind `want`. */
static int rk_array_of(const jello_bc_module* m, const jello_type_id* reg_types, uint32_t r,
                       jello_type_kind want) {
  return !rk_ne(m, reg_types, r, JELLO_T_ARRAY) &&
         m->types[m->types[reg_types[r]].as.unary.elem].kind == want;
}

static int is_narrow_int_kind(jello_type_kind k) {
  return k == JELLO_T_I8 || k == JELLO_T_I16 || k == JELLO_T_I32;
}

/** Whether a value of type `a` may stand where type `b` is expected; the same id, or the same
 * kind when that kind has no type parameters. List, Array, Struct, Function, Abstract and Enum
 * types must match by id. */
static int type_ids_compatible(const jello_bc_module* m, jello_type_id a, jello_type_id b) {
  if(a == b) return 1;
  jello_type_kind k = m->types[a].kind;
  if(k != m->types[b].kind) return 0;
  return k != JELLO_T_LIST && k != JELLO_T_ARRAY && k != JELLO_T_STRUCT && k != JELLO_T_FUNCTION &&
         k != JELLO_T_ABSTRACT && k != JELLO_T_ENUM;
}

static int rk_is(const jello_bc_module* m, const jello_type_id* reg_types, uint32_t r,
                 jello_type_kind want) {
  return !rk_ne(m, reg_types, r, want);
}

static int rk_is_narrow(const jello_bc_module* m, const jello_type_id* reg_types, uint32_t r) {
  return is_narrow_int_kind(reg_kind(m, reg_types, r));
}

/** Checks a = b op c, where b and c have `operand_kind`. So does a, or with `dest_may_narrow`
 * any of I8, I16 and I32; the store narrows the result. */
static jello_bc_result validate_bin(const jello_bc_module* m, const jello_type_id* reg_types,
                                    const jello_insn* ins, uint32_t nregs,
                                    jello_type_kind operand_kind, int dest_may_narrow) {
  if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) {
    return err(JELLO_BC_BAD_FORMAT, "arith reg out of range", 0);
  }
  if(dest_may_narrow ? !rk_is_narrow(m, reg_types, ins->a)
                     : !rk_is(m, reg_types, ins->a, operand_kind)) {
    return err(JELLO_BC_BAD_FORMAT, "arith dest wrong kind", 0);
  }
  if(!rk_is(m, reg_types, ins->b, operand_kind) || !rk_is(m, reg_types, ins->c, operand_kind)) {
    return err(JELLO_BC_BAD_FORMAT, "arith operand wrong kind", 0);
  }
  return ok();
}

/** Like validate_bin, for a = op b. */
static jello_bc_result validate_un(const jello_bc_module* m, const jello_type_id* reg_types,
                                   const jello_insn* ins, uint32_t nregs,
                                   jello_type_kind operand_kind, int dest_may_narrow) {
  if(ins->a >= nregs || ins->b >= nregs)
    return err(JELLO_BC_BAD_FORMAT, "arith reg out of range", 0);
  if(dest_may_narrow ? !rk_is_narrow(m, reg_types, ins->a)
                     : !rk_is(m, reg_types, ins->a, operand_kind)) {
    return err(JELLO_BC_BAD_FORMAT, "arith dest wrong kind", 0);
  }
  if(!rk_is(m, reg_types, ins->b, operand_kind))
    return err(JELLO_BC_BAD_FORMAT, "arith operand wrong kind", 0);
  return ok();
}

/** Checks a = b op imm; b has `operand_kind` and a is I8, I16 or I32. */
static jello_bc_result validate_bin_imm(const jello_bc_module* m, const jello_type_id* reg_types,
                                        const jello_insn* ins, uint32_t nregs,
                                        jello_type_kind operand_kind) {
  if(ins->a >= nregs || ins->b >= nregs)
    return err(JELLO_BC_BAD_FORMAT, "arith reg out of range", 0);
  if(!rk_is_narrow(m, reg_types, ins->a))
    return err(JELLO_BC_BAD_FORMAT, "arith dest wrong kind", 0);
  if(!rk_is(m, reg_types, ins->b, operand_kind))
    return err(JELLO_BC_BAD_FORMAT, "arith operand wrong kind", 0);
  return ok();
}

static jello_bc_result validate_cmp(const jello_bc_module* m, const jello_type_id* reg_types,
                                    const jello_insn* ins, uint32_t nregs,
                                    jello_type_kind operand_kind) {
  if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
    return err(JELLO_BC_BAD_FORMAT, "cmp reg out of range", 0);
  if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
    return err(JELLO_BC_BAD_FORMAT, "cmp dest must be Bool", 0);
  if(!rk_is(m, reg_types, ins->b, operand_kind) || !rk_is(m, reg_types, ins->c, operand_kind)) {
    return err(JELLO_BC_BAD_FORMAT, "cmp operand wrong kind", 0);
  }
  return ok();
}

static jello_bc_result validate_cmp_imm(const jello_bc_module* m, const jello_type_id* reg_types,
                                        const jello_insn* ins, uint32_t nregs,
                                        jello_type_kind operand_kind) {
  if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "cmp reg out of range", 0);
  if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
    return err(JELLO_BC_BAD_FORMAT, "cmp dest must be Bool", 0);
  if(!rk_is(m, reg_types, ins->b, operand_kind))
    return err(JELLO_BC_BAD_FORMAT, "cmp operand wrong kind", 0);
  return ok();
}

static jello_bc_result validate_conv(const jello_bc_module* m, const jello_type_id* reg_types,
                                     const jello_insn* ins, uint32_t nregs,
                                     jello_type_kind dst_kind, jello_type_kind src_kind) {
  if(ins->a >= nregs || ins->b >= nregs)
    return err(JELLO_BC_BAD_FORMAT, "conv reg out of range", 0);
  if(rk_ne(m, reg_types, ins->a, dst_kind))
    return err(JELLO_BC_BAD_FORMAT, "conv dest wrong kind", 0);
  if(rk_ne(m, reg_types, ins->b, src_kind))
    return err(JELLO_BC_BAD_FORMAT, "conv src wrong kind", 0);
  return ok();
}

static jello_bc_result validate_const(const jello_bc_module* m, const jello_type_id* reg_types,
                                      const jello_insn* ins, uint32_t nregs, jello_type_kind kind) {
  if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "const reg out of range", 0);
  if(reg_kind(m, reg_types, ins->a) != kind)
    return err(JELLO_BC_BAD_FORMAT, "const dest wrong kind", 0);
  return ok();
}

/** MOV copies a raw slot, so the two registers' types must be compatible. */
static jello_bc_result validate_mov(const jello_bc_module* m, const jello_type_id* reg_types,
                                    const jello_insn* ins) {
  jello_type_id tid_a = reg_types[ins->a];
  jello_type_id tid_b = reg_types[ins->b];
  if(tid_a == tid_b) return ok();

  if(type_ids_compatible(m, tid_a, tid_b)) return ok();

  return err(JELLO_BC_BAD_FORMAT, "mov type mismatch", 0);
}

/** CALL and TAILCALL: a = function imm applied to registers b to b + c - 1, each compatible
 * with the callee's register of the same index. `check_dest` is 0 for TAILCALL, which has
 * no destination. */
static jello_bc_result validate_direct_call(const jello_bc_module* m,
                                            const jello_type_id* reg_types, const jello_insn* ins,
                                            uint32_t nregs, uint32_t nfuncs, int check_dest) {
  if(ins->imm >= nfuncs) return err(JELLO_BC_BAD_FORMAT, "function index out of range", 0);
  if(check_dest && ins->a >= nregs)
    return err(JELLO_BC_BAD_FORMAT, "call dest reg out of range", 0);
  if((uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
    return err(JELLO_BC_BAD_FORMAT, "call arg regs out of range", 0);
  }
  {
    const jello_bc_function* callee = &m->funcs[ins->imm];
    if(ins->c > callee->nregs)
      return err(JELLO_BC_BAD_FORMAT, "call passes more args than callee has registers", 0);
    for(uint32_t i = 0; i < ins->c; i++) {
      if(!type_ids_compatible(m, reg_types[ins->b + i], callee->reg_types[i])) {
        return err(JELLO_BC_BAD_FORMAT, "call arg type mismatch", 0);
      }
    }
  }
  return ok();
}

/** CALLR and TAILCALLR; a = Function register b applied to registers imm to imm + c - 1,
 * which must match its signature. `check_dest` is 0 for TAILCALLR. */
static jello_bc_result validate_callr_window(const jello_bc_module* m,
                                             const jello_type_id* reg_types, const jello_insn* ins,
                                             uint32_t nregs, int check_dest) {
  if(check_dest && ins->a >= nregs)
    return err(JELLO_BC_BAD_FORMAT, "callr dest reg out of range", 0);
  if(ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "callr callee reg out of range", 0);
  if(rk_ne(m, reg_types, ins->b, JELLO_T_FUNCTION)) {
    return err(JELLO_BC_BAD_FORMAT, "callr callee must be Function", 0);
  }
  if((uint64_t)ins->imm + (uint64_t)ins->c > (uint64_t)nregs) {
    return err(JELLO_BC_BAD_FORMAT, "callr arg window out of range", 0);
  }
  {
    const jello_fun_sig* sig = &m->sigs[m->types[reg_types[ins->b]].as.fun.sig_id];
    if(ins->c != sig->nargs) return err(JELLO_BC_BAD_FORMAT, "callr arg count mismatch", 0);
    for(uint32_t i = 0; i < ins->c; i++) {
      if(!type_ids_compatible(m, reg_types[ins->imm + i], sig->args[i])) {
        return err(JELLO_BC_BAD_FORMAT, "callr arg type mismatch", 0);
      }
    }
  }
  return ok();
}

/** Checks the branch target `tgt` of the instruction at `pc`; an instruction of the function
 * (`range_msg` otherwise), at or after `lo`, and not a row of a switch_kind table. Every CASE_KIND
 * is such a row, since validation rejects any other. */
static jello_bc_result check_target(const jello_insn* ins, uint32_t pc, int64_t tgt, int64_t lo,
                                    uint32_t ninsns, const char* range_msg) {
  if(tgt < 0 || tgt >= (int64_t)ninsns) return err(JELLO_BC_BAD_FORMAT, range_msg, 0);
  if(tgt < lo) return err(JELLO_BC_BAD_FORMAT, "switch_kind target must follow its table", 0);
  if(ins[tgt - (int64_t)pc].op == (uint8_t)JOP_CASE_KIND) {
    return err(JELLO_BC_BAD_FORMAT, "jump target inside a switch_kind table", 0);
  }
  return ok();
}

jello_bc_result jello_bc_validate_insn(const jello_bc_module* m, const jello_type_id* reg_types,
                                       const jello_insn* ins, uint32_t nregs, uint32_t pc,
                                       uint32_t ninsns, uint32_t nfuncs) {
  const uint8_t op = ins->op;

  switch((jello_op)op) {
    case JOP_NOP: return ok();

    case JOP_RET:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "ret reg out of range", 0);
      return ok();

    case JOP_MOV:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "mov reg out of range", 0);
      return validate_mov(m, reg_types, ins);

    /* A jump target must be an instruction. One past the end is rejected too; every function
     * ends in a terminator (loader.c), so nothing may run off the end. */
    case JOP_JMP:
      return check_target(ins, pc, (int64_t)pc + 1 + (int32_t)ins->imm, 0, ninsns,
                          "jmp target out of range");

    case JOP_JMP_IF: {
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "jmp_if cond reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "jmp_if cond must be bool", 0);
      return check_target(ins, pc, (int64_t)pc + 1 + (int32_t)ins->imm, 0, ninsns,
                          "jmp_if target out of range");
    }

    case JOP_ASSERT: return err(JELLO_BC_BAD_FORMAT, "assert opcode not implemented", 0);

    case JOP_CALL: return validate_direct_call(m, reg_types, ins, nregs, nfuncs, 1);

    case JOP_TAILCALL: return validate_direct_call(m, reg_types, ins, nregs, nfuncs, 0);

    case JOP_CALLR: return validate_callr_window(m, reg_types, ins, nregs, 1);

    case JOP_TAILCALLR: return validate_callr_window(m, reg_types, ins, nregs, 0);

    case JOP_CONST_FUN:
    case JOP_CLOSURE: {
      if(ins->imm >= nfuncs) return err(JELLO_BC_BAD_FORMAT, "function index out of range", 0);
      if((jello_op)op == JOP_CONST_FUN && ins->c != 0) {
        return err(JELLO_BC_BAD_FORMAT, "const_fun capture count must be 0", 0);
      }
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "closure dest reg out of range", 0);
      if((uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "closure capture window out of range", 0);
      }
      for(uint32_t i = 0; i < ins->c; i++) {
        if(reg_kind(m, reg_types, ins->b + i) != JELLO_T_DYNAMIC) {
          return err(JELLO_BC_BAD_FORMAT, "closure capture must be Dynamic", 0);
        }
      }
      {
        const jello_bc_function* callee = &m->funcs[ins->imm];
        if(callee->cap_start + (uint32_t)ins->c > callee->nregs) {
          return err(JELLO_BC_BAD_FORMAT, "closure cap_start out of range", 0);
        }
      }
      return ok();
    }

    case JOP_CONST_I8_IMM:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "const reg out of range", 0);
      if(!rk_is_narrow(m, reg_types, ins->a))
        return err(JELLO_BC_BAD_FORMAT, "const dest wrong kind", 0);
      return ok();
    case JOP_CONST_I32: return validate_const(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_CONST_I64:
      if(ins->imm >= m->nconst_i64)
        return err(JELLO_BC_BAD_FORMAT, "const_i64 index out of range", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_I64);
    case JOP_CONST_F16: return validate_const(m, reg_types, ins, nregs, JELLO_T_F16);
    case JOP_CONST_F32: return validate_const(m, reg_types, ins, nregs, JELLO_T_F32);
    case JOP_CONST_F64:
      if(ins->imm >= m->nconst_f64)
        return err(JELLO_BC_BAD_FORMAT, "const_f64 index out of range", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_F64);
    case JOP_CONST_BOOL:
      if(ins->imm > 1) return err(JELLO_BC_BAD_FORMAT, "const_bool imm must be 0/1", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_BOOL);
    /* CONST_NULL, IS_NULL and NULL_CHECK take a register that can hold null;
     * a Dynamic or a reference. */
    case JOP_CONST_NULL:
    case JOP_NULL_CHECK:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "null op reg out of range", 0);
      if(!vm_kind_needs_gc_scan(reg_kind(m, reg_types, ins->a)))
        return err(JELLO_BC_BAD_FORMAT, "null op register must be Dynamic or a reference", 0);
      return ok();
    case JOP_IS_NULL:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "is_null reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "is_null dst must be Bool", 0);
      if(!vm_kind_needs_gc_scan(reg_kind(m, reg_types, ins->b)))
        return err(JELLO_BC_BAD_FORMAT, "null op register must be Dynamic or a reference", 0);
      return ok();
    case JOP_IS_ENUM:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "is_enum reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "is_enum dst must be Bool", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC)
        return err(JELLO_BC_BAD_FORMAT, "is_enum src must be Dynamic", 0);
      if(ins->imm >= m->ntypes || m->types[ins->imm].kind != JELLO_T_ENUM)
        return err(JELLO_BC_BAD_FORMAT, "is_enum type must be an enum", 0);
      return ok();
    /* BYTES_SLICE a, b, c: the start and length are registers c and c + 1. */
    case JOP_BYTES_SLICE:
      if(ins->a >= nregs || ins->b >= nregs || (uint64_t)ins->c + 1u >= (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_slice reg out of range", 0);
      }
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_slice operands must be Bytes", 0);
      }
      if(rk_ne(m, reg_types, ins->c, JELLO_T_I32) ||
         rk_ne(m, reg_types, ins->c + 1u, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_slice start and length must be I32", 0);
      }
      return ok();
    case JOP_GLOBAL_GET:
    case JOP_GLOBAL_SET:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "global reg out of range", 0);
      if(ins->imm >= m->nglobals) return err(JELLO_BC_BAD_FORMAT, "global index out of range", 0);
      if(!type_ids_compatible(m, reg_types[ins->a], m->global_types[ins->imm])) {
        return err(JELLO_BC_BAD_FORMAT, "global type mismatch", 0);
      }
      return ok();
    case JOP_CONST_BYTES: {
      jello_bc_result r = validate_const(m, reg_types, ins, nregs, JELLO_T_BYTES);
      if(!r.err && ins->imm >= m->nconst_bytes)
        return err(JELLO_BC_BAD_FORMAT, "const_bytes index out of range", 0);
      return r;
    }
    case JOP_CONST_ATOM:
      if(ins->imm >= m->natoms)
        return err(JELLO_BC_BAD_FORMAT, "const_atom atom id out of range", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_ATOM);

    case JOP_ADD_I32:
    case JOP_SUB_I32:
    case JOP_MUL_I32:
    case JOP_SHL_I32:
    case JOP_SHR_I32:
    case JOP_BITAND_I32:
    case JOP_BITOR_I32:
    case JOP_BITXOR_I32: return validate_bin(m, reg_types, ins, nregs, JELLO_T_I32, 1);
    case JOP_DIV_I32:
    case JOP_MOD_I32: return validate_bin(m, reg_types, ins, nregs, JELLO_T_I32, 1);
    case JOP_DIV_U32:
    case JOP_MOD_U32: return validate_bin(m, reg_types, ins, nregs, JELLO_T_I32, 1);
    case JOP_ADD_I32_IMM:
    case JOP_SUB_I32_IMM:
    case JOP_MUL_I32_IMM: return validate_bin_imm(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_NEG_I32: return validate_un(m, reg_types, ins, nregs, JELLO_T_I32, 1);

    case JOP_ADD_I64:
    case JOP_SUB_I64:
    case JOP_MUL_I64:
    case JOP_DIV_I64:
    case JOP_MOD_I64:
    case JOP_DIV_U64:
    case JOP_MOD_U64:
    case JOP_SHL_I64:
    case JOP_SHR_I64:
    case JOP_BITAND_I64:
    case JOP_BITOR_I64:
    case JOP_BITXOR_I64: return validate_bin(m, reg_types, ins, nregs, JELLO_T_I64, 0);
    case JOP_NEG_I64: return validate_un(m, reg_types, ins, nregs, JELLO_T_I64, 0);

    case JOP_ADD_F16:
    case JOP_SUB_F16:
    case JOP_MUL_F16: return validate_bin(m, reg_types, ins, nregs, JELLO_T_F16, 0);
    case JOP_ADD_F32:
    case JOP_SUB_F32:
    case JOP_MUL_F32:
    case JOP_DIV_F32: return validate_bin(m, reg_types, ins, nregs, JELLO_T_F32, 0);
    case JOP_NEG_F32: return validate_un(m, reg_types, ins, nregs, JELLO_T_F32, 0);
    case JOP_ADD_F64:
    case JOP_SUB_F64:
    case JOP_MUL_F64:
    case JOP_DIV_F64: return validate_bin(m, reg_types, ins, nregs, JELLO_T_F64, 0);
    case JOP_NEG_F64: return validate_un(m, reg_types, ins, nregs, JELLO_T_F64, 0);
    case JOP_FMA_F64: {
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs || ins->imm >= nregs) {
        return err(JELLO_BC_BAD_FORMAT, "fma_f64 reg out of range", 0);
      }
      if(!rk_is(m, reg_types, ins->a, JELLO_T_F64) || !rk_is(m, reg_types, ins->b, JELLO_T_F64) ||
         !rk_is(m, reg_types, ins->c, JELLO_T_F64) ||
         !rk_is(m, reg_types, (uint8_t)ins->imm, JELLO_T_F64)) {
        return err(JELLO_BC_BAD_FORMAT, "fma_f64 operand wrong kind", 0);
      }
      return ok();
    }
    case JOP_LDEXP_F64: {
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) {
        return err(JELLO_BC_BAD_FORMAT, "ldexp_f64 reg out of range", 0);
      }
      if(!rk_is(m, reg_types, ins->a, JELLO_T_F64) || !rk_is(m, reg_types, ins->b, JELLO_T_F64) ||
         !rk_is(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "ldexp_f64 operand wrong kind", 0);
      }
      return ok();
    }
    case JOP_COPYSIGN_F64: return validate_bin(m, reg_types, ins, nregs, JELLO_T_F64, 0);
    case JOP_NOT_BOOL: return validate_un(m, reg_types, ins, nregs, JELLO_T_BOOL, 0);

    case JOP_EQ_I32:
    case JOP_LT_I32:
    case JOP_LT_U32: return validate_cmp(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_EQ_I32_IMM:
    case JOP_LT_I32_IMM: return validate_cmp_imm(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_EQ_I64:
    case JOP_LT_I64:
    case JOP_LT_U64: return validate_cmp(m, reg_types, ins, nregs, JELLO_T_I64);
    case JOP_EQ_F32:
    case JOP_LT_F32: return validate_cmp(m, reg_types, ins, nregs, JELLO_T_F32);
    case JOP_EQ_F64:
    case JOP_LT_F64: return validate_cmp(m, reg_types, ins, nregs, JELLO_T_F64);

    /* SEXT_I16 widens I16 back to I32, the inverse of TRUNC_I16; there is no SEXT_I8. */
    case JOP_SEXT_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_I32);
    case JOP_ZEXT_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_I32);
    case JOP_SEXT_I16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_I16);
    case JOP_TRUNC_I8: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I8, JELLO_T_I32);
    case JOP_TRUNC_I16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I16, JELLO_T_I32);
    case JOP_I32_FROM_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_I64);
    case JOP_F64_FROM_I32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F64, JELLO_T_I32);
    case JOP_I32_FROM_F64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_F64);
    case JOP_F64_FROM_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F64, JELLO_T_I64);
    case JOP_I64_FROM_F64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_F64);
    case JOP_F32_FROM_I32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_I32);
    case JOP_I32_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_F32);
    case JOP_F64_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F64, JELLO_T_F32);
    case JOP_F32_FROM_F64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_F64);
    case JOP_F32_FROM_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_I64);
    case JOP_I64_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_F32);
    case JOP_F16_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F16, JELLO_T_F32);
    case JOP_F32_FROM_F16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_F16);
    case JOP_F16_FROM_I32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F16, JELLO_T_I32);
    case JOP_I32_FROM_F16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_F16);

    case JOP_TO_DYN:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "to_dyn reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_DYNAMIC)
        return err(JELLO_BC_BAD_FORMAT, "to_dyn dst must be Dynamic", 0);
      return ok();
    case JOP_FROM_DYN_I8:
    case JOP_FROM_DYN_I16:
    case JOP_FROM_DYN_I32:
    case JOP_FROM_DYN_I64:
    case JOP_FROM_DYN_F16:
    case JOP_FROM_DYN_F32:
    case JOP_FROM_DYN_F64:
    case JOP_FROM_DYN_BOOL:
    case JOP_FROM_DYN_ATOM: {
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "from_dyn reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC)
        return err(JELLO_BC_BAD_FORMAT, "from_dyn src must be Dynamic", 0);
      jello_type_kind want;
      switch((jello_op)op) {
        case JOP_FROM_DYN_I8: want = JELLO_T_I8; break;
        case JOP_FROM_DYN_I16: want = JELLO_T_I16; break;
        case JOP_FROM_DYN_I32: want = JELLO_T_I32; break;
        case JOP_FROM_DYN_I64: want = JELLO_T_I64; break;
        case JOP_FROM_DYN_F16: want = JELLO_T_F16; break;
        case JOP_FROM_DYN_F32: want = JELLO_T_F32; break;
        case JOP_FROM_DYN_F64: want = JELLO_T_F64; break;
        case JOP_FROM_DYN_BOOL: want = JELLO_T_BOOL; break;
        default: want = JELLO_T_ATOM; break;
      }
      if(rk_ne(m, reg_types, ins->a, want))
        return err(JELLO_BC_BAD_FORMAT, "from_dyn dst wrong kind", 0);
      return ok();
    }
    case JOP_FROM_DYN_PTR: {
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "from_dyn_ptr reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC)
        return err(JELLO_BC_BAD_FORMAT, "from_dyn_ptr src must be Dynamic", 0);
      jello_type_kind dk = reg_kind(m, reg_types, ins->a);
      if(vm_kind_is_gc_pointer(dk)) return ok();
      return err(JELLO_BC_BAD_FORMAT, "from_dyn_ptr dst must be a pointer kind", 0);
    }
    case JOP_KINDOF:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "kindof reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32))
        return err(JELLO_BC_BAD_FORMAT, "kindof dst must be I32", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC)
        return err(JELLO_BC_BAD_FORMAT, "kindof src must be Dynamic", 0);
      return ok();
    case JOP_PHYSEQ:
      /* b and c may have any kind; PHYSEQ boxes each by its declared kind before comparing. */
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "physeq reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "physeq dst must be Bool", 0);
      return ok();

    case JOP_STRUCT_NEW:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "struct_new reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_STRUCT))
        return err(JELLO_BC_BAD_FORMAT, "struct_new dst must be Struct", 0);
      return ok();

    case JOP_STRUCT_GET:
    case JOP_STRUCT_SET: {
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "struct reg out of range", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_STRUCT))
        return err(JELLO_BC_BAD_FORMAT, "struct op target must be Struct", 0);
      uint32_t sig_id = m->types[reg_types[ins->b]].as.strct.struct_sig_id;
      if(sig_id >= m->nstruct_sigs)
        return err(JELLO_BC_BAD_FORMAT, "struct sig id out of range", 0);
      const jello_struct_sig* sig = &m->struct_sigs[sig_id];
      if(ins->imm >= sig->nfields)
        return err(JELLO_BC_BAD_FORMAT, "struct field index out of range", 0);
      if((jello_op)op == JOP_STRUCT_SET && !sig->is_mutable) {
        return err(JELLO_BC_BAD_FORMAT, "struct_set on immutable struct", 0);
      }
      /* The value register's type must equal the field's declared type. */
      if(reg_types[ins->a] != sig->field_type[ins->imm]) {
        return err(JELLO_BC_BAD_FORMAT, "struct field type mismatch", 0);
      }
      return ok();
    }

    /* The _ATOM opcodes take the key as an atom id in imm; OBJ_GET, OBJ_SET, OBJ_HAS and
     * OBJ_REMOVE take it in an Atom register c. The value register's kind is not checked; an
     * object field has no static type. */
    case JOP_OBJ_NEW:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_new reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_new dst must be Object", 0);
      return ok();
    case JOP_OBJ_HAS_ATOM:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_has_atom reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "obj_has_atom dst must be Bool", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT)
        return err(JELLO_BC_BAD_FORMAT, "obj_has_atom src must be Object", 0);
      if(ins->imm >= m->natoms)
        return err(JELLO_BC_BAD_FORMAT, "obj_has_atom atom id out of range", 0);
      return ok();
    case JOP_OBJ_GET_ATOM:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_get_atom reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT)
        return err(JELLO_BC_BAD_FORMAT, "obj_get_atom src must be Object", 0);
      if(ins->imm >= m->natoms)
        return err(JELLO_BC_BAD_FORMAT, "obj_get_atom atom id out of range", 0);
      return ok();
    case JOP_OBJ_SET_ATOM:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_set_atom reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT)
        return err(JELLO_BC_BAD_FORMAT, "obj_set_atom dst must be Object", 0);
      if(ins->imm >= m->natoms)
        return err(JELLO_BC_BAD_FORMAT, "obj_set_atom atom id out of range", 0);
      return ok();
    case JOP_OBJ_GET:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_get reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT)
        return err(JELLO_BC_BAD_FORMAT, "obj_get src must be Object", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_ATOM))
        return err(JELLO_BC_BAD_FORMAT, "obj_get key must be Atom", 0);
      return ok();
    case JOP_OBJ_SET:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_set reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT)
        return err(JELLO_BC_BAD_FORMAT, "obj_set dst must be Object", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_ATOM))
        return err(JELLO_BC_BAD_FORMAT, "obj_set key must be Atom", 0);
      return ok();
    case JOP_OBJ_HAS:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_has reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "obj_has dst must be Bool", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_has src must be Object", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_ATOM))
        return err(JELLO_BC_BAD_FORMAT, "obj_has key must be Atom", 0);
      return ok();
    case JOP_OBJ_REMOVE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_remove reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "obj_remove dst must be Bool", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_remove src must be Object", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_ATOM))
        return err(JELLO_BC_BAD_FORMAT, "obj_remove key must be Atom", 0);
      return ok();
    case JOP_OBJ_CLEAR:
      if(ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_clear reg out of range", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_clear src must be Object", 0);
      return ok();
    case JOP_OBJ_COPY:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_copy reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_copy dst must be Object", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_copy src must be Object", 0);
      return ok();
    case JOP_OBJ_KEYS:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "obj_keys reg out of range", 0);
      if(!rk_array_of(m, reg_types, ins->a, JELLO_T_ATOM)) {
        return err(JELLO_BC_BAD_FORMAT, "obj_keys dst must be Array<Atom>", 0);
      }
      if(rk_ne(m, reg_types, ins->b, JELLO_T_OBJECT))
        return err(JELLO_BC_BAD_FORMAT, "obj_keys src must be Object", 0);
      return ok();

    /* Null and bounds checks on Bytes happen at run time. */
    case JOP_BYTES_NEW:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_new reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES) || rk_ne(m, reg_types, ins->b, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_new types required", 0);
      }
      return ok();
    case JOP_BYTES_FREEZE:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_freeze reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_freeze types required", 0);
      }
      return ok();
    case JOP_BYTES_LEN:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_len reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_len types required", 0);
      }
      return ok();
    case JOP_BYTES_GET_U8:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_get_u8 reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_get_u8 types required", 0);
      }
      return ok();
    case JOP_BYTES_SET_U8:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_set_u8 reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_set_u8 types required", 0);
      }
      return ok();

    case JOP_BYTES_CONCAT2:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_concat2 reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_concat2 types required", 0);
      }
      return ok();
    /* BYTES_CONCAT_MANY joins the c Bytes in the window from b into a. */
    case JOP_BYTES_CONCAT_MANY:
      if(ins->a >= nregs || (uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_concat_many reg out of range", 0);
      }
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES))
        return err(JELLO_BC_BAD_FORMAT, "bytes_concat_many dest must be Bytes", 0);
      for(uint32_t i = 0; i < ins->c; i++) {
        if(rk_ne(m, reg_types, ins->b + i, JELLO_T_BYTES))
          return err(JELLO_BC_BAD_FORMAT, "bytes_concat_many part must be Bytes", 0);
      }
      return ok();
    /* BYTES_JOIN joins the Array<Bytes> in b, with the Bytes in c between each two parts. */
    case JOP_BYTES_JOIN:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_join reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES) ||
         !rk_array_of(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_join types required", 0);
      }
      return ok();
    case JOP_BYTES_EQ:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_eq reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_eq types required", 0);
      }
      return ok();
    case JOP_BYTES_BITAND2:
    case JOP_BYTES_BITOR2:
    case JOP_BYTES_BITXOR2:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes bitwise2 reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_BYTES)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_bitwise2 types required", 0);
      }
      return ok();
    /* BYTES_READ_* and BYTES_WRITE_*; a is the value, of the kind the name gives, b the Bytes
     * and c an I32 byte offset. Offsets need no alignment; wire formats place fields anywhere. */
    case JOP_BYTES_READ_U16_LE:
    case JOP_BYTES_READ_U16_BE:
    case JOP_BYTES_READ_U32_LE:
    case JOP_BYTES_READ_U32_BE:
    case JOP_BYTES_READ_I32_LE:
    case JOP_BYTES_READ_I32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_F32_LE:
    case JOP_BYTES_READ_F32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_F32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read_f32 types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_I64_LE:
    case JOP_BYTES_READ_I64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I64) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read_i64 types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_F64_LE:
    case JOP_BYTES_READ_F64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_F64) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read_f64 types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_U16_LE:
    case JOP_BYTES_WRITE_U16_BE:
    case JOP_BYTES_WRITE_U32_LE:
    case JOP_BYTES_WRITE_U32_BE:
    case JOP_BYTES_WRITE_I32_LE:
    case JOP_BYTES_WRITE_I32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_F32_LE:
    case JOP_BYTES_WRITE_F32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_F32) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write_f32 types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_I64_LE:
    case JOP_BYTES_WRITE_I64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I64) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write_i64 types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_F64_LE:
    case JOP_BYTES_WRITE_F64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_F64) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write_f64 types required", 0);
      }
      return ok();
    /* BYTES_BLIT copies all of Bytes b into Bytes a at the I32 byte offset c. */
    case JOP_BYTES_BLIT:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bytes_blit reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BYTES) || rk_ne(m, reg_types, ins->b, JELLO_T_BYTES) ||
         rk_ne(m, reg_types, ins->c, JELLO_T_I32)) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_blit types required", 0);
      }
      return ok();

    /* The loader has matched each import's declared signature against its `.jdll` export. Here
     * the instruction must match the declaration; c arguments from register b on, of the
     * declared kinds, and a destination a of the result kind, or Dynamic for a Void result,
     * which stores null there. */
    case JOP_CALL_NATIVE: {
      if(ins->imm >= m->nimports) return err(JELLO_BC_BAD_FORMAT, "import index out of range", 0);
      const jello_import_sig* sig = &m->imports[ins->imm];
      if(ins->c != sig->arity) return err(JELLO_BC_BAD_FORMAT, "call_native arg count mismatch", 0);
      if((uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "call_native arg regs out of range", 0);
      }
      for(uint32_t i = 0; i < ins->c; i++) {
        jello_type_kind ak = reg_kind(m, reg_types, ins->b + i);
        if(ak != (jello_type_kind)sig->arg_kinds[i]) {
          return err(JELLO_BC_BAD_FORMAT, "call_native arg type mismatch", 0);
        }
      }
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "call_native dest reg out of range", 0);
      jello_type_kind want = (jello_type_kind)sig->ret_kind;
      if(want == JELLO_T_VOID) want = JELLO_T_DYNAMIC;
      if(reg_kind(m, reg_types, ins->a) != want) {
        return err(JELLO_BC_BAD_FORMAT, "call_native dest wrong kind", 0);
      }
      return ok();
    }

    /* BITS_WRITE stores I32 b into Bytes a; BITS_READ loads Bytes b into I32 a. Either way c is
     * the width in bits and imm the bit offset. */
    case JOP_BITS_WRITE:
    case JOP_BITS_READ: {
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "bits op reg out of range", 0);
      if(ins->c < 1 || ins->c > 32)
        return err(JELLO_BC_BAD_FORMAT, "bits op width must be 1-32", 0);
      uint32_t bytes_reg = ((jello_op)op == JOP_BITS_WRITE) ? ins->a : ins->b;
      uint32_t i32_reg = ((jello_op)op == JOP_BITS_WRITE) ? ins->b : ins->a;
      if(rk_ne(m, reg_types, bytes_reg, JELLO_T_BYTES))
        return err(JELLO_BC_BAD_FORMAT, "bits op Bytes operand wrong kind", 0);
      if(rk_ne(m, reg_types, i32_reg, JELLO_T_I32))
        return err(JELLO_BC_BAD_FORMAT, "bits op I32 operand wrong kind", 0);
      return ok();
    }

    case JOP_TRY: {
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "try reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_DYNAMIC) {
        return err(JELLO_BC_BAD_FORMAT, "try dest must be Dynamic", 0);
      }
      return check_target(ins, pc, (int64_t)pc + 1 + (int32_t)ins->imm, 0, ninsns,
                          "try handler out of range");
    }

    case JOP_ENDTRY: return ok();

    case JOP_THROW:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "throw reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_DYNAMIC) {
        return err(JELLO_BC_BAD_FORMAT, "throw payload must be Dynamic", 0);
      }
      return ok();

    /* SWITCH_KIND a, b; b CASE_KIND rows follow it. Each row's target and the default target
     * (imm) are relative to the instruction after the table and may not precede it, so a switch
     * is never a loop back-edge. */
    case JOP_SWITCH_KIND: {
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "switch_kind reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_DYNAMIC) {
        return err(JELLO_BC_BAD_FORMAT, "switch_kind subject must be Dynamic", 0);
      }
      int64_t after = (int64_t)pc + 1 + ins->b;
      if(after > (int64_t)ninsns)
        return err(JELLO_BC_BAD_FORMAT, "switch_kind table out of range", 0);
      for(uint32_t i = 1; i <= ins->b; i++) {
        if(ins[i].op != (uint8_t)JOP_CASE_KIND)
          return err(JELLO_BC_BAD_FORMAT, "switch_kind row must be CASE_KIND", 0);
        jello_bc_result r = check_target(ins, pc, after + (int32_t)ins[i].imm, after, ninsns,
                                         "switch_kind case target out of range");
        if(r.err) return r;
      }
      return check_target(ins, pc, after + (int32_t)ins->imm, after, ninsns,
                          "switch_kind default target out of range");
    }

    case JOP_SPILL_PUSH:
    case JOP_SPILL_POP:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "spill reg out of range", 0);
      if(ins->imm >= JELLO_SPILL_SLOTS_MAX)
        return err(JELLO_BC_BAD_FORMAT, "spill slot out of range", 0);
      return ok();

    case JOP_ARRAY_NEW:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "array_new reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_ARRAY))
        return err(JELLO_BC_BAD_FORMAT, "array_new dest must be Array", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_I32))
        return err(JELLO_BC_BAD_FORMAT, "array_new length must be I32", 0);
      return ok();

    case JOP_ARRAY_LEN:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "array_len reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_I32))
        return err(JELLO_BC_BAD_FORMAT, "array_len dest must be I32", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_ARRAY))
        return err(JELLO_BC_BAD_FORMAT, "array_len src must be Array", 0);
      return ok();

    case JOP_ARRAY_GET:
    case JOP_ARRAY_SET:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) {
        return err(JELLO_BC_BAD_FORMAT, "array_get/set reg out of range", 0);
      }
      if(rk_ne(m, reg_types, ins->b, JELLO_T_ARRAY))
        return err(JELLO_BC_BAD_FORMAT, "array_get/set src must be Array", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_I32))
        return err(JELLO_BC_BAD_FORMAT, "array_get/set index must be I32", 0);
      if(reg_types[ins->a] != m->types[reg_types[ins->b]].as.unary.elem) {
        return err(JELLO_BC_BAD_FORMAT, "array_get/set element type mismatch", 0);
      }
      return ok();

    case JOP_ARRAY_RESIZE:
      if(ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "array_resize reg out of range", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_ARRAY))
        return err(JELLO_BC_BAD_FORMAT, "array_resize target must be Array", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_I32))
        return err(JELLO_BC_BAD_FORMAT, "array_resize length must be I32", 0);
      return ok();

    case JOP_ARRAY_CONCAT2:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) {
        return err(JELLO_BC_BAD_FORMAT, "array_concat2 reg out of range", 0);
      }
      if(rk_ne(m, reg_types, ins->a, JELLO_T_ARRAY))
        return err(JELLO_BC_BAD_FORMAT, "array_concat2 dst must be Array", 0);
      if(reg_types[ins->b] != reg_types[ins->a] || reg_types[ins->c] != reg_types[ins->a]) {
        return err(JELLO_BC_BAD_FORMAT, "array_concat2 operands must have the result's array type",
                   0);
      }
      return ok();

    case JOP_LIST_NIL:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "list_nil reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_nil dst must be List", 0);
      return ok();

    case JOP_LIST_CONS:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) {
        return err(JELLO_BC_BAD_FORMAT, "list_cons reg out of range", 0);
      }
      if(rk_ne(m, reg_types, ins->a, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_cons dst must be List", 0);
      if(rk_ne(m, reg_types, ins->c, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_cons tail must be List", 0);
      if(reg_types[ins->a] != reg_types[ins->c]) {
        return err(JELLO_BC_BAD_FORMAT, "list_cons list type mismatch", 0);
      }
      if(m->types[reg_types[ins->a]].as.unary.elem != reg_types[ins->b]) {
        return err(JELLO_BC_BAD_FORMAT, "list_cons head type mismatch", 0);
      }
      return ok();

    case JOP_LIST_HEAD:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "list_head reg out of range", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_head src must be List", 0);
      if(m->types[reg_types[ins->b]].as.unary.elem != reg_types[ins->a]) {
        return err(JELLO_BC_BAD_FORMAT, "list_head dst type mismatch", 0);
      }
      return ok();

    case JOP_LIST_TAIL:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "list_tail reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_tail dst must be List", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_tail src must be List", 0);
      if(reg_types[ins->a] != reg_types[ins->b]) {
        return err(JELLO_BC_BAD_FORMAT, "list_tail list type mismatch", 0);
      }
      return ok();

    case JOP_LIST_IS_NIL:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "list_is_nil reg out of range", 0);
      if(rk_ne(m, reg_types, ins->a, JELLO_T_BOOL))
        return err(JELLO_BC_BAD_FORMAT, "list_is_nil dst must be Bool", 0);
      if(rk_ne(m, reg_types, ins->b, JELLO_T_LIST))
        return err(JELLO_BC_BAD_FORMAT, "list_is_nil src must be List", 0);
      return ok();

    case JOP_ENUM_NEW:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "enum_new dest reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_ENUM)
        return err(JELLO_BC_BAD_FORMAT, "enum_new dest must be Enum", 0);
      if((uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "enum_new payload regs out of range", 0);
      }
      if(m->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA) {
        const jello_enum_sig* sig = &m->enum_sigs[m->types[reg_types[ins->a]].as.enm.enum_sig_id];
        if(ins->imm >= sig->nvariants)
          return err(JELLO_BC_BAD_FORMAT, "enum_new tag out of range", 0);
        const jello_enum_variant_sig* variant = &sig->variants[ins->imm];
        if(ins->c != variant->nfields)
          return err(JELLO_BC_BAD_FORMAT, "enum_new field count mismatch", 0);
        for(uint16_t i = 0; i < variant->nfields; i++) {
          if(reg_types[(uint32_t)ins->b + i] != variant->field_type[i])
            return err(JELLO_BC_BAD_FORMAT, "enum_new field type mismatch", 0);
        }
      }
      return ok();
    case JOP_ENUM_TAG:
      if(ins->a >= nregs || ins->b >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "enum_tag reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32)
        return err(JELLO_BC_BAD_FORMAT, "enum_tag dest must be I32", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_ENUM)
        return err(JELLO_BC_BAD_FORMAT, "enum_tag src must be Enum", 0);
      return ok();
    case JOP_ENUM_GET:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs)
        return err(JELLO_BC_BAD_FORMAT, "enum_get reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_ENUM)
        return err(JELLO_BC_BAD_FORMAT, "enum_get src must be Enum", 0);
      if(reg_kind(m, reg_types, ins->c) != JELLO_T_I32)
        return err(JELLO_BC_BAD_FORMAT, "enum_get index must be I32", 0);
      return ok();
    case JOP_BIND_THIS: return err(JELLO_BC_BAD_FORMAT, "opcode not implemented", 0);

    /* A table's rows are checked by their SWITCH_KIND, so a CASE_KIND checked on its own is a row
     * outside a table. */
    case JOP_CASE_KIND: return err(JELLO_BC_BAD_FORMAT, "case_kind outside a switch_kind table", 0);

    default: return ok();
  }
}

/** The result entry of a function with no RET that tail calls nothing that returns (it only
 * throws or loops); it constrains nothing. */
#define NO_RESULT ((jello_type_id)UINT32_MAX)

static int result_ok(const jello_bc_module* m, jello_type_id expected, jello_type_id actual) {
  return expected == NO_RESULT || type_ids_compatible(m, expected, actual);
}

static const jello_fun_sig* fun_sig_of(const jello_bc_module* m, jello_type_id tid) {
  return &m->sigs[m->types[tid].as.fun.sig_id];
}

/** CONST_FUN and CLOSURE; the destination's signature must take the callee's arguments and
 * give its result (a Void one gives nothing, so any result goes). A closure's captures land
 * in the callee's Dynamic registers from cap_start, past its arguments. */
static jello_bc_result validate_function_value(const jello_bc_module* m,
                                               const jello_type_id* results,
                                               const jello_bc_function* f, const jello_insn* ins) {
  const jello_bc_function* callee = &m->funcs[ins->imm];
  if(rk_ne(m, f->reg_types, ins->a, JELLO_T_FUNCTION))
    return err(JELLO_BC_BAD_FORMAT, "function value dest must be Function", 0);
  const jello_fun_sig* sig = fun_sig_of(m, f->reg_types[ins->a]);
  if(sig->nargs != callee->nargs)
    return err(JELLO_BC_BAD_FORMAT, "function value arg count mismatch", 0);
  for(uint32_t a = 0; a < sig->nargs; a++) {
    if(!type_ids_compatible(m, sig->args[a], callee->reg_types[a]))
      return err(JELLO_BC_BAD_FORMAT, "function value arg type mismatch", 0);
  }
  if(m->types[sig->ret].kind != JELLO_T_VOID && !result_ok(m, results[ins->imm], sig->ret)) {
    return err(JELLO_BC_BAD_FORMAT, "function value result type mismatch", 0);
  }
  if(ins->c > 0 && callee->cap_start < callee->nargs)
    return err(JELLO_BC_BAD_FORMAT, "closure captures overlap the callee's arguments", 0);
  for(uint32_t i = 0; i < ins->c; i++) {
    if(rk_ne(m, callee->reg_types, callee->cap_start + i, JELLO_T_DYNAMIC)) {
      return err(JELLO_BC_BAD_FORMAT, "closure capture register in the callee must be Dynamic", 0);
    }
  }
  return ok();
}

jello_bc_result jello_bc_validate_results(const jello_bc_module* m) {
  jello_type_id* results =
      (jello_type_id*)malloc((m->nfuncs ? m->nfuncs : 1u) * sizeof(jello_type_id));
  if(!results) return err(JELLO_BC_OUT_OF_MEMORY, "oom result types", 0);
  jello_bc_result r = ok();
  for(uint32_t i = 0; i < m->nfuncs && !r.err; i++) {
    const jello_bc_function* f = &m->funcs[i];
    results[i] = NO_RESULT;
    for(uint32_t pc = 0; pc < f->ninsns; pc++) {
      if(f->insns[pc].op != (uint8_t)JOP_RET) continue;
      jello_type_id t = f->reg_types[f->insns[pc].a];
      if(!result_ok(m, results[i], t)) {
        r = err(JELLO_BC_BAD_FORMAT, "inconsistent return types", 0);
        break;
      }
      results[i] = t;
    }
  }
  /* A function without RET returns what it tail calls. Each pass that changes anything
   * settles another function, so this stops within nfuncs + 1 passes. */
  for(int changed = 1; changed && !r.err;) {
    changed = 0;
    for(uint32_t i = 0; i < m->nfuncs; i++) {
      const jello_bc_function* f = &m->funcs[i];
      for(uint32_t pc = 0; pc < f->ninsns && results[i] == NO_RESULT; pc++) {
        const jello_insn* ins = &f->insns[pc];
        if(ins->op == (uint8_t)JOP_TAILCALL && results[ins->imm] != NO_RESULT) {
          results[i] = results[ins->imm];
          changed = 1;
        }
        if(ins->op == (uint8_t)JOP_TAILCALLR) {
          results[i] = fun_sig_of(m, f->reg_types[ins->b])->ret;
          changed = 1;
        }
      }
    }
  }
  for(uint32_t i = 0; i < m->nfuncs && !r.err; i++) {
    const jello_bc_function* f = &m->funcs[i];
    for(uint32_t pc = 0; pc < f->ninsns && !r.err; pc++) {
      const jello_insn* ins = &f->insns[pc];
      switch((jello_op)ins->op) {
        case JOP_CALL:
          if(!result_ok(m, results[ins->imm], f->reg_types[ins->a]))
            r = err(JELLO_BC_BAD_FORMAT, "call result type mismatch", 0);
          break;
        case JOP_TAILCALL:
          if(!result_ok(m, results[ins->imm], results[i]))
            r = err(JELLO_BC_BAD_FORMAT, "tailcall result type mismatch", 0);
          break;
        case JOP_CALLR:
          if(!type_ids_compatible(m, fun_sig_of(m, f->reg_types[ins->b])->ret,
                                  f->reg_types[ins->a])) {
            r = err(JELLO_BC_BAD_FORMAT, "callr result type mismatch", 0);
          }
          break;
        case JOP_TAILCALLR:
          if(!type_ids_compatible(m, fun_sig_of(m, f->reg_types[ins->b])->ret, results[i])) {
            r = err(JELLO_BC_BAD_FORMAT, "tailcallr result type mismatch", 0);
          }
          break;
        case JOP_CONST_FUN:
        case JOP_CLOSURE: r = validate_function_value(m, results, f, ins); break;
        default: break;
      }
    }
  }
  free(results);
  return r;
}

/** Records depth `d` for `pc`, queueing it on first visit. Returns 0 when `pc` is out of
 * range or was reached at another depth. */
static int region_visit(int32_t* depth, uint32_t* work, uint32_t* nwork, int64_t pc,
                        uint32_t ninsns, int32_t d) {
  if(pc < 0 || pc >= (int64_t)ninsns) return 0;
  if(depth[pc] < 0) {
    depth[pc] = d;
    work[(*nwork)++] = (uint32_t)pc;
    return 1;
  }
  return depth[pc] == d;
}

/** Checks `f`'s try regions. TRY pushes a handler scope on the frame and ENDTRY pops it, so
 * every pc must be reached at one scope depth on every path. The handler runs one scope out;
 * THROW pops the scope before jumping to it. ENDTRY at depth 0, nesting past
 * JELLO_TRY_STACK_MAX, and a tail call inside a region (which would drop its scopes with the
 * frame) are rejected. `depth` and `work` are scratch arrays of f->ninsns entries. */
static jello_bc_result validate_regions(const jello_bc_function* f, int32_t* depth,
                                        uint32_t* work) {
  uint32_t n = f->ninsns, nwork = 0;
  for(uint32_t pc = 0; pc < n; pc++) depth[pc] = -1;
  depth[0] = 0;
  work[nwork++] = 0;
  while(nwork > 0) {
    uint32_t pc = work[--nwork];
    const jello_insn* ins = &f->insns[pc];
    int32_t d = depth[pc];
    int64_t next = (int64_t)pc + 1, jump = next + (int32_t)ins->imm;
    int ok_edges = 1;
    switch((jello_op)ins->op) {
      case JOP_RET:
      case JOP_THROW: break;
      case JOP_TAILCALL:
      case JOP_TAILCALLR:
        if(d > 0) return err(JELLO_BC_BAD_FORMAT, "tailcall inside try region", 0);
        break;
      case JOP_JMP: ok_edges = region_visit(depth, work, &nwork, jump, n, d); break;
      case JOP_JMP_IF:
        ok_edges = region_visit(depth, work, &nwork, next, n, d) &&
                   region_visit(depth, work, &nwork, jump, n, d);
        break;
      case JOP_TRY:
        if(d + 1 > (int32_t)JELLO_TRY_STACK_MAX)
          return err(JELLO_BC_BAD_FORMAT, "try regions nest too deep", 0);
        ok_edges = region_visit(depth, work, &nwork, next, n, d + 1) &&
                   region_visit(depth, work, &nwork, jump, n, d);
        break;
      case JOP_ENDTRY:
        if(d == 0) return err(JELLO_BC_BAD_FORMAT, "endtry outside try region", 0);
        ok_edges = region_visit(depth, work, &nwork, next, n, d - 1);
        break;
      case JOP_SWITCH_KIND: {
        int64_t after = next + ins->b;
        for(uint32_t i = 0; i < ins->b && ok_edges; i++) {
          if(next + i >= n) return err(JELLO_BC_BAD_FORMAT, "switch_kind table out of range", 0);
          ok_edges =
              region_visit(depth, work, &nwork, after + (int32_t)f->insns[next + i].imm, n, d);
        }
        if(ok_edges) ok_edges = region_visit(depth, work, &nwork, after + (int32_t)ins->imm, n, d);
        break;
      }
      default: ok_edges = region_visit(depth, work, &nwork, next, n, d); break;
    }
    if(!ok_edges) return err(JELLO_BC_BAD_FORMAT, "inconsistent try region depth", 0);
  }
  return ok();
}

jello_bc_result jello_bc_validate_try_regions(const jello_bc_module* m) {
  jello_bc_result r = ok();
  for(uint32_t i = 0; i < m->nfuncs && !r.err; i++) {
    const jello_bc_function* f = &m->funcs[i];
    int has_regions = 0;
    for(uint32_t pc = 0; pc < f->ninsns && !has_regions; pc++) {
      has_regions = f->insns[pc].op == (uint8_t)JOP_TRY || f->insns[pc].op == (uint8_t)JOP_ENDTRY;
    }
    if(!has_regions) continue;
    int32_t* depth = (int32_t*)malloc(f->ninsns * sizeof(int32_t));
    uint32_t* work = (uint32_t*)malloc(f->ninsns * sizeof(uint32_t));
    r = (depth && work) ? validate_regions(f, depth, work)
                        : err(JELLO_BC_OUT_OF_MEMORY, "oom try regions", 0);
    free(depth);
    free(work);
  }
  return r;
}
