// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jello_op_table; JELLO_OP_LIST expanded into operand metadata, with short local names for its
 * role and kind columns. The `handler` column is unused here; dispatch.c reads it. */

#include <jello/internal.h>

#define REG JELLO_OPR_REG
#define UNUSED JELLO_OPR_UNUSED
#define IMM_RAW JELLO_OPR_IMM_RAW
#define IMM_JUMP JELLO_OPR_IMM_JUMP
#define IMM_POOL JELLO_OPR_IMM_POOL
#define IMM_NONE JELLO_OPR_UNUSED
#define POLY JELLO_OPK_POLY
#define T_VOID JELLO_T_VOID
#define T_BOOL JELLO_T_BOOL
#define T_ATOM JELLO_T_ATOM
#define T_I8 JELLO_T_I8
#define T_I16 JELLO_T_I16
#define T_I32 JELLO_T_I32
#define T_I64 JELLO_T_I64
#define T_F16 JELLO_T_F16
#define T_F32 JELLO_T_F32
#define T_F64 JELLO_T_F64
#define T_BYTES JELLO_T_BYTES
#define T_LIST JELLO_T_LIST
#define T_ARRAY JELLO_T_ARRAY
#define T_STRUCT JELLO_T_STRUCT
#define T_OBJECT JELLO_T_OBJECT
#define T_FUNCTION JELLO_T_FUNCTION
#define T_ABSTRACT JELLO_T_ABSTRACT
#define T_DYNAMIC JELLO_T_DYNAMIC
#define T_ENUM JELLO_T_ENUM

#define OP(id, name, ar, ak, br, bk, cr, ck, ir, fl, h)                                            \
  {name,          (uint8_t)(ar), (uint8_t)(br), (uint8_t)(cr), (uint8_t)(ak),                      \
   (uint8_t)(bk), (uint8_t)(ck), (uint8_t)(ir), (uint8_t)(fl)},

const jello_op_info jello_op_table[JOP_COUNT] = {JELLO_OP_LIST(OP)};

#undef OP

#undef T_ENUM
#undef T_DYNAMIC
#undef T_ABSTRACT
#undef T_FUNCTION
#undef T_OBJECT
#undef T_STRUCT
#undef T_ARRAY
#undef T_LIST
#undef T_BYTES
#undef T_F64
#undef T_F32
#undef T_F16
#undef T_I64
#undef T_I32
#undef T_I16
#undef T_I8
#undef T_ATOM
#undef T_BOOL
#undef T_VOID
#undef POLY
#undef IMM_NONE
#undef IMM_POOL
#undef IMM_JUMP
#undef IMM_RAW
#undef UNUSED
#undef REG
