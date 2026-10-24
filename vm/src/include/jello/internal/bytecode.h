// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The `.jlo` bytecode format; feature bits, the opcode list and its operand metadata, the 8-byte
 * instruction and the in-memory function record.
 *
 *      _______________________________________________________________________
 *   0 |          magic 0x4A4C5942         |  0x01  |  0x00  |  0x00  |  0x00  |
 *     |___________________________________|________|________|________|________|
 *   8 |              features             |               ntypes              |
 *     |___________________________________|___________________________________|
 *  16 |               nsigs               |            nstruct_sigs           |
 *     |___________________________________|___________________________________|
 *  24 |               natoms              |               nfuncs              |
 *     |___________________________________|___________________________________|
 *  32 |               entry               |              nimports             |
 *     |___________________________________|___________________________________|
 *  40 |      nenum_sigs (ENUM_SCHEMA)     |        nconst_i64 (CONST64)       |
 *     |___________________________________|___________________________________|
 *  48 |        nconst_f64 (CONST64)       |     nconst_bytes (CONSTBYTES)     |
 *     |___________________________________|___________________________________|
 *  56 |         nglobals (GLOBALS)        |         tables follow ...         |
 *     |___________________________________|___________________________________|
 *
 * Each cell is one byte; the bracketed counts are present only when their feature bit is set,
 * and later fields move up to close the gap.
 *
 * A `.jlo` file is little-endian and laid out as follows (loader.c reads it):
 * - header, all u32: magic 0x4A4C5942, version 1, features, ntypes, nsigs, nstruct_sigs,
 *   natoms, nfuncs, entry, nimports; then nenum_sigs (ENUM_SCHEMA), nconst_i64 and nconst_f64
 *   (CONST64), nconst_bytes (CONSTBYTES) and nglobals (GLOBALS);
 * - types, 12 bytes each: u8 kind, 3 pad bytes, u32 parameter (element type, signature,
 *   struct or enum signature, or abstract id, by kind), u32 unused;
 * - function signatures: u32 result type, u16 nargs, u32 per argument type;
 * - struct signatures: u16 nfields, u8 named, u8 mutable, u32 per field type, then u32 per
 *   field name atom when named;
 * - enum signatures: u16 nvariants, then per variant u16 nfields and u32 per field type;
 * - atoms: u32 length and the bytes;
 * - imports: u32 library atom, u32 export atom, u8 result kind, u8 arity (at most 8), and
 *   8 u8 argument kinds;
 * - constant pools: i64 and f64 as two u32 halves (low first), bytes as u32 length and data;
 * - globals: u32 type each;
 * - functions: u32 name atom, nregs (at most 256), cap_start, nargs and ninsns, a u32 type per
 *   register, then the instructions (8 bytes each, or compressed with COMPRESSED_INSN);
 * - the line table (LINE_TABLE), and nothing after it.
 *
 * ENUM_SCHEMA also turns on the enum signature checks. The VM ignores the feature bits not named
 * above; cap_start, nargs and nimports are always present whatever the bits say. */

#ifndef JELLO_BYTECODE_H
#define JELLO_BYTECODE_H

#include <jello.h>

/** Bits of a `.jlo` header's `features` word; bits 16-31 are reserved. */
typedef enum jello_bc_feature_flags {
  JELLO_BC_FEAT_NONE = 0,
  JELLO_BC_FEAT_CONST64 = 1u << 0,
  JELLO_BC_FEAT_CONSTBYTES = 1u << 1,
  JELLO_BC_FEAT_CAP_START = 1u << 2,
  JELLO_BC_FEAT_LINE_TABLE = 1u << 3,
  JELLO_BC_FEAT_LINE_COL = 1u << 4,
  JELLO_BC_FEAT_LINE_DELTA = 1u << 5,
  JELLO_BC_FEAT_COMPRESSED_INSN = 1u << 6,
  JELLO_BC_FEAT_LOOP_ANCHOR = 1u << 7,
  JELLO_BC_FEAT_TRAP_MAP = 1u << 8,
  JELLO_BC_FEAT_HANDLER_MAP = 1u << 9,
  JELLO_BC_FEAT_TYPE_PROFILE = 1u << 10,
  JELLO_BC_FEAT_STACK_MAP = 1u << 11,
  JELLO_BC_FEAT_PKG_DEPS = 1u << 12,
  /** Whether jlo_tool's text form has an `imports:` block; nimports is in every header. */
  JELLO_BC_FEAT_IMPORTS = 1u << 13,
  JELLO_BC_FEAT_ENUM_SCHEMA = 1u << 14,
  JELLO_BC_FEAT_GLOBALS = 1u << 15,
} jello_bc_feature_flags;

/** What an operand slot of an instruction holds, per opcode (jello_op_info's role columns).
 * Operand metadata records shape and side effects only; checks that need data beyond the one
 * instruction (a struct's field types, a callee's registers, a signature) are check.c's. */
typedef enum jello_operand_role {
  JELLO_OPR_UNUSED = 0,
  /** A register index. In `imm` it is CALLR's and TAILCALLR's first argument register and
   * FMA_F64's addend. */
  JELLO_OPR_REG,
  /** An opcode-specific literal, in `imm` or in the a/b/c byte itself (CALL's argument count
   * `c`, for example). */
  JELLO_OPR_IMM_RAW,
  /** A signed jump delta in `imm`, relative to the next instruction (to the end of the case
   * table for SWITCH_KIND and CASE_KIND). */
  JELLO_OPR_IMM_JUMP,
  /** An index in `imm` into an opcode-specific module table; a constant pool, the functions,
   * the atoms, the imports or the globals. */
  JELLO_OPR_IMM_POOL,
} jello_operand_role;

/** Kind column value for a register whose kind the opcode alone does not fix; it depends on a
 * struct field, a callee or a MOV source, or (for I32 arithmetic results) may be I8, I16 or I32.
 * check.c decides those. It differs from every jello_type_kind (they start at 1) and from the
 * 0 used for operands that are not registers. */
#define JELLO_OPK_POLY ((uint8_t)0xFFu)

/** Side effects of an opcode. jit/ir.c withholds JELLO_JIR_FLAG_NUMERIC_FAST from a function
 * that has any opcode with CAN_ALLOC, TOUCHES_DYNAMIC or UNIMPLEMENTED. */
typedef enum jello_op_flags {
  JELLO_OPF_NONE = 0,
  JELLO_OPF_CAN_TRAP = 1u << 0,  /**< May set trap_code and return OP_TRAP. */
  JELLO_OPF_CAN_ALLOC = 1u << 1, /**< May allocate on the GC heap. */
  JELLO_OPF_CAN_CALL = 1u << 2,  /**< Calls another function or a native export. */
  JELLO_OPF_IS_BRANCH = 1u << 3, /**< `imm` is a jump delta that control may take. */
  /** A tail call; control never falls through. RET, JMP and THROW do not carry it. */
  JELLO_OPF_IS_TERMINATOR = 1u << 4,
  JELLO_OPF_TOUCHES_DYNAMIC = 1u << 5, /**< Reads or produces Dynamic values or Objects. */
  JELLO_OPF_UNIMPLEMENTED = 1u << 6,   /**< No interpreter body; validation rejects it. */
} jello_op_flags;

/** Operand metadata of one opcode, as listed in JELLO_OP_LIST. */
typedef struct jello_op_info {
  const char* name;               /**< As spelt in jlo_tool's text form and diagnostics. */
  uint8_t a_role, b_role, c_role; /**< jello_operand_role of each operand byte. */
  /** jello_type_kind of a register operand, JELLO_OPK_POLY, or 0 for other operands. */
  uint8_t a_kind, b_kind, c_kind;
  uint8_t imm_role; /**< jello_operand_role of `imm`. */
  uint8_t flags;    /**< jello_op_flags. */
} jello_op_info;

/** Every opcode, once; OP(id, name, a_role, a_kind, b_role, b_kind, c_role, c_kind, imm_role,
 * flags, handler). It generates the jello_op enum below, jello_op_table (op_table.c, which
 * defines the short role and kind names the rows use) and op_dispatch's table (dispatch.c).
 * An opcode's number is its position in the list and is part of the `.jlo` format, so the list is
 * append-only.
 *
 * `handler` is the function op_dispatch calls. It is op_unimplemented for opcodes the main loop
 * (exec_loop.c) runs inline and for those with no interpreter body, which carry
 * JELLO_OPF_UNIMPLEMENTED. */
#define JELLO_OP_LIST(OP)                                                                          \
  OP(JOP_NOP, "NOP", UNUSED, 0, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE, op_unimplemented)  \
  OP(JOP_RET, "RET", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE, op_unimplemented)  \
  OP(JOP_JMP, "JMP", UNUSED, 0, UNUSED, 0, UNUSED, 0, IMM_JUMP, JELLO_OPF_IS_BRANCH,               \
     op_unimplemented)                                                                             \
  OP(JOP_JMP_IF, "JMP_IF", REG, T_BOOL, UNUSED, 0, UNUSED, 0, IMM_JUMP, JELLO_OPF_IS_BRANCH,       \
     op_unimplemented)                                                                             \
  OP(JOP_MOV, "MOV", REG, POLY, REG, POLY, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE, op_unimplemented)  \
  OP(JOP_TRY, "TRY", REG, POLY, IMM_RAW, 0, UNUSED, 0, IMM_JUMP, JELLO_OPF_IS_BRANCH, op_try)      \
  OP(JOP_ENDTRY, "ENDTRY", UNUSED, 0, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE, op_endtry)   \
  OP(JOP_THROW, "THROW", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_CAN_TRAP, op_throw)  \
  OP(JOP_ASSERT, "ASSERT", REG, T_BOOL, REG, T_BYTES, IMM_RAW, 0, IMM_NONE,                        \
     JELLO_OPF_UNIMPLEMENTED | JELLO_OPF_CAN_TRAP, op_unimplemented)                               \
  OP(JOP_SWITCH_KIND, "SWITCH_KIND", REG, T_DYNAMIC, IMM_RAW, 0, UNUSED, 0, IMM_JUMP,              \
     JELLO_OPF_IS_BRANCH, op_switch_kind)                                                          \
  OP(JOP_CASE_KIND, "CASE_KIND", IMM_RAW, 0, UNUSED, 0, UNUSED, 0, IMM_JUMP, JELLO_OPF_NONE,       \
     op_case_kind)                                                                                 \
  OP(JOP_CALL, "CALL", REG, POLY, REG, POLY, IMM_RAW, 0, IMM_POOL, JELLO_OPF_CAN_CALL,             \
     op_unimplemented)                                                                             \
  OP(JOP_CALLR, "CALLR", REG, POLY, REG, POLY, IMM_RAW, 0, REG, JELLO_OPF_CAN_CALL, op_callr)      \
  OP(JOP_TAILCALL, "TAILCALL", REG, POLY, REG, POLY, IMM_RAW, 0, IMM_POOL,                         \
     JELLO_OPF_CAN_CALL | JELLO_OPF_IS_TERMINATOR, op_tailcall)                                    \
  OP(JOP_TAILCALLR, "TAILCALLR", REG, POLY, REG, POLY, IMM_RAW, 0, REG,                            \
     JELLO_OPF_CAN_CALL | JELLO_OPF_IS_TERMINATOR, op_tailcallr)                                   \
  OP(JOP_CONST_FUN, "CONST_FUN", REG, POLY, REG, POLY, IMM_RAW, 0, IMM_POOL, JELLO_OPF_CAN_ALLOC,  \
     op_const_fun)                                                                                 \
  OP(JOP_CLOSURE, "CLOSURE", REG, POLY, REG, POLY, IMM_RAW, 0, IMM_POOL, JELLO_OPF_CAN_ALLOC,      \
     op_closure)                                                                                   \
  OP(JOP_BIND_THIS, "BIND_THIS", REG, POLY, REG, POLY, UNUSED, 0, IMM_NONE,                        \
     JELLO_OPF_UNIMPLEMENTED, op_unimplemented)                                                    \
  OP(JOP_CONST_I8_IMM, "CONST_I8_IMM", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,   \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_I32, "CONST_I32", REG, T_I32, UNUSED, 0, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,        \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_I64, "CONST_I64", REG, T_I64, UNUSED, 0, UNUSED, 0, IMM_POOL, JELLO_OPF_NONE,       \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_F16, "CONST_F16", REG, T_F16, UNUSED, 0, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,        \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_F32, "CONST_F32", REG, T_F32, UNUSED, 0, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,        \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_F64, "CONST_F64", REG, T_F64, UNUSED, 0, UNUSED, 0, IMM_POOL, JELLO_OPF_NONE,       \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_BOOL, "CONST_BOOL", REG, T_BOOL, UNUSED, 0, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,     \
     op_unimplemented)                                                                             \
  OP(JOP_CONST_NULL, "CONST_NULL", REG, T_DYNAMIC, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE, \
     op_const_null)                                                                                \
  OP(JOP_CONST_ATOM, "CONST_ATOM", REG, T_ATOM, UNUSED, 0, UNUSED, 0, IMM_POOL, JELLO_OPF_NONE,    \
     op_const_atom)                                                                                \
  OP(JOP_CONST_BYTES, "CONST_BYTES", REG, T_BYTES, UNUSED, 0, UNUSED, 0, IMM_POOL,                 \
     JELLO_OPF_CAN_TRAP, op_const_bytes)                                                           \
  OP(JOP_ADD_I32, "ADD_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_SUB_I32, "SUB_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_MUL_I32, "MUL_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_DIV_I32, "DIV_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_CAN_TRAP,      \
     op_div_i32)                                                                                   \
  OP(JOP_MOD_I32, "MOD_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_CAN_TRAP,      \
     op_mod_i32)                                                                                   \
  OP(JOP_ADD_I32_IMM, "ADD_I32_IMM", REG, POLY, REG, T_I32, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,    \
     op_unimplemented)                                                                             \
  OP(JOP_SUB_I32_IMM, "SUB_I32_IMM", REG, POLY, REG, T_I32, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,    \
     op_unimplemented)                                                                             \
  OP(JOP_MUL_I32_IMM, "MUL_I32_IMM", REG, POLY, REG, T_I32, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,    \
     op_unimplemented)                                                                             \
  OP(JOP_NEG_I32, "NEG_I32", REG, POLY, REG, T_I32, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,           \
     op_neg_i32)                                                                                   \
  OP(JOP_SHL_I32, "SHL_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_shl_i32)                                                                                   \
  OP(JOP_SHR_I32, "SHR_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_shr_i32)                                                                                   \
  OP(JOP_BITAND_I32, "BITAND_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,    \
     op_bitand_i32)                                                                                \
  OP(JOP_BITOR_I32, "BITOR_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,      \
     op_bitor_i32)                                                                                 \
  OP(JOP_BITXOR_I32, "BITXOR_I32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,    \
     op_bitxor_i32)                                                                                \
  OP(JOP_ADD_I64, "ADD_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,         \
     op_add_i64)                                                                                   \
  OP(JOP_SUB_I64, "SUB_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,         \
     op_sub_i64)                                                                                   \
  OP(JOP_MUL_I64, "MUL_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,         \
     op_mul_i64)                                                                                   \
  OP(JOP_DIV_I64, "DIV_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_CAN_TRAP,     \
     op_div_i64)                                                                                   \
  OP(JOP_MOD_I64, "MOD_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_CAN_TRAP,     \
     op_mod_i64)                                                                                   \
  OP(JOP_NEG_I64, "NEG_I64", REG, T_I64, REG, T_I64, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,          \
     op_neg_i64)                                                                                   \
  OP(JOP_SHL_I64, "SHL_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,         \
     op_shl_i64)                                                                                   \
  OP(JOP_SHR_I64, "SHR_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,         \
     op_shr_i64)                                                                                   \
  OP(JOP_BITAND_I64, "BITAND_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,   \
     op_bitand_i64)                                                                                \
  OP(JOP_BITOR_I64, "BITOR_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,     \
     op_bitor_i64)                                                                                 \
  OP(JOP_BITXOR_I64, "BITXOR_I64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,   \
     op_bitxor_i64)                                                                                \
  OP(JOP_ADD_F16, "ADD_F16", REG, T_F16, REG, T_F16, REG, T_F16, IMM_NONE, JELLO_OPF_NONE,         \
     op_add_f16)                                                                                   \
  OP(JOP_SUB_F16, "SUB_F16", REG, T_F16, REG, T_F16, REG, T_F16, IMM_NONE, JELLO_OPF_NONE,         \
     op_sub_f16)                                                                                   \
  OP(JOP_MUL_F16, "MUL_F16", REG, T_F16, REG, T_F16, REG, T_F16, IMM_NONE, JELLO_OPF_NONE,         \
     op_mul_f16)                                                                                   \
  OP(JOP_ADD_F32, "ADD_F32", REG, T_F32, REG, T_F32, REG, T_F32, IMM_NONE, JELLO_OPF_NONE,         \
     op_add_f32)                                                                                   \
  OP(JOP_SUB_F32, "SUB_F32", REG, T_F32, REG, T_F32, REG, T_F32, IMM_NONE, JELLO_OPF_NONE,         \
     op_sub_f32)                                                                                   \
  OP(JOP_MUL_F32, "MUL_F32", REG, T_F32, REG, T_F32, REG, T_F32, IMM_NONE, JELLO_OPF_NONE,         \
     op_mul_f32)                                                                                   \
  OP(JOP_DIV_F32, "DIV_F32", REG, T_F32, REG, T_F32, REG, T_F32, IMM_NONE, JELLO_OPF_NONE,         \
     op_div_f32)                                                                                   \
  OP(JOP_NEG_F32, "NEG_F32", REG, T_F32, REG, T_F32, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,          \
     op_neg_f32)                                                                                   \
  OP(JOP_ADD_F64, "ADD_F64", REG, T_F64, REG, T_F64, REG, T_F64, IMM_NONE, JELLO_OPF_NONE,         \
     op_unimplemented)                                                                             \
  OP(JOP_SUB_F64, "SUB_F64", REG, T_F64, REG, T_F64, REG, T_F64, IMM_NONE, JELLO_OPF_NONE,         \
     op_unimplemented)                                                                             \
  OP(JOP_MUL_F64, "MUL_F64", REG, T_F64, REG, T_F64, REG, T_F64, IMM_NONE, JELLO_OPF_NONE,         \
     op_unimplemented)                                                                             \
  OP(JOP_DIV_F64, "DIV_F64", REG, T_F64, REG, T_F64, REG, T_F64, IMM_NONE, JELLO_OPF_NONE,         \
     op_unimplemented)                                                                             \
  OP(JOP_NEG_F64, "NEG_F64", REG, T_F64, REG, T_F64, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_NOT_BOOL, "NOT_BOOL", REG, T_BOOL, REG, T_BOOL, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,      \
     op_unimplemented)                                                                             \
  OP(JOP_EQ_I32, "EQ_I32", REG, T_BOOL, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_LT_I32, "LT_I32", REG, T_BOOL, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_EQ_I32_IMM, "EQ_I32_IMM", REG, T_BOOL, REG, T_I32, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,    \
     op_unimplemented)                                                                             \
  OP(JOP_LT_I32_IMM, "LT_I32_IMM", REG, T_BOOL, REG, T_I32, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,    \
     op_unimplemented)                                                                             \
  OP(JOP_EQ_I64, "EQ_I64", REG, T_BOOL, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,          \
     op_eq_i64)                                                                                    \
  OP(JOP_LT_I64, "LT_I64", REG, T_BOOL, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,          \
     op_lt_i64)                                                                                    \
  OP(JOP_EQ_F32, "EQ_F32", REG, T_BOOL, REG, T_F32, REG, T_F32, IMM_NONE, JELLO_OPF_NONE,          \
     op_eq_f32)                                                                                    \
  OP(JOP_LT_F32, "LT_F32", REG, T_BOOL, REG, T_F32, REG, T_F32, IMM_NONE, JELLO_OPF_NONE,          \
     op_lt_f32)                                                                                    \
  OP(JOP_EQ_F64, "EQ_F64", REG, T_BOOL, REG, T_F64, REG, T_F64, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_LT_F64, "LT_F64", REG, T_BOOL, REG, T_F64, REG, T_F64, IMM_NONE, JELLO_OPF_NONE,          \
     op_unimplemented)                                                                             \
  OP(JOP_SEXT_I64, "SEXT_I64", REG, T_I64, REG, T_I32, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,        \
     op_sext_i64)                                                                                  \
  OP(JOP_SEXT_I16, "SEXT_I16", REG, T_I32, REG, T_I16, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,        \
     op_sext_i16)                                                                                  \
  OP(JOP_TRUNC_I8, "TRUNC_I8", REG, T_I8, REG, T_I32, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,         \
     op_trunc_i8)                                                                                  \
  OP(JOP_TRUNC_I16, "TRUNC_I16", REG, T_I16, REG, T_I32, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,      \
     op_trunc_i16)                                                                                 \
  OP(JOP_I32_FROM_I64, "I32_FROM_I64", REG, T_I32, REG, T_I64, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_i32_from_i64)                                                              \
  OP(JOP_F64_FROM_I32, "F64_FROM_I32", REG, T_F64, REG, T_I32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_unimplemented)                                                             \
  OP(JOP_I32_FROM_F64, "I32_FROM_F64", REG, T_I32, REG, T_F64, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_CAN_TRAP, op_i32_from_f64)                                                          \
  OP(JOP_F64_FROM_I64, "F64_FROM_I64", REG, T_F64, REG, T_I64, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f64_from_i64)                                                              \
  OP(JOP_I64_FROM_F64, "I64_FROM_F64", REG, T_I64, REG, T_F64, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_CAN_TRAP, op_i64_from_f64)                                                          \
  OP(JOP_F32_FROM_I32, "F32_FROM_I32", REG, T_F32, REG, T_I32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f32_from_i32)                                                              \
  OP(JOP_I32_FROM_F32, "I32_FROM_F32", REG, T_I32, REG, T_F32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_CAN_TRAP, op_i32_from_f32)                                                          \
  OP(JOP_F64_FROM_F32, "F64_FROM_F32", REG, T_F64, REG, T_F32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f64_from_f32)                                                              \
  OP(JOP_F32_FROM_F64, "F32_FROM_F64", REG, T_F32, REG, T_F64, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f32_from_f64)                                                              \
  OP(JOP_F32_FROM_I64, "F32_FROM_I64", REG, T_F32, REG, T_I64, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f32_from_i64)                                                              \
  OP(JOP_I64_FROM_F32, "I64_FROM_F32", REG, T_I64, REG, T_F32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_CAN_TRAP, op_i64_from_f32)                                                          \
  OP(JOP_F16_FROM_F32, "F16_FROM_F32", REG, T_F16, REG, T_F32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f16_from_f32)                                                              \
  OP(JOP_F32_FROM_F16, "F32_FROM_F16", REG, T_F32, REG, T_F16, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f32_from_f16)                                                              \
  OP(JOP_F16_FROM_I32, "F16_FROM_I32", REG, T_F16, REG, T_I32, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_f16_from_i32)                                                              \
  OP(JOP_I32_FROM_F16, "I32_FROM_F16", REG, T_I32, REG, T_F16, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_CAN_TRAP, op_i32_from_f16)                                                          \
  OP(JOP_TO_DYN, "TO_DYN", REG, T_DYNAMIC, REG, POLY, UNUSED, 0, IMM_NONE,                         \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_ALLOC, op_to_dyn)                                   \
  OP(JOP_FROM_DYN_I8, "FROM_DYN_I8", REG, T_I8, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,               \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_i8)                               \
  OP(JOP_FROM_DYN_I16, "FROM_DYN_I16", REG, T_I16, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,            \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_i16)                              \
  OP(JOP_FROM_DYN_I32, "FROM_DYN_I32", REG, T_I32, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,            \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_i32)                              \
  OP(JOP_FROM_DYN_I64, "FROM_DYN_I64", REG, T_I64, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,            \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_i64)                              \
  OP(JOP_FROM_DYN_F16, "FROM_DYN_F16", REG, T_F16, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,            \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_f16)                              \
  OP(JOP_FROM_DYN_F32, "FROM_DYN_F32", REG, T_F32, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,            \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_f32)                              \
  OP(JOP_FROM_DYN_F64, "FROM_DYN_F64", REG, T_F64, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,            \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_f64)                              \
  OP(JOP_FROM_DYN_BOOL, "FROM_DYN_BOOL", REG, T_BOOL, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,         \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_bool)                             \
  OP(JOP_FROM_DYN_ATOM, "FROM_DYN_ATOM", REG, T_ATOM, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,         \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_atom)                             \
  OP(JOP_FROM_DYN_PTR, "FROM_DYN_PTR", REG, POLY, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,             \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_from_dyn_ptr)                              \
  OP(JOP_SPILL_PUSH, "SPILL_PUSH", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_RAW,                       \
     JELLO_OPF_TOUCHES_DYNAMIC, op_spill_push)                                                     \
  OP(JOP_SPILL_POP, "SPILL_POP", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_RAW,                         \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_TRAP, op_spill_pop)                                 \
  OP(JOP_PHYSEQ, "PHYSEQ", REG, T_BOOL, REG, POLY, REG, POLY, IMM_NONE, JELLO_OPF_TOUCHES_DYNAMIC, \
     op_physeq)                                                                                    \
  OP(JOP_KINDOF, "KINDOF", REG, T_I32, REG, T_DYNAMIC, UNUSED, 0, IMM_NONE,                        \
     JELLO_OPF_TOUCHES_DYNAMIC, op_kindof)                                                         \
  OP(JOP_LIST_NIL, "LIST_NIL", REG, T_LIST, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,        \
     op_list_nil)                                                                                  \
  OP(JOP_LIST_CONS, "LIST_CONS", REG, T_LIST, REG, POLY, REG, T_LIST, IMM_NONE,                    \
     JELLO_OPF_CAN_ALLOC, op_list_cons)                                                            \
  OP(JOP_LIST_HEAD, "LIST_HEAD", REG, POLY, REG, T_LIST, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,      \
     op_list_head)                                                                                 \
  OP(JOP_LIST_TAIL, "LIST_TAIL", REG, T_LIST, REG, T_LIST, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,    \
     op_list_tail)                                                                                 \
  OP(JOP_LIST_IS_NIL, "LIST_IS_NIL", REG, T_BOOL, REG, T_LIST, UNUSED, 0, IMM_NONE,                \
     JELLO_OPF_NONE, op_list_is_nil)                                                               \
  OP(JOP_ARRAY_NEW, "ARRAY_NEW", REG, T_ARRAY, REG, T_I32, UNUSED, 0, IMM_NONE,                    \
     JELLO_OPF_CAN_ALLOC, op_array_new)                                                            \
  OP(JOP_ARRAY_LEN, "ARRAY_LEN", REG, T_I32, REG, T_ARRAY, UNUSED, 0, IMM_NONE,                    \
     JELLO_OPF_CAN_TRAP, op_array_len)                                                             \
  OP(JOP_ARRAY_GET, "ARRAY_GET", REG, POLY, REG, T_ARRAY, REG, T_I32, IMM_NONE,                    \
     JELLO_OPF_CAN_TRAP, op_array_get)                                                             \
  OP(JOP_ARRAY_SET, "ARRAY_SET", REG, POLY, REG, T_ARRAY, REG, T_I32, IMM_NONE,                    \
     JELLO_OPF_CAN_TRAP, op_array_set)                                                             \
  OP(JOP_ARRAY_CONCAT2, "ARRAY_CONCAT2", REG, T_ARRAY, REG, T_ARRAY, REG, T_ARRAY, IMM_NONE,       \
     JELLO_OPF_CAN_ALLOC | JELLO_OPF_CAN_TRAP, op_array_concat2)                                   \
  OP(JOP_ARRAY_RESIZE, "ARRAY_RESIZE", UNUSED, 0, REG, T_ARRAY, REG, T_I32, IMM_NONE,              \
     JELLO_OPF_CAN_ALLOC | JELLO_OPF_CAN_TRAP, op_array_resize)                                    \
  OP(JOP_BYTES_NEW, "BYTES_NEW", REG, T_BYTES, REG, T_I32, UNUSED, 0, IMM_NONE,                    \
     JELLO_OPF_CAN_ALLOC, op_bytes_new)                                                            \
  OP(JOP_BYTES_LEN, "BYTES_LEN", REG, T_I32, REG, T_BYTES, UNUSED, 0, IMM_NONE,                    \
     JELLO_OPF_CAN_TRAP, op_bytes_len)                                                             \
  OP(JOP_BYTES_GET_U8, "BYTES_GET_U8", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,             \
     JELLO_OPF_CAN_TRAP, op_bytes_get_u8)                                                          \
  OP(JOP_BYTES_SET_U8, "BYTES_SET_U8", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,             \
     JELLO_OPF_CAN_TRAP, op_bytes_set_u8)                                                          \
  OP(JOP_BYTES_CONCAT2, "BYTES_CONCAT2", REG, T_BYTES, REG, T_BYTES, REG, T_BYTES, IMM_NONE,       \
     JELLO_OPF_CAN_TRAP | JELLO_OPF_CAN_ALLOC, op_bytes_concat2)                                   \
  OP(JOP_BYTES_CONCAT_MANY, "BYTES_CONCAT_MANY", REG, T_BYTES, REG, POLY, IMM_RAW, 0, IMM_NONE,    \
     JELLO_OPF_CAN_TRAP | JELLO_OPF_CAN_ALLOC, op_bytes_concat_many)                               \
  OP(JOP_BYTES_EQ, "BYTES_EQ", REG, T_BOOL, REG, T_BYTES, REG, T_BYTES, IMM_NONE, JELLO_OPF_NONE,  \
     op_bytes_eq)                                                                                  \
  OP(JOP_BYTES_BITAND2, "BYTES_BITAND2", REG, T_BYTES, REG, T_BYTES, REG, T_BYTES, IMM_NONE,       \
     JELLO_OPF_CAN_TRAP | JELLO_OPF_CAN_ALLOC, op_bytes_bitand2)                                   \
  OP(JOP_BYTES_BITOR2, "BYTES_BITOR2", REG, T_BYTES, REG, T_BYTES, REG, T_BYTES, IMM_NONE,         \
     JELLO_OPF_CAN_TRAP | JELLO_OPF_CAN_ALLOC, op_bytes_bitor2)                                    \
  OP(JOP_BYTES_BITXOR2, "BYTES_BITXOR2", REG, T_BYTES, REG, T_BYTES, REG, T_BYTES, IMM_NONE,       \
     JELLO_OPF_CAN_TRAP | JELLO_OPF_CAN_ALLOC, op_bytes_bitxor2)                                   \
  OP(JOP_BYTES_READ_U16_LE, "BYTES_READ_U16_LE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_u16_le)                                                     \
  OP(JOP_BYTES_READ_U16_BE, "BYTES_READ_U16_BE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_u16_be)                                                     \
  OP(JOP_BYTES_READ_U32_LE, "BYTES_READ_U32_LE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_u32_le)                                                     \
  OP(JOP_BYTES_READ_U32_BE, "BYTES_READ_U32_BE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_u32_be)                                                     \
  OP(JOP_BYTES_READ_I32_LE, "BYTES_READ_I32_LE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_i32_le)                                                     \
  OP(JOP_BYTES_READ_I32_BE, "BYTES_READ_I32_BE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_i32_be)                                                     \
  OP(JOP_BYTES_READ_F32_LE, "BYTES_READ_F32_LE", REG, T_F32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_f32_le)                                                     \
  OP(JOP_BYTES_READ_F32_BE, "BYTES_READ_F32_BE", REG, T_F32, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_f32_be)                                                     \
  OP(JOP_BYTES_WRITE_U16_LE, "BYTES_WRITE_U16_LE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_u16_le)                                                    \
  OP(JOP_BYTES_WRITE_U16_BE, "BYTES_WRITE_U16_BE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_u16_be)                                                    \
  OP(JOP_BYTES_WRITE_U32_LE, "BYTES_WRITE_U32_LE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_u32_le)                                                    \
  OP(JOP_BYTES_WRITE_U32_BE, "BYTES_WRITE_U32_BE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_u32_be)                                                    \
  OP(JOP_BYTES_WRITE_I32_LE, "BYTES_WRITE_I32_LE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_i32_le)                                                    \
  OP(JOP_BYTES_WRITE_I32_BE, "BYTES_WRITE_I32_BE", REG, T_I32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_i32_be)                                                    \
  OP(JOP_BYTES_WRITE_F32_LE, "BYTES_WRITE_F32_LE", REG, T_F32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_f32_le)                                                    \
  OP(JOP_BYTES_WRITE_F32_BE, "BYTES_WRITE_F32_BE", REG, T_F32, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_f32_be)                                                    \
  OP(JOP_BITS_WRITE, "BITS_WRITE", REG, T_BYTES, REG, T_I32, IMM_RAW, 0, IMM_RAW,                  \
     JELLO_OPF_CAN_TRAP, op_bits_write)                                                            \
  OP(JOP_BITS_READ, "BITS_READ", REG, T_I32, REG, T_BYTES, IMM_RAW, 0, IMM_RAW,                    \
     JELLO_OPF_CAN_TRAP, op_bits_read)                                                             \
  OP(JOP_STRUCT_NEW, "STRUCT_NEW", REG, T_STRUCT, UNUSED, 0, UNUSED, 0, IMM_NONE,                  \
     JELLO_OPF_CAN_ALLOC, op_struct_new)                                                           \
  OP(JOP_STRUCT_GET, "STRUCT_GET", REG, POLY, REG, T_STRUCT, UNUSED, 0, IMM_RAW, JELLO_OPF_NONE,   \
     op_struct_get)                                                                                \
  OP(JOP_STRUCT_SET, "STRUCT_SET", REG, POLY, REG, T_STRUCT, UNUSED, 0, IMM_RAW,                   \
     JELLO_OPF_CAN_TRAP, op_struct_set)                                                            \
  OP(JOP_OBJ_NEW, "OBJ_NEW", REG, T_OBJECT, UNUSED, 0, UNUSED, 0, IMM_NONE,                        \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_ALLOC, op_obj_new)                                  \
  OP(JOP_OBJ_HAS_ATOM, "OBJ_HAS_ATOM", REG, T_BOOL, REG, T_OBJECT, UNUSED, 0, IMM_POOL,            \
     JELLO_OPF_TOUCHES_DYNAMIC, op_obj_has_atom)                                                   \
  OP(JOP_OBJ_GET_ATOM, "OBJ_GET_ATOM", REG, POLY, REG, T_OBJECT, UNUSED, 0, IMM_POOL,              \
     JELLO_OPF_TOUCHES_DYNAMIC, op_obj_get_atom)                                                   \
  OP(JOP_OBJ_SET_ATOM, "OBJ_SET_ATOM", REG, POLY, REG, T_OBJECT, UNUSED, 0, IMM_POOL,              \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_ALLOC, op_obj_set_atom)                             \
  OP(JOP_OBJ_GET, "OBJ_GET", REG, POLY, REG, T_OBJECT, REG, T_ATOM, IMM_NONE,                      \
     JELLO_OPF_TOUCHES_DYNAMIC, op_obj_get)                                                        \
  OP(JOP_OBJ_SET, "OBJ_SET", REG, POLY, REG, T_OBJECT, REG, T_ATOM, IMM_NONE,                      \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_ALLOC, op_obj_set)                                  \
  OP(JOP_OBJ_HAS, "OBJ_HAS", REG, T_BOOL, REG, T_OBJECT, REG, T_ATOM, IMM_NONE,                    \
     JELLO_OPF_TOUCHES_DYNAMIC, op_obj_has)                                                        \
  OP(JOP_OBJ_REMOVE, "OBJ_REMOVE", REG, T_BOOL, REG, T_OBJECT, REG, T_ATOM, IMM_NONE,              \
     JELLO_OPF_TOUCHES_DYNAMIC, op_obj_remove)                                                     \
  OP(JOP_OBJ_CLEAR, "OBJ_CLEAR", UNUSED, 0, REG, T_OBJECT, UNUSED, 0, IMM_NONE,                    \
     JELLO_OPF_TOUCHES_DYNAMIC, op_obj_clear)                                                      \
  OP(JOP_OBJ_COPY, "OBJ_COPY", REG, T_OBJECT, REG, T_OBJECT, UNUSED, 0, IMM_NONE,                  \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_ALLOC, op_obj_copy)                                 \
  OP(JOP_OBJ_KEYS, "OBJ_KEYS", REG, T_ARRAY, REG, T_OBJECT, UNUSED, 0, IMM_NONE,                   \
     JELLO_OPF_TOUCHES_DYNAMIC | JELLO_OPF_CAN_ALLOC, op_obj_keys)                                 \
  OP(JOP_ENUM_NEW, "ENUM_NEW", REG, T_ENUM, REG, POLY, IMM_RAW, 0, IMM_RAW, JELLO_OPF_CAN_ALLOC,   \
     op_enum_new)                                                                                  \
  OP(JOP_ENUM_TAG, "ENUM_TAG", REG, T_I32, REG, T_ENUM, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,       \
     op_enum_tag)                                                                                  \
  OP(JOP_ENUM_GET, "ENUM_GET", REG, POLY, REG, T_ENUM, REG, T_I32, IMM_NONE, JELLO_OPF_CAN_TRAP,   \
     op_enum_get)                                                                                  \
  OP(JOP_CALL_NATIVE, "CALL_NATIVE", REG, POLY, REG, POLY, IMM_RAW, 0, IMM_POOL,                   \
     JELLO_OPF_CAN_CALL | JELLO_OPF_CAN_ALLOC | JELLO_OPF_CAN_TRAP, op_call_native)                \
  OP(JOP_BYTES_READ_I64_LE, "BYTES_READ_I64_LE", REG, T_I64, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_i64_le)                                                     \
  OP(JOP_BYTES_READ_I64_BE, "BYTES_READ_I64_BE", REG, T_I64, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_i64_be)                                                     \
  OP(JOP_BYTES_READ_F64_LE, "BYTES_READ_F64_LE", REG, T_F64, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_f64_le)                                                     \
  OP(JOP_BYTES_READ_F64_BE, "BYTES_READ_F64_BE", REG, T_F64, REG, T_BYTES, REG, T_I32, IMM_NONE,   \
     JELLO_OPF_CAN_TRAP, op_bytes_read_f64_be)                                                     \
  OP(JOP_BYTES_WRITE_I64_LE, "BYTES_WRITE_I64_LE", REG, T_I64, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_i64_le)                                                    \
  OP(JOP_BYTES_WRITE_I64_BE, "BYTES_WRITE_I64_BE", REG, T_I64, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_i64_be)                                                    \
  OP(JOP_BYTES_WRITE_F64_LE, "BYTES_WRITE_F64_LE", REG, T_F64, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_f64_le)                                                    \
  OP(JOP_BYTES_WRITE_F64_BE, "BYTES_WRITE_F64_BE", REG, T_F64, REG, T_BYTES, REG, T_I32, IMM_NONE, \
     JELLO_OPF_CAN_TRAP, op_bytes_write_f64_be)                                                    \
  OP(JOP_BYTES_BLIT, "BYTES_BLIT", REG, T_BYTES, REG, T_BYTES, REG, T_I32, IMM_NONE,               \
     JELLO_OPF_CAN_TRAP, op_bytes_blit)                                                            \
  OP(JOP_DIV_U32, "DIV_U32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_CAN_TRAP,      \
     op_div_u32)                                                                                   \
  OP(JOP_MOD_U32, "MOD_U32", REG, POLY, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_CAN_TRAP,      \
     op_mod_u32)                                                                                   \
  OP(JOP_DIV_U64, "DIV_U64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_CAN_TRAP,     \
     op_div_u64)                                                                                   \
  OP(JOP_MOD_U64, "MOD_U64", REG, T_I64, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_CAN_TRAP,     \
     op_mod_u64)                                                                                   \
  OP(JOP_LT_U32, "LT_U32", REG, T_BOOL, REG, T_I32, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,          \
     op_lt_u32)                                                                                    \
  OP(JOP_LT_U64, "LT_U64", REG, T_BOOL, REG, T_I64, REG, T_I64, IMM_NONE, JELLO_OPF_NONE,          \
     op_lt_u64)                                                                                    \
  OP(JOP_ZEXT_I64, "ZEXT_I64", REG, T_I64, REG, T_I32, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,        \
     op_zext_i64)                                                                                  \
  OP(JOP_FMA_F64, "FMA_F64", REG, T_F64, REG, T_F64, REG, T_F64, REG, JELLO_OPF_NONE, op_fma_f64)  \
  OP(JOP_LDEXP_F64, "LDEXP_F64", REG, T_F64, REG, T_F64, REG, T_I32, IMM_NONE, JELLO_OPF_NONE,     \
     op_ldexp_f64)                                                                                 \
  OP(JOP_COPYSIGN_F64, "COPYSIGN_F64", REG, T_F64, REG, T_F64, REG, T_F64, IMM_NONE,               \
     JELLO_OPF_NONE, op_copysign_f64)                                                              \
  OP(JOP_GLOBAL_GET, "GLOBAL_GET", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_POOL, JELLO_OPF_NONE,      \
     op_global_get)                                                                                \
  OP(JOP_GLOBAL_SET, "GLOBAL_SET", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_POOL, JELLO_OPF_NONE,      \
     op_global_set)                                                                                \
  OP(JOP_BYTES_SLICE, "BYTES_SLICE", REG, T_BYTES, REG, T_BYTES, REG, T_I32, IMM_NONE,             \
     JELLO_OPF_CAN_ALLOC | JELLO_OPF_CAN_TRAP, op_bytes_slice)                                     \
  OP(JOP_IS_NULL, "IS_NULL", REG, T_BOOL, REG, POLY, UNUSED, 0, IMM_NONE, JELLO_OPF_NONE,          \
     op_is_null)                                                                                   \
  OP(JOP_NULL_CHECK, "NULL_CHECK", REG, POLY, UNUSED, 0, UNUSED, 0, IMM_NONE, JELLO_OPF_CAN_TRAP,  \
     op_null_check)                                                                                \
  OP(JOP_IS_ENUM, "IS_ENUM", REG, T_BOOL, REG, T_DYNAMIC, UNUSED, 0, IMM_RAW,                      \
     JELLO_OPF_TOUCHES_DYNAMIC, op_is_enum)                                                        \
  OP(JOP_BYTES_FREEZE, "BYTES_FREEZE", REG, T_BYTES, UNUSED, 0, UNUSED, 0, IMM_NONE,               \
     JELLO_OPF_CAN_TRAP, op_bytes_freeze)                                                          \
  OP(JOP_BYTES_JOIN, "BYTES_JOIN", REG, T_BYTES, REG, T_ARRAY, REG, T_BYTES, IMM_NONE,             \
     JELLO_OPF_CAN_TRAP | JELLO_OPF_CAN_ALLOC, op_bytes_join)

#define OP(id, name, ar, ak, br, bk, cr, ck, ir, fl, h) id,
typedef enum jello_op { JELLO_OP_LIST(OP) } jello_op;
#undef OP

/** The number of opcodes; JOP_BYTES_JOIN is the last in JELLO_OP_LIST. */
#define JOP_COUNT (JOP_BYTES_JOIN + 1)

/** Operand metadata of every opcode, indexed by jello_op. */
extern const jello_op_info jello_op_table[JOP_COUNT];

/** One instruction in its canonical 8-byte form, which the interpreter indexes directly. A
 * module with JELLO_BC_FEAT_COMPRESSED_INSN is expanded to this form as it loads. */
typedef struct jello_insn {
  uint8_t op;
  uint8_t a;
  uint8_t b;
  uint8_t c;
  uint32_t imm;
} jello_insn;

/** jello_bc_function::name_atom of an anonymous function. Hot reload matches functions across
 * compiles by name, so an anonymous function never matches; it is reported as removed or new. */
#define JELLO_BC_NO_NAME UINT32_MAX

/** One run of the line table (JELLO_BC_FEAT_LINE_TABLE); the instructions from `pc` up to the
 * next run's pc came from source file `file` (an index into the module's source_files), at a
 * 1-based line and code-point column. */
typedef struct jello_bc_location {
  uint32_t pc;
  uint32_t file;
  uint32_t line;
  uint32_t col;
} jello_bc_location;

/** One function of a loaded module. */
typedef struct jello_bc_function {
  uint32_t name_atom; /**< Atom index of the function's name, or JELLO_BC_NO_NAME. */
  uint32_t nargs;     /**< Declared parameter count, at most nregs. */
  uint32_t nregs;     /**< At most 256. */
  uint32_t cap_start; /**< First register that receives a closure's captures. */
  const jello_type_id* reg_types;
  uint32_t ninsns;
  const jello_insn* insns;
  /** Line table runs in increasing pc order; none without JELLO_BC_FEAT_LINE_TABLE. */
  uint32_t nlocs;
  const jello_bc_location* locs;
} jello_bc_function;

static inline int jello_bc_op_is_valid(uint32_t op) { return op < (uint32_t)JOP_COUNT; }

static inline int jello_bc_type_kind_is_valid(uint32_t kind) {
  return kind >= (uint32_t)JELLO_T_VOID && kind <= (uint32_t)JELLO_T_ENUM;
}

#endif /* JELLO_BYTECODE_H */
