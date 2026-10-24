// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * x64 JIT emitter; compiles a function's JIR to SysV x86-64 code with hand-encoded
 * instructions, inline frame pushes for JIT-to-JIT calls, and shared return and trap tails.
 *
 * Registers outside the allocator's pool (RSI, RDI, R8-R11, R14 and xmm0-xmm5):
 * - RBX, R12, R13 and R15 are reserved (jit_impl.h).
 * - RAX, RCX and RDX are scratch within an instruction; C calls clobber them.
 * - RBP (X64_STAGE) holds a value across the C helper calls of one instruction. It is
 *   callee-saved, so the calls keep it, and never allocated, so writing it cannot clobber a
 *   vreg's register.
 * - xmm6 and xmm7 are floating-point scratch. */

#include <jello/internal/jit_internal.h>
#include <jello.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  X64_RAX = 0,
  X64_RCX = 1,
  X64_RDX = 2,
  X64_RBX = 3,
  X64_RSP = 4,
  X64_RBP = 5,
  X64_RSI = 6,
  X64_RDI = 7,
  X64_R8 = 8,
  X64_R9 = 9,
  X64_R10 = 10,
  X64_R11 = 11,
  X64_R14 = 14,
  X64_R15 = 15,
  X64_STAGE = X64_RBP,
};

static void emit_push_r64(jit_exec_buf* buf, unsigned reg) {
  if(reg >= 8u) {
    uint8_t bytes[2] = {0x41, (uint8_t)(0x50 + (reg & 7u))};
    jit_exec_buf_write(buf, bytes, sizeof(bytes));
  } else {
    uint8_t byte = (uint8_t)(0x50 + reg);
    jit_exec_buf_write(buf, &byte, 1);
  }
}

static void emit_pop_r64(jit_exec_buf* buf, unsigned reg) {
  if(reg >= 8u) {
    uint8_t bytes[2] = {0x41, (uint8_t)(0x58 + (reg & 7u))};
    jit_exec_buf_write(buf, bytes, sizeof(bytes));
  } else {
    uint8_t byte = (uint8_t)(0x58 + reg);
    jit_exec_buf_write(buf, &byte, 1);
  }
}

/** mov dst, src (REX.W 89 /r, mod=11). */
static void emit_mov_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  bytes[0] = (uint8_t)(0x48 | ((dst >= 8) ? 0x01 : 0) /* REX.B: rm */
                       | ((src >= 8) ? 0x04 : 0) /* REX.R: reg */);
  bytes[1] = 0x89;
  bytes[2] = (uint8_t)(0xC0 | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

/** mov dst32, src32; zero-extends into the full register. */
static void emit_mov_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  uint8_t bytes[6];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = (uint8_t)(0xB8u + (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r64_imm64(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  uint8_t bytes[10];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = (uint8_t)(0xB8u + (dst & 7u));
  memcpy(&bytes[2], &imm, 8);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_dword_rbx_disp_imm32(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  uint8_t bytes[10];
  bytes[0] = 0xC7;
  bytes[1] = 0x83;
  memcpy(&bytes[2], &disp, 4);
  memcpy(&bytes[6], &imm, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_dword_rbx_disp_r32(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  uint8_t bytes[10];
  size_t n = 0;
  // mov [rbx + disp32], src32; REX.R for r8-r15
  if(src >= 8) bytes[n++] = 0x44;
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x83u | ((src & 7u) << 3));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_qword_rbx_disp_r64(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0)); // REX.W, REX.R for r8-r15
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x83u | ((src & 7u) << 3));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_alu_r32_r32(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = opcode;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_alu_r64_r64(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = opcode;
  bytes[2] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_alu_r32_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  uint8_t bytes[7];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x81;
  bytes[n++] = (uint8_t)(0xC0u | ((op_ext & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_alu_r32_imm8(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint8_t imm) {
  uint8_t bytes[4];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x83;
  bytes[n++] = (uint8_t)(0xC0u | ((op_ext & 7u) << 3) | (dst & 7u));
  bytes[n++] = imm;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_neg_r32(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[3];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xF7;
  bytes[n++] = (uint8_t)(0xD8u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cmp_r32_r32(jit_exec_buf* buf, unsigned a, unsigned b) {
  // cmp a32, b32 (3B /r): flags from a - b
  uint8_t bytes[3];
  size_t n = 0;
  if((a >= 8) || (b >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x3B;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

/** cmp dst32, imm: 83 /7 ib when `imm` sign-extends from a byte, else 81 /7 id. */
static void emit_cmp_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  uint8_t bytes[7];
  size_t n = 0;
  int32_t v = (int32_t)imm;
  int short_imm = v >= -128 && v <= 127;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = short_imm ? 0x83 : 0x81;
  bytes[n++] = (uint8_t)(0xF8u | (dst & 7u));
  if(short_imm) {
    bytes[n++] = (uint8_t)imm;
  } else {
    memcpy(&bytes[n], &imm, 4);
    n += 4;
  }
  jit_exec_buf_write(buf, bytes, n);
}

/** cmp dword [base + disp], imm8 (83 /7 ib, sign-extended), with a disp8 when `disp` fits one.
 * `base` must not be RSP or R12, which would need a SIB byte. */
static void emit_cmp_m32_r64_disp_imm8(jit_exec_buf* buf, unsigned base, uint32_t disp,
                                       int8_t imm) {
  assert((base & 7u) != 4u);
  uint8_t bytes[8];
  size_t n = 0;
  if(base >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x83;
  if(disp <= 127u) {
    bytes[n++] = (uint8_t)(0x78u | (base & 7u));
    bytes[n++] = (uint8_t)disp;
  } else {
    bytes[n++] = (uint8_t)(0xB8u | (base & 7u));
    memcpy(&bytes[n], &disp, 4);
    n += 4;
  }
  bytes[n++] = (uint8_t)imm;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_setcc(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  // setcc dst8: REX for r8b-r15b, and for spl/bpl/sil/dil, which are ah/ch/dh/bh without it
  uint8_t bytes[4];
  size_t n = 0;
  if(dst >= 4) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = cc_opcode;
  bytes[n++] = (uint8_t)(0xC0u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movzx_r32_r8(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  size_t n = 0;
  if(dst >= 8 || src >= 4)
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xB6;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

/** dst32 = 1 when the flags satisfy `cc_opcode` (a setcc's second opcode byte), else 0. */
static void emit_setcc_from_flags(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  emit_setcc(buf, cc_opcode, dst);
  emit_movzx_r32_r8(buf, dst, dst);
}

static void emit_cdq(jit_exec_buf* buf) {
  uint8_t b = 0x99;
  jit_exec_buf_write(buf, &b, 1);
}

static void emit_cqo(jit_exec_buf* buf) {
  uint8_t bytes[2] = {0x48, 0x99};
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_idiv_r32(jit_exec_buf* buf, unsigned divisor) {
  uint8_t bytes[3];
  size_t n = 0;
  if(divisor >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xF7;
  bytes[n++] = (uint8_t)(0xF8u | (divisor & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_idiv_r64(jit_exec_buf* buf, unsigned divisor) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((divisor >= 8) ? 0x01u : 0));
  bytes[1] = 0xF7;
  bytes[2] = (uint8_t)(0xF8u | (divisor & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_shl_cl_r32(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[3];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xD3;
  bytes[n++] = (uint8_t)(0xE0u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_shr_cl_r32(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[3];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xD3;
  bytes[n++] = (uint8_t)(0xF8u | (dst & 7u)); // sar; the interpreter's >> is arithmetic
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_shl_cl_r64(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xD3;
  bytes[2] = (uint8_t)(0xE0u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_shr_cl_r64(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xD3;
  bytes[2] = (uint8_t)(0xF8u | (dst & 7u)); // SAR
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_test_r32_r32(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[3];
  size_t n = 0;
  if((a >= 8) || (b >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x85;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static size_t emit_jmp_rel32(jit_exec_buf* buf, int32_t rel) {
  size_t at = buf->len;
  uint8_t bytes[5];
  bytes[0] = 0xE9;
  memcpy(&bytes[1], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static size_t emit_jcc_rel32(jit_exec_buf* buf, uint8_t cc, int32_t rel) {
  size_t at = buf->len;
  uint8_t bytes[6];
  bytes[0] = 0x0F;
  bytes[1] = (uint8_t)(0x80u | (cc & 0xFu));
  memcpy(&bytes[2], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static void patch_jcc_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  memcpy(buf->mem + at + 2, &rel, 4);
}

/** A short jcc (7x rel8), for a forward skip its caller knows is under 128 bytes. */
static size_t emit_jcc_rel8(jit_exec_buf* buf, uint8_t cc) {
  size_t at = buf->len;
  uint8_t bytes[2] = {(uint8_t)(0x70u | (cc & 0xFu)), 0};
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

/** Points the short jcc at `at` to the end of the code. */
static void patch_jcc_rel8_here(jit_exec_buf* buf, size_t at) {
  size_t rel = buf->len - (at + 2);
  assert(rel <= 127u);
  buf->mem[at + 1] = (uint8_t)rel;
}

static void patch_jmp_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  memcpy(buf->mem + at + 1, &rel, 4);
}

/** Points the rel32 jcc at `at` to the end of the code. */
static void patch_jcc_rel32_here(jit_exec_buf* buf, size_t at) {
  patch_jcc_rel32(buf, at, (int32_t)(buf->len - (at + 6)));
}

/** Points the rel32 jmp at `at` to the end of the code. */
static void patch_jmp_rel32_here(jit_exec_buf* buf, size_t at) {
  patch_jmp_rel32(buf, at, (int32_t)(buf->len - (at + 5)));
}

static void emit_mov_eax_imm32(jit_exec_buf* buf, uint32_t imm) {
  uint8_t bytes[5];
  bytes[0] = 0xB8;
  memcpy(&bytes[1], &imm, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_neg_r64(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xF7;
  bytes[2] = (uint8_t)(0xD8u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_movsxd_r64_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[1] = 0x63;
  bytes[2] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_cmp_r64_r64(jit_exec_buf* buf, unsigned a, unsigned b) {
  // cmp a64, b64 (REX.W 3B /r): flags from a - b
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[1] = 0x3B;
  bytes[2] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_xor_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_alu_r32_imm32(buf, 0x06, dst, imm);
}

static void emit_movss_xmm_xmm(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_binss_xmm_xmm(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = op;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movsd_xmm_xmm(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_binsd_xmm_xmm(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = op;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_ucomiss(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[5];
  size_t n = 0;
  if((a >= 8) || (b >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2E;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_ucomisd(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((a >= 8) || (b >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2E;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

/** cvtsi2ss xmm, gpr (F3 0F 2A); `wide` selects the 64-bit source with REX.W. */
static void emit_cvtsi2ss(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(wide || xmm >= 8 || gpr >= 8)
    bytes[n++] =
        (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2A;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtsi2sd(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if(wide || xmm >= 8 || gpr >= 8)
    bytes[n++] =
        (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2A;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

/** cvttss2si gpr, xmm (F3 0F 2C), truncating; `wide` selects the 64-bit result with REX.W. */
static void emit_cvttss2si(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(wide || gpr >= 8 || xmm >= 8)
    bytes[n++] =
        (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((gpr >= 8) ? 0x04u : 0) | ((xmm >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2C;
  bytes[n++] = (uint8_t)(0xC0u | ((gpr & 7u) << 3) | (xmm & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvttsd2si(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if(wide || gpr >= 8 || xmm >= 8)
    bytes[n++] =
        (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((gpr >= 8) ? 0x04u : 0) | ((xmm >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2C;
  bytes[n++] = (uint8_t)(0xC0u | ((gpr & 7u) << 3) | (xmm & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtss2sd(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x5A;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtsd2ss(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x5A;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movabs_rax(jit_exec_buf* buf, uint64_t imm) {
  uint8_t bytes[10];
  bytes[0] = 0x48;
  bytes[1] = 0xB8;
  memcpy(&bytes[2], &imm, 8);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

/** An indirect call or jmp through `reg`; FF with ModRM reg field `ext` (/2 call, /4 jmp). */
static void emit_ff_r64(jit_exec_buf* buf, uint8_t ext, unsigned reg) {
  uint8_t bytes[3];
  size_t n = 0;
  if(reg >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xFF;
  bytes[n++] = (uint8_t)(0xC0u | ((unsigned)ext << 3) | (reg & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_call_r64(jit_exec_buf* buf, unsigned reg) { emit_ff_r64(buf, 2, reg); }

static void emit_jmp_r64(jit_exec_buf* buf, unsigned reg) { emit_ff_r64(buf, 4, reg); }

static void emit_ret(jit_exec_buf* buf) {
  uint8_t byte = 0xC3;
  jit_exec_buf_write(buf, &byte, 1);
}

static void emit_call_fn(jit_exec_buf* buf, uintptr_t fn);
static void emit_imul_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src);
static void emit_shl_r64_imm8(jit_exec_buf* buf, unsigned dst, uint8_t imm8);
static void emit_imul_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm);

/** Calls the C function `fn` with `movabs rax, fn; call rax`; the executable buffer is not
 * guaranteed to lie within rel32 reach of the C helpers. `fn` takes the System V convention:
 * it is declared JELLO_JIT_ABI, or is JIT_C of a function that is not. */
static void emit_call_fn(jit_exec_buf* buf, uintptr_t fn) {
  emit_movabs_rax(buf, (uint64_t)fn);
  emit_call_r64(buf, X64_RAX);
#ifdef JELLO_JIT_SCRIBBLE_CALLS
  /* A checking build overwrites every caller-saved general register but RAX after each C call,
   * so code that keeps a value in one across a call fails every time, not only when the callee
   * happens to use it. C helpers therefore return in RAX alone. */
  static const unsigned scribbled[] = {X64_RCX, X64_RDX, X64_RSI, X64_RDI,
                                       X64_R8,  X64_R9,  X64_R10, X64_R11};
  for(size_t i = 0; i < sizeof scribbled / sizeof scribbled[0]; i++)
    emit_mov_r64_imm64(buf, scribbled[i], 0x5c5c5c5c5c5c5c5cull);
#endif
}

/** The System V thunks JIT_C names on Windows. */
#ifdef JELLO_JIT_SYSV_THUNKS
void* jit_sysv_memcpy(void* dst, const void* src, size_t n) { return memcpy(dst, src, n); }
void* jit_sysv_memmove(void* dst, const void* src, size_t n) { return memmove(dst, src, n); }
int jit_sysv_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
jello_box_f64* jit_sysv_jello_box_f64_new(jello_vm* vm, double v) {
  return jello_box_f64_new(vm, v);
}
jello_object* jit_sysv_jello_object_new(jello_vm* vm, uint32_t type_id) {
  return jello_object_new(vm, type_id);
}
jello_object* jit_sysv_jello_object_copy(jello_vm* vm, const jello_object* src) {
  return jello_object_copy(vm, src);
}
int jit_sysv_jello_object_has(const jello_object* o, uint32_t atom_id) {
  return jello_object_has(o, atom_id);
}
jello_struct* jit_sysv_jello_struct_new(jello_vm* vm, const jello_bc_module* m,
                                        uint32_t struct_sig_id) {
  return jello_struct_new(vm, m, struct_sig_id);
}
jello_value jit_sysv_vm_box_from_typed(jello_vm* vm, const jello_bc_module* m,
                                       const jello_bc_function* f, reg_frame* rf, uint32_t r) {
  return vm_box_from_typed(vm, m, f, rf, r);
}
int jit_sysv_vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                                 reg_frame* rf, uint32_t dst, jello_value v) {
  return vm_store_from_boxed(vm, m, f, rf, dst, v);
}
jello_bytes* jit_sysv_vm_buffer_new(jello_vm* vm, uint32_t length) {
  return vm_buffer_new(vm, length);
}
uint32_t jit_sysv_vm_dynamic_is_enum(jello_value v, jello_type_id type) {
  return vm_dynamic_is_enum(v, type);
}
uint32_t jit_sysv_vm_kindof_dynamic(jello_value v) { return vm_kindof_dynamic(v); }
float jit_sysv_vm_f16_to_f32(uint16_t bits) { return vm_f16_to_f32(bits); }
uint16_t jit_sysv_vm_f32_to_f16(float f) { return vm_f32_to_f16(f); }
void jit_sysv_vm_pop_frame(jello_vm* vm) { vm_pop_frame(vm); }
int jit_sysv_vm_try_push(jello_vm* vm, uint32_t catch_pc, uint32_t payload_reg) {
  return vm_try_push(vm, catch_pc, payload_reg);
}
void jit_sysv_vm_try_pop(jello_vm* vm) { vm_try_pop(vm); }
void jit_sysv_vm_safepoint_check(jello_vm* vm) { vm_safepoint_check(vm); }
int jit_sysv_vm_reductions_out(jello_vm* vm) { return vm_reductions_out(vm); }
#endif

/** mov dst64, [r12 + disp32]: a jello_vm field. rm=100 means "SIB follows" even with REX.B, so
 * an R12 base always takes a SIB byte (0x24: no index, base=100); REX.B is always present, as it
 * is what selects R12 over RSP. The other `r12_disp` helpers share this shape. */
static void emit_mov_r64_from_r12_disp(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x49u | ((dst >= 8) ? 0x04u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r32_from_r12_disp(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x41u | ((dst >= 8) ? 0x04u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** cmp a, [r12 + disp32] (3B /r), of 32 or, when `wide`, 64 bits: flags from `a` minus a
 * jello_vm field. */
static void emit_cmp_r_r12_disp(jit_exec_buf* buf, unsigned a, uint32_t disp, int wide) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x41u | (wide ? 0x08u : 0) | ((a >= 8) ? 0x04u : 0));
  bytes[n++] = 0x3B;
  bytes[n++] = (uint8_t)(0x80u | ((a & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** sub dword [r12 + disp32], imm8 (83 /5 ib, sign-extended): a jello_vm field less `imm`, with the
 * flags of the result. */
static void emit_sub_m32_r12_disp_imm8(jit_exec_buf* buf, uint32_t disp, int8_t imm) {
  uint8_t bytes[9] = {0x41, 0x83, 0xAC, 0x24};
  memcpy(&bytes[4], &disp, 4);
  bytes[8] = (uint8_t)imm;
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_store_r32_to_r12_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x41u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** mov dword [r12 + disp32], imm32 (41 C7 84 24): stores a constant into a jello_vm field. */
static void emit_store_imm32_to_vm_field(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  uint8_t bytes[12];
  size_t n = 0;
  bytes[n++] = 0x41;
  bytes[n++] = 0xC7;
  bytes[n++] = 0x84;
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r64_from_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base,
                                       uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r32_from_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base,
                                       uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  if((dst >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r32_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base,
                                       uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  if((src >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r64_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base,
                                       uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r32_to_r13_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  // mov [r13 + disp32], src32: REX.B always, since without it rm=101 means rbp
  bytes[n++] = (uint8_t)(0x41u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r64_to_r13_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_add_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0x01;
  bytes[2] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

/** A jmp or jcc (`is_jcc`) at `at` whose rel32 is patched to bytecode pc `bc_tgt`'s code once
 * the whole function is emitted. */
typedef struct jit_branch_patch {
  size_t at;
  uint32_t bc_tgt;
  uint8_t is_jcc;
} jit_branch_patch;

/** Which shared tail a trap site jumps to. Most kinds have a tail that calls a jit_trap_* setter
 * for vm->trap_code; the site itself records the trap location. */
typedef enum jit_trap_kind {
  JIT_TRAP_ARITH = 0,
  JIT_TRAP_NULL,
  JIT_TRAP_BOUNDS,
  JIT_TRAP_TYPE,
  JIT_TRAP_PASSTHROUGH, /**< Code and location already recorded, by a callee or jit_run_handler. */
  JIT_TRAP_CALLEE,      /**< A callee's trap; the site records the call in the trace. */
  JIT_TRAP_HELPER,      /**< A C helper set the code; the site records only the location. */
  JIT_TRAP_LIMIT,
  JIT_TRAP_KIND_COUNT,
} jit_trap_kind;

typedef struct jit_trap_jump {
  size_t at;
  uint8_t kind; /**< A jit_trap_kind. */
} jit_trap_jump;

typedef struct emit_ctx {
  jit_exec_buf* buf;
  jit_reg_state* ra;
  const jello_bc_module* m;
  const jello_bc_function* f;
  uint32_t fi;
  uint32_t cur_bc_pc; /**< bc_pc of the instruction being emitted; trap sites record it. */
  jit_trap_jump* trap_jumps;
  size_t trap_jump_cap;
  size_t trap_jump_count;
  jit_branch_patch* branches;
  size_t branch_cap;
  size_t branch_count;
  size_t* ret_jumps;
  size_t ret_jump_cap;
  size_t ret_jump_count;
  size_t* stack_overflow_jumps;
  size_t stack_overflow_jump_cap;
  size_t stack_overflow_jump_count;
  const void* const* entries; /**< Each function's call target; NULL declines every call. */
  /** Handler pcs of this function's TRYs, each once; the passthrough trap exit first checks
   * whether a throw is unwinding to one of them. */
  uint32_t* catch_pcs;
  uint32_t ncatch;
  jello_type_kind ret_kind;
  /** Whether any call site holds a value in JIT_REG_SURVIVE. If so, every activation saves R15
   * in a 16-byte stack slot at body_off and every exit path restores it. */
  int survive_any_armed;
  /** Set when a site list could not grow; an unrecorded site would never be patched, so the
   * function is not compiled. */
  int oom;
} emit_ctx;

/** Site lists grow as they fill; recording fails, setting `oom`, only when memory runs out. */
static void push_trap_jump(emit_ctx* ctx, size_t at, uint8_t kind) {
  jit_trap_jump t = {at, kind};
  if(!jit_list_push(&ctx->trap_jumps, &ctx->trap_jump_count, &ctx->trap_jump_cap, &t, sizeof(t)))
    ctx->oom = 1;
}

static void push_branch(emit_ctx* ctx, size_t at, uint32_t bc_tgt, uint8_t is_jcc) {
  jit_branch_patch b = {at, bc_tgt, is_jcc};
  if(!jit_list_push(&ctx->branches, &ctx->branch_count, &ctx->branch_cap, &b, sizeof(b)))
    ctx->oom = 1;
}

/** Records the trap location and jumps to the stack-overflow tail. Callers branch around it, so
 * it runs only on the overflow path. */
static void emit_jmp_stack_overflow(emit_ctx* ctx) {
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  size_t at = emit_jmp_rel32(ctx->buf, 0);
  if(!jit_list_push(&ctx->stack_overflow_jumps, &ctx->stack_overflow_jump_count,
                    &ctx->stack_overflow_jump_cap, &at, sizeof(at)))
    ctx->oom = 1;
}

/** Branches, through the placeholder it returns, when RSP is at or above
 * vm->native_stack_limit, so a call may nest another activation on the native stack. */
static size_t emit_branch_if_native_room(jit_exec_buf* buf) {
  emit_cmp_r_r12_disp(buf, X64_RSP, (uint32_t)offsetof(jello_vm, native_stack_limit), 1);
  return emit_jcc_rel32(buf, 0x83, 0); // JAE: at or above the limit
}

/** Traps STACK_OVERFLOW when RSP is below vm->native_stack_limit, before a call nests another
 * activation on the native stack. */
static void emit_native_stack_check(emit_ctx* ctx) {
  size_t fits = emit_branch_if_native_room(ctx->buf);
  emit_jmp_stack_overflow(ctx);
  patch_jcc_rel32_here(ctx->buf, fits);
}

/** Points `target`, a call's target, at the bridge when RSP is below vm->native_stack_limit, so
 * that a call nested too deep for the native stack runs its callee in the interpreter, which
 * nests no native frame per call, rather than trapping. The bridge is the entry past the
 * functions'. */
static void emit_bridge_when_deep(emit_ctx* ctx, unsigned target) {
  jit_exec_buf* buf = ctx->buf;
  size_t fits = emit_branch_if_native_room(buf);
  emit_mov_r64_imm64(buf, target, (uint64_t)(uintptr_t)&ctx->entries[ctx->m->nfuncs]);
  emit_mov_r64_from_r64_disp(buf, target, target, 0);
  patch_jcc_rel32_here(buf, fits);
}

static void patch_stack_overflow_jumps(emit_ctx* ctx, size_t so_off) {
  for(size_t i = 0; i < ctx->stack_overflow_jump_count; i++) {
    size_t at = ctx->stack_overflow_jumps[i];
    patch_jmp_rel32(ctx->buf, at, (int32_t)(so_off - (at + 5)));
  }
}

/** Traps to the `kind` tail when condition `cc` (a jcc's second opcode byte) holds. The trapping
 * path records ctx->fi and cur_bc_pc in vm->trap_func_index/trap_pc, then jumps to the tail; the
 * inverted condition (`cc ^ 1`) skips it. PASSTHROUGH and CALLEE record no location, so a callee's
 * trap keeps its own; CALLEE records this call as the frame's site in the trace instead. */
static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc, uint8_t kind) {
  size_t skip = emit_jcc_rel32(ctx->buf, (uint8_t)(cc ^ 1u), 0);
  if(kind == JIT_TRAP_CALLEE) {
    emit_store_imm32_to_vm_field(ctx->buf, JIT_TRAP_SITE_OFF, ctx->cur_bc_pc + 1u);
  } else if(kind != JIT_TRAP_PASSTHROUGH) {
    emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
    emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  }
  size_t at = emit_jmp_rel32(ctx->buf, 0);
  patch_jcc_rel32(ctx->buf, skip, (int32_t)(ctx->buf->len - (skip + 6)));
  push_trap_jump(ctx, at, kind);
}

static void patch_trap_jumps(emit_ctx* ctx, const size_t* trap_off_by_kind) {
  for(size_t i = 0; i < ctx->trap_jump_count; i++) {
    size_t at = ctx->trap_jumps[i].at;
    size_t trap_off = trap_off_by_kind[ctx->trap_jumps[i].kind];
    uint8_t op = ctx->buf->mem[at];
    if(op == 0xE9) patch_jmp_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 5)));
    else patch_jcc_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 6)));
  }
}

static void push_ret_jump(emit_ctx* ctx, size_t at) {
  if(!jit_list_push(&ctx->ret_jumps, &ctx->ret_jump_count, &ctx->ret_jump_cap, &at, sizeof(at)))
    ctx->oom = 1;
}

/** Traps ARITH on a zero divisor and on INT32_MIN / -1, which idiv would fault on. */
static void emit_divmod_guard_i32(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test_r32_r32(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  emit_cmp_r32_imm32(ctx->buf, div_gpr, (uint32_t)-1);
  size_t jne = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, 0x85, 0);
  emit_cmp_r32_imm32(ctx->buf, lhs_gpr, (uint32_t)INT32_MIN);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_jcc_rel32(ctx->buf, jne, (int32_t)(ok - (jne + 6)));
}

static void emit_test_r64_r64(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[1] = 0x85;
  bytes[2] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_cmp_r64_imm8(jit_exec_buf* buf, unsigned reg, int8_t imm) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((reg >= 8) ? 0x01u : 0));
  bytes[1] = 0x83;
  bytes[2] = (uint8_t)(0xF8u | (reg & 7u));
  bytes[3] = (uint8_t)imm;
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_divmod_guard_i64(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test_r64_r64(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  emit_cmp_r64_imm8(ctx->buf, div_gpr, (int8_t)-1);
  size_t jne = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, 0x85, 0);
  // INT64_MIN needs a register operand; RAX is reloaded from lhs for the idiv anyway
  emit_mov_r64_imm64(ctx->buf, X64_RAX, (uint64_t)INT64_MIN);
  emit_cmp_r64_r64(ctx->buf, lhs_gpr, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_jcc_rel32(ctx->buf, jne, (int32_t)(ok - (jne + 6)));
}

static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8);
static void emit_store_r64_to_mem_rsp(jit_exec_buf* buf, unsigned src, uint8_t disp);
static void emit_mov_r64_from_mem_rsp(jit_exec_buf* buf, unsigned dst, uint8_t disp);

/** Emits the entry prologue; saves RBX, R12 and R13 and loads them from the SysV arguments, and
 * saves R14 and X64_STAGE, which the code below may also write. `*out_body_off` is set past it,
 * where JIT-to-JIT calls and the enter stub start; RBX, R12 and R13 hold for the whole call
 * chain, and no caller keeps a value in R14 or X64_STAGE across a call. The five pushes leave RSP
 * 16-byte aligned at body_off, which is where every activation expects it. JIT_REG_SURVIVE is
 * not pushed; only a function with `any_armed` writes it, and that body saves it, per activation,
 * in a 16-byte slot that keeps RSP aligned for C calls, so it is saved once on every path. */
static void emit_prologue(jit_exec_buf* buf, size_t* out_body_off, int any_armed) {
  emit_push_r64(buf, (unsigned)JIT_REG_VM);
  emit_push_r64(buf, (unsigned)JIT_REG_BASE);
  emit_push_r64(buf, (unsigned)JIT_REG_OUT);
  emit_push_r64(buf, X64_R14);
  emit_push_r64(buf, X64_STAGE);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_VM, X64_RDI);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_BASE, X64_RSI);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_OUT, X64_RDX);
  *out_body_off = buf->len;
  if(any_armed) {
    emit_rsp_adjust_local(buf, 0, 16); // sub rsp, 16
    emit_store_r64_to_mem_rsp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    // Every C helper call needs RSP 16-byte aligned; a misaligned one stops here, at a UD2.
    static const uint8_t check[] = {0x48, 0xF7, 0xC4, 0x0F, 0x00, 0x00, 0x00, // test rsp, 0xf
                                    0x74, 0x02,                               // je +2
                                    0x0F, 0x0B};                              // ud2
    jit_exec_buf_write(buf, check, sizeof(check));
  }
}

static void emit_restore_callee_saved(jit_exec_buf* buf, int any_armed) {
  if(any_armed) {
    emit_mov_r64_from_mem_rsp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_rsp_adjust_local(buf, 1, 16); // add rsp, 16
  }
  emit_pop_r64(buf, X64_STAGE);
  emit_pop_r64(buf, X64_R14);
  emit_pop_r64(buf, (unsigned)JIT_REG_OUT);
  emit_pop_r64(buf, (unsigned)JIT_REG_BASE);
  emit_pop_r64(buf, (unsigned)JIT_REG_VM);
}

/** Writes `ret_vreg` to the C caller's jello_numeric_result (RBX) on the outermost return. */
static void emit_store_out(emit_ctx* ctx, jello_type_kind ret_kind, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                (uint32_t)JELLO_NUM_VOID);
  if(ret_kind == JELLO_T_BOOL) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_BOOL);
    emit_mov_dword_rbx_disp_r32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_I32);
    emit_mov_dword_rbx_disp_r32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_I64);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_F32);
    emit_mov_dword_rbx_disp_r32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_F64);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_BYTES || ret_kind == JELLO_T_STRUCT || ret_kind == JELLO_T_OBJECT) {
    // A raw pointer, possibly NULL, as the interpreter's outermost RET returns it.
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_PTR);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_DYNAMIC) {
    // The tagged jello_value word, unchanged, as the interpreter returns it.
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                                  (uint32_t)JELLO_NUM_DYN);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  }
}

/** The outermost return; pops the frame, restores the saved registers and returns
 * JELLO_EXEC_OK to the C caller. */
static void emit_epilogue_ok_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_pop_frame));
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_OK);
  emit_ret(buf);
}

static void emit_trap_tail_x64(emit_ctx* ctx);
static void emit_alu_r64_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm);

/** A trap tail; calls `trap_fn` (a jit_trap_* setter, or 0 when the code is already set),
 * then unwinds. */
static void emit_epilogue_trap_generic(emit_ctx* ctx, uintptr_t trap_fn) {
  jit_exec_buf* buf = ctx->buf;
  if(trap_fn) {
    emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, trap_fn);
  }
  emit_trap_tail_x64(ctx);
}

static void emit_epilogue_stack_overflow(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_stack_overflow);
  emit_trap_tail_x64(ctx);
}

static void emit_movzx_r32_m8_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base,
                                       uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  if((dst >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xB6;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movzx_r32_m16_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base,
                                        uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  if((dst >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xB7;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** mov byte [base + disp32], src8. Any src of 4-7 takes a REX prefix, since without one those
 * encodings mean AH/CH/DH/BH, and the register pool includes RSI and RDI. */
static void emit_store_r8_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base,
                                      uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  if((src >= 4) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x88;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** mov byte [base + disp32], imm8 (C6 /0 ib). `base` must not be RSP or R12, which would need a
 * SIB byte. */
static void emit_store_imm8_to_r64_disp(jit_exec_buf* buf, unsigned base, uint32_t disp,
                                        uint8_t imm) {
  assert((base & 7u) != 4u);
  uint8_t bytes[8];
  size_t n = 0;
  if(base >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xC6;
  bytes[n++] = (uint8_t)(0x80u | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  bytes[n++] = imm;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r16_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base,
                                       uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((src >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_bswap_r32(jit_exec_buf* buf, unsigned reg) {
  uint8_t bytes[3];
  size_t n = 0;
  if(reg >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x0F;
  bytes[n++] = (uint8_t)(0xC8u | (reg & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_bswap_r64(jit_exec_buf* buf, unsigned reg) {
  uint8_t bytes[3];
  bytes[0] = (uint8_t)(0x48u | ((reg >= 8) ? 0x01u : 0));
  bytes[1] = 0x0F;
  bytes[2] = (uint8_t)(0xC8u | (reg & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

/** rol reg16, imm8 (66 C1 /0). With 8 it swaps the low two bytes, as bswap has no 16-bit form. */
static void emit_rol16_imm8(jit_exec_buf* buf, unsigned reg, uint8_t imm8) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0x66;
  if(reg >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xC1;
  bytes[n++] = (uint8_t)(0xC0u | (reg & 7u));
  bytes[n++] = imm8;
  jit_exec_buf_write(buf, bytes, n);
}

/** movd xmm, gpr32 (66 0F 6E); copies bits, not a numeric conversion. The movd/movq helpers
 * carry raw IEEE-754 patterns between Bytes memory and float registers. */
static void emit_movd_xmm_r32(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((xmm >= 8) || (gpr >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x6E;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movd_r32_xmm(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((xmm >= 8) || (gpr >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x7E;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movq_xmm_r64(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  uint8_t bytes[6];
  bytes[0] = 0x66;
  bytes[1] = (uint8_t)(0x48u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[2] = 0x0F;
  bytes[3] = 0x6E;
  bytes[4] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, 5);
}

static void emit_movq_r64_xmm(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  uint8_t bytes[6];
  bytes[0] = 0x66;
  bytes[1] = (uint8_t)(0x48u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[2] = 0x0F;
  bytes[3] = 0x7E;
  bytes[4] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, 5);
}

static void emit_sub_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x29;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

/** Points JIT_REG_BASE at the top frame's registers and leaves that call_frame's address in R8.
 * Clobbers RAX, RCX and RDX. */
static void emit_reload_jit_reg_base_x64(jit_exec_buf* buf) {
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_R8, X64_RCX);
  emit_mov_r64_from_r64_disp(buf, (unsigned)JIT_REG_BASE, X64_R8,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, mem));
}

/** Pops the trapping frame and returns JELLO_EXEC_TRAP; to the JIT caller, whose post-call check
 * propagates the trap, or from the outermost activation to C. */
static void emit_trap_tail_x64(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_R8, X64_RCX);
  emit_movzx_r32_m8_r64_disp(buf, X64_RDX, X64_R8, (uint32_t)offsetof(call_frame, has_caller));
  emit_test_r32_r32(buf, X64_RDX, X64_RDX);
  size_t j_boundary = emit_jcc_rel32(buf, 0x84, 0);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_reload_jit_reg_base_x64(buf);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  // Undo this activation's R15 save, or `ret` pops it as the return address.
  if(ctx->survive_any_armed) {
    emit_mov_r64_from_mem_rsp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_rsp_adjust_local(buf, 1, 16);
  }
  emit_ret(buf);

  size_t boundary = buf->len;
  patch_jcc_rel32(buf, j_boundary, (int32_t)(boundary - (j_boundary + 6)));
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret(buf);
}

/** RET. A nested activation stores the value in its caller's `caller_dst` register, pops its
 * frame and returns to the JIT caller; the outermost one writes the result and jumps to the OK
 * tail. */
static void emit_ret_insn(emit_ctx* ctx, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = jello_slot_size(rk) == 8u; // pointers too, not only I64/F64
  int ret_gpr = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);

  /* The frame lookup below clobbers RAX, RCX, RDX and R8, and the value may be in R8; move it to
   * R9 and rebind the vreg there, so emit_store_out reads the copy. */
  if(wide) emit_mov_r64_r64(buf, X64_R9, (unsigned)ret_gpr);
  else emit_mov_r32_r32(buf, X64_R9, (unsigned)ret_gpr);
  jit_reg_bind(ctx->ra, ret_vreg, X64_R9);
  ret_gpr = X64_R9;

  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_R8, X64_RCX);
  emit_movzx_r32_m8_r64_disp(buf, X64_RDX, X64_R8, (uint32_t)offsetof(call_frame, has_caller));
  emit_test_r32_r32(buf, X64_RDX, X64_RDX);
  size_t j_boundary = emit_jcc_rel32(buf, 0x84, 0);

  /* caller_dst must be read from this frame (R8) before it is popped; the caller's frame has
   * its own caller_dst, for its own return. */
  emit_mov_r32_from_r64_disp(buf, X64_R10, X64_R8, (uint32_t)offsetof(call_frame, caller_dst));

  if(wide) emit_mov_r64_r64(buf, X64_R11, (unsigned)ret_gpr);
  else emit_mov_r32_r32(buf, X64_R11, (unsigned)ret_gpr);

  /* A frame without pop_work or spill slots, the common case, pops inline as vm_pop_frame
   * would; the caller's frame is the call_frame before it. */
  emit_mov_r32_from_r64_disp(buf, X64_RAX, X64_R8, (uint32_t)offsetof(call_frame, pop_work));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  size_t j_try = emit_jcc_rel32(buf, 0x85, 0);
  emit_mov_r64_from_r64_disp(buf, X64_RAX, X64_R8, (uint32_t)offsetof(call_frame, spill_slots));
  emit_test_r64_r64(buf, X64_RAX, X64_RAX);
  size_t j_spill = emit_jcc_rel32(buf, 0x85, 0);
  emit_mov_r32_from_r64_disp(buf, X64_RCX, X64_R8,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, size));
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_sub_r32_r32(buf, X64_RAX, X64_RCX);
  emit_store_r32_to_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_alu_r32_imm32(buf, 5, X64_RAX, 1u); // sub eax, 1
  emit_store_r32_to_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_alu_r64_imm32(buf, 5, X64_R8, (uint32_t)sizeof(call_frame)); // sub r8, imm32
  emit_mov_r64_from_r64_disp(buf, (unsigned)JIT_REG_BASE, X64_R8,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, mem));
  size_t j_popped = emit_jmp_rel32(buf, 0);
  patch_jcc_rel32(buf, j_try, (int32_t)(buf->len - (j_try + 6)));
  patch_jcc_rel32(buf, j_spill, (int32_t)(buf->len - (j_spill + 6)));

  /* R10 and R11 are caller-saved, so they are pushed across vm_pop_frame; two pushes keep RSP
   * 16-byte aligned. */
  emit_push_r64(buf, X64_R10);
  emit_push_r64(buf, X64_R11);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_pop_frame));
  emit_pop_r64(buf, X64_R11);
  emit_pop_r64(buf, X64_R10);

  // After the pop the top frame is the caller's; restore JIT_REG_BASE and store into caller_dst.
  emit_reload_jit_reg_base_x64(buf);
  patch_jmp_rel32(buf, j_popped, (int32_t)(buf->len - (j_popped + 5)));
  emit_imul_r32_imm32(buf, X64_R10, REG_SLOT_BYTES);
  emit_mov_r64_r64(buf, X64_R8, (unsigned)JIT_REG_BASE);
  emit_add_r64_r64(buf, X64_R8, X64_R10);
  if(wide) emit_store_r64_to_r64_disp(buf, X64_R11, X64_R8, 0);
  else emit_store_r32_to_r64_disp(buf, X64_R11, X64_R8, 0);

  /* A nested activation entered at body_off and pushed none of the prologue's registers, so it
   * does not pop them; it only undoes its R15 survive slot, which is per activation. */
  if(ctx->survive_any_armed) {
    emit_mov_r64_from_mem_rsp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_rsp_adjust_local(buf, 1, 16); // add rsp, 16
  }
  emit_ret(buf);

  size_t boundary = buf->len;
  patch_jcc_rel32(buf, j_boundary, (int32_t)(boundary - (j_boundary + 6)));

  emit_store_out(ctx, rk, ret_vreg);
  size_t at = emit_jmp_rel32(buf, 0);
  push_ret_jump(ctx, at);
}

static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8);

/** Calls JIT code at `target`, a register. The 8-byte pad plus the return address keep
 * RSP 16-byte aligned at the callee's body_off, which has no pushes of its own, for the C calls
 * it makes. */
static void emit_call_jit_code(jit_exec_buf* buf, unsigned target) {
  emit_rsp_adjust_local(buf, 0, 8);
  emit_call_r64(buf, target);
  emit_rsp_adjust_local(buf, 1, 8);
}

/** After a JIT call; a callee's trap returns with vm->trap_code set, which passes through rather
 * than continuing; otherwise the result is reloaded from `dst`'s home. */
static void emit_after_jit_call(emit_ctx* ctx, uint8_t dst) {
  emit_mov_r32_from_r12_disp(ctx->buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test_r32_r32(ctx->buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_CALLEE);
  jit_reg_state_discard_currents(ctx->ra);
  (void)jit_reg_get_gpr(ctx->ra, dst, 1);
}

/** Fills call_frames[call_frames_len] for a callee frame whose registers are at R11, as
 * push_frame_shell fills it, and stores call_frames_len + 1. A known callee `f` comes with its
 * index `fi`, `has_pointer` and `total`; for a NULL one they are read from the frame_layout in
 * X64_STAGE, and its index from R9. Clobbers RAX, RCX, RDX and R8. */
static void emit_fill_call_frame(jit_exec_buf* buf, const jello_bc_function* f, uint32_t fi,
                                 int has_pointer, uint32_t total, uint32_t caller_dst) {
  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_r32(buf, X64_R8, X64_RCX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_RAX, X64_R8);

  if(f) emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)f);
  else emit_mov_r64_from_r64_disp(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, f));
  emit_store_r64_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, f));
  unsigned fi_reg = X64_R9;
  if(f) {
    emit_mov_r32_imm32(buf, X64_RDX, fi);
    fi_reg = X64_RDX;
  }
  emit_store_r32_to_r64_disp(buf, fi_reg, X64_RAX, (uint32_t)offsetof(call_frame, func_index));
  emit_mov_r32_imm32(buf, X64_RDX, 0u);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, pc));
  // has_caller and has_pointer are single bytes; a wider store would overrun into the next field.
  emit_store_imm8_to_r64_disp(buf, X64_RAX, (uint32_t)offsetof(call_frame, has_caller), 1u);
  if(f) {
    emit_store_imm8_to_r64_disp(buf, X64_RAX, (uint32_t)offsetof(call_frame, has_pointer),
                                (uint8_t)has_pointer);
  } else {
    emit_mov_r32_from_r64_disp(buf, X64_RDX, X64_STAGE,
                               (uint32_t)offsetof(frame_layout, has_pointer));
    emit_store_r8_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, has_pointer));
  }
  emit_mov_r32_imm32(buf, X64_RDX, caller_dst);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, caller_dst));

  emit_mov_r64_from_r64_disp(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, kind));
  emit_store_r64_to_r64_disp(buf, X64_RDX, X64_RAX,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, kind));
  emit_mov_r64_from_r64_disp(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, wide));
  emit_store_r64_to_r64_disp(buf, X64_RDX, X64_RAX,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, wide));
  if(f) emit_mov_r32_imm32(buf, X64_RDX, total);
  else emit_mov_r32_from_r64_disp(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, total));
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, size));
  emit_store_r64_to_r64_disp(buf, X64_R11, X64_RAX,
                             (uint32_t)offsetof(call_frame, rf) +
                                 (uint32_t)offsetof(reg_frame, mem));

  emit_alu_r32_imm32(buf, 0, X64_RCX, 1u); // add ecx, 1
  emit_store_r32_to_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
}

/** Moves JIT_REG_BASE to the callee frame at R11 and copies `nargs` arguments into it from the
 * caller's registers from `arg_first`, at the widths of `kinds_f`'s registers from
 * `kinds_first`. Clobbers RAX and R8. */
static void emit_copy_args(emit_ctx* ctx, const jello_bc_function* kinds_f, uint32_t kinds_first,
                           uint32_t arg_first, uint32_t nargs) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_R8, (unsigned)JIT_REG_BASE);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_BASE, X64_R11);
  for(uint32_t ai = 0; ai < nargs; ai++) {
    uint32_t src_disp = (arg_first + ai) * REG_SLOT_BYTES;
    uint32_t dst_disp = ai * REG_SLOT_BYTES;
    if(jello_slot_size(vm_reg_kind(ctx->m, kinds_f, kinds_first + ai)) == 8u) {
      emit_mov_r64_from_r64_disp(buf, X64_RAX, X64_R8, src_disp);
      emit_store_r64_to_r13_disp(buf, X64_RAX, dst_disp);
    } else {
      emit_mov_r32_from_r64_disp(buf, X64_RAX, X64_R8, src_disp);
      emit_store_r32_to_r13_disp(buf, X64_RAX, dst_disp);
    }
  }
}

/** Loads frame_stack_top into RCX, and it plus `total`, a frame's bytes, into R8. */
static void emit_frame_tops(jit_exec_buf* buf, uint32_t total) {
  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_r32_r32(buf, X64_R8, X64_RCX);
  emit_alu_r32_imm32(buf, 0, X64_R8, total); // add r8d, total
}

/** JIR_CALL_SELF/JIR_CALL_DIRECT; pushes the callee's frame inline, as vm_push_frame would,
 * copies the arguments and calls through the entry table. Failure to get a layout or to grow the
 * stacks traps STACK_OVERFLOW. Returns 0, declining the function, without an entry table. */
static int emit_call_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins, jello_jit_ir_op op) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t callee_fi = (op == JIR_CALL_SELF) ? ctx->fi : ins->imm;
  if(!ctx->entries) return 0;
  uint32_t caller_dst = ins->a;
  uint32_t arg_first = ins->b;
  uint32_t nargs = ins->c;
  const jello_bc_function* callee_f = &ctx->m->funcs[callee_fi];

  jit_reg_state_discard_currents(ctx->ra);

  /* The callee's frame_layout, from vm->jit_layout_by_func, stays in X64_STAGE across the C
   * calls below; its fields are read with plain loads. */
  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
  emit_cmp_r32_imm32(buf, X64_RCX, callee_fi);
  size_t j_indexed = emit_jcc_rel32(buf, 0x87, 0); // JA: callee_fi < len
  emit_jmp_stack_overflow(ctx);
  patch_jcc_rel32(buf, j_indexed, (int32_t)(buf->len - (j_indexed + 6)));
  emit_mov_r64_from_r12_disp(buf, X64_STAGE, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
  emit_mov_r64_from_r64_disp(buf, X64_STAGE, X64_STAGE,
                             callee_fi * (uint32_t)sizeof(frame_layout*));
  emit_test_r64_r64(buf, X64_STAGE, X64_STAGE);
  size_t j_no_layout = emit_jcc_rel32(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  patch_jcc_rel32(buf, j_no_layout, (int32_t)(buf->len - (j_no_layout + 6)));

  /* The frame's size and whether it holds a reference depend only on the callee. */
  uint32_t total = callee_f->nregs * REG_SLOT_BYTES;
  int has_pointer = vm_func_has_pointer(ctx->m, callee_f);

  /* Room on the frame stack and a free call_frame slot, as push_frame_shell makes them; when
   * either must grow, a C call grows them and the tops are loaded again. */
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_cmp_r_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_limit), 0);
  size_t j_frames = emit_jcc_rel32(buf, 0x82, 0); // JB: below the frame limit
  emit_jmp_stack_overflow(ctx);
  patch_jcc_rel32(buf, j_frames, (int32_t)(buf->len - (j_frames + 6)));
  emit_frame_tops(buf, total);
  emit_cmp_r_r12_disp(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_limit), 0);
  size_t j_grow = emit_jcc_rel32(buf, 0x87, 0); // JA: past what is committed
  emit_cmp_r_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_cap), 0);
  size_t j_room = emit_jcc_rel32(buf, 0x82, 0); // JB: a free slot
  patch_jcc_rel32(buf, j_grow, (int32_t)(buf->len - (j_grow + 6)));
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_r32(buf, X64_RSI, X64_R8);
  emit_call_fn(buf, (uintptr_t)jit_try_grow_push);
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  size_t j_grown = emit_jcc_rel32(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  patch_jcc_rel32(buf, j_grown, (int32_t)(buf->len - (j_grown + 6)));
  emit_frame_tops(buf, total);
  patch_jcc_rel32(buf, j_room, (int32_t)(buf->len - (j_room + 6)));

  // The frame stack never moves; R11 = the new frame's registers.
  emit_store_r32_to_r12_disp(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_r64_from_r12_disp(buf, X64_R11, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_add_r64_r64(buf, X64_R11, X64_RCX);

  /* As in vm_push_frame, a frame with a reference register starts as the layout's init image;
   * memcpy returns the frame's registers, as R11 is caller-saved. */
  if(has_pointer) {
    emit_mov_r64_r64(buf, X64_RDI, X64_R11);
    emit_mov_r64_from_r64_disp(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(frame_layout, init));
    emit_mov_r32_imm32(buf, X64_RDX, total);
    emit_call_fn(buf, (uintptr_t)JIT_C(memcpy));
    emit_mov_r64_r64(buf, X64_R11, X64_RAX);
  }

  emit_fill_call_frame(buf, callee_f, callee_fi, has_pointer, total, caller_dst);
  emit_copy_args(ctx, callee_f, 0, arg_first, nargs);

  /* The callee's current target (its body, the bridge, or the lazy stub that compiles it on
   * first use) is in the module's entry table. */
  emit_mov_r64_imm64(buf, X64_RAX, (uint64_t)(uintptr_t)&ctx->entries[callee_fi]);
  emit_mov_r64_from_r64_disp(buf, X64_RAX, X64_RAX, 0);
  emit_bridge_when_deep(ctx, X64_RAX);
  emit_call_jit_code(buf, X64_RAX);
  emit_after_jit_call(ctx, ins->a);
  return 1;
}

/** Jumps, through rel32 placeholders appended to `slow`, unless `reg` holds a non-NULL pointer to
 * an object of header kind `kind`. Clobbers RAX. */
static void emit_obj_kind_guard(jit_exec_buf* buf, unsigned reg, uint32_t kind, size_t* slow,
                                size_t* nslow) {
  emit_mov_r32_r32(buf, X64_RAX, reg);
  emit_alu_r32_imm32(buf, 4, X64_RAX, 7u);         // and eax, 7
  slow[(*nslow)++] = emit_jcc_rel32(buf, 0x85, 0); // not a pointer
  emit_test_r64_r64(buf, reg, reg);
  slow[(*nslow)++] = emit_jcc_rel32(buf, 0x84, 0); // NULL
  emit_movzx_r32_m8_r64_disp(buf, X64_RAX, reg, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp_r32_imm32(buf, X64_RAX, kind);
  slow[(*nslow)++] = emit_jcc_rel32(buf, 0x85, 0);
}

/** CALLR. The common case is inline: a Function value whose frame has a layout and fits is
 * pushed as emit_call_insn pushes one, from the layout and the Function read at run time, and its
 * captures, always Dynamic and so 8 bytes each, follow its arguments. Anything else (null, not a
 * Function, no layout, a full call-frame array or frame stack) takes jit_callr_push, which pushes
 * or traps as op_callr does, located at this CALLR. Either way the callee's entry (its body, the
 * bridge or the lazy stub) ends in X64_STAGE; a trap inside the callee passes through. As for
 * CALL, the layout index is warmed for the running module before any JIT code runs. */
static void emit_callr_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t caller_dst = ins->a;
  uint32_t callee_disp = ins->b * REG_SLOT_BYTES;
  uint32_t arg_first = ins->imm;
  uint32_t nargs = ins->c;
  const jello_bc_function* caller_f = &ctx->m->funcs[ctx->fi];
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->entries) emit_native_stack_check(ctx);

  size_t slow[8];
  size_t nslow = 0;
  size_t j_call = 0;
  if(ctx->entries) {
    // R10 = the Function, R9 = its function index, X64_STAGE = its frame_layout.
    emit_mov_r64_from_r64_disp(buf, X64_R10, (unsigned)JIT_REG_BASE, callee_disp);
    emit_obj_kind_guard(buf, X64_R10, (uint32_t)JELLO_OBJ_FUNCTION, slow, &nslow);
    emit_mov_r32_from_r64_disp(buf, X64_R9, X64_R10,
                               (uint32_t)offsetof(jello_function, func_index));
    emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
    emit_cmp_r32_r32(buf, X64_RAX, X64_R9);
    slow[nslow++] = emit_jcc_rel32(buf, 0x86, 0); // JBE: no layout index
    emit_mov_r64_from_r12_disp(buf, X64_STAGE, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
    emit_mov_r64_r64(buf, X64_RAX, X64_R9);
    emit_shl_r64_imm8(buf, X64_RAX, 3);
    emit_add_r64_r64(buf, X64_STAGE, X64_RAX);
    emit_mov_r64_from_r64_disp(buf, X64_STAGE, X64_STAGE, 0);
    emit_test_r64_r64(buf, X64_STAGE, X64_STAGE);
    slow[nslow++] = emit_jcc_rel32(buf, 0x84, 0);
    emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
    emit_cmp_r_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_limit), 0);
    slow[nslow++] = emit_jcc_rel32(buf, 0x83, 0); // JAE
    emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_cap));
    emit_cmp_r32_r32(buf, X64_RCX, X64_RAX);
    slow[nslow++] = emit_jcc_rel32(buf, 0x83, 0);
    emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_mov_r32_from_r64_disp(buf, X64_R8, X64_STAGE, (uint32_t)offsetof(frame_layout, total));
    emit_add_r64_r64(buf, X64_R8, X64_RCX);
    emit_cmp_r_r12_disp(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_limit), 0);
    slow[nslow++] = emit_jcc_rel32(buf, 0x87, 0); // JA: does not fit

    // The push cannot fail from here; R11 = the new frame's registers.
    emit_store_r32_to_r12_disp(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_mov_r64_from_r12_disp(buf, X64_R11, (uint32_t)offsetof(jello_vm, frame_stack));
    emit_add_r64_r64(buf, X64_R11, X64_RCX);
    emit_mov_r32_from_r64_disp(buf, X64_RAX, X64_STAGE,
                               (uint32_t)offsetof(frame_layout, has_pointer));
    emit_test_r32_r32(buf, X64_RAX, X64_RAX);
    size_t j_no_ptr = emit_jcc_rel32(buf, 0x84, 0);
    emit_mov_r64_r64(buf, X64_RDI, X64_R11);
    emit_mov_r64_from_r64_disp(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(frame_layout, init));
    emit_mov_r32_from_r64_disp(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, total));
    emit_call_fn(buf, (uintptr_t)JIT_C(memcpy));
    emit_mov_r64_r64(buf, X64_R11, X64_RAX);
    emit_mov_r64_from_r64_disp(buf, X64_R10, (unsigned)JIT_REG_BASE, callee_disp);
    emit_mov_r32_from_r64_disp(buf, X64_R9, X64_R10,
                               (uint32_t)offsetof(jello_function, func_index));
    patch_jcc_rel32(buf, j_no_ptr, (int32_t)(buf->len - (j_no_ptr + 6)));

    emit_fill_call_frame(buf, NULL, 0, 0, 0, caller_dst);

    // The captures, from RDI into the registers from cap_start at RSI, EDX at a time.
    emit_mov_r32_from_r64_disp(buf, X64_RDX, X64_R10, (uint32_t)offsetof(jello_function, ncaps));
    emit_test_r32_r32(buf, X64_RDX, X64_RDX);
    size_t j_no_caps = emit_jcc_rel32(buf, 0x84, 0);
    emit_mov_r64_from_r64_disp(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(frame_layout, f));
    emit_mov_r32_from_r64_disp(buf, X64_RSI, X64_RSI,
                               (uint32_t)offsetof(jello_bc_function, cap_start));
    emit_shl_r64_imm8(buf, X64_RSI, 3);
    emit_add_r64_r64(buf, X64_RSI, X64_R11);
    emit_mov_r64_r64(buf, X64_RDI, X64_R10);
    emit_alu_r64_imm32(buf, 0, X64_RDI, (uint32_t)offsetof(jello_function, captures));
    size_t loop = buf->len;
    emit_mov_r64_from_r64_disp(buf, X64_RAX, X64_RDI, 0);
    emit_store_r64_to_r64_disp(buf, X64_RAX, X64_RSI, 0);
    emit_alu_r64_imm32(buf, 0, X64_RDI, 8u);
    emit_alu_r64_imm32(buf, 0, X64_RSI, 8u);
    emit_alu_r32_imm32(buf, 5, X64_RDX, 1u); // sub edx, 1
    emit_jcc_rel32(buf, 0x85, (int32_t)((int64_t)loop - (int64_t)(buf->len + 6)));
    patch_jcc_rel32(buf, j_no_caps, (int32_t)(buf->len - (j_no_caps + 6)));

    // The arguments, at the caller's widths, which validation makes the callee's.
    emit_copy_args(ctx, caller_f, arg_first, arg_first, nargs);
    emit_mov_r64_imm64(buf, X64_RAX, (uint64_t)(uintptr_t)ctx->entries);
    emit_shl_r64_imm8(buf, X64_R9, 3);
    emit_add_r64_r64(buf, X64_RAX, X64_R9);
    emit_mov_r64_from_r64_disp(buf, X64_STAGE, X64_RAX, 0);
    j_call = emit_jmp_rel32(buf, 0);
  }

  for(size_t i = 0; i < nslow; i++)
    patch_jcc_rel32(buf, slow[i], (int32_t)(buf->len - (slow[i] + 6)));
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_imm32(buf, X64_RSI, caller_dst);
  emit_mov_r32_imm32(buf, X64_RDX, ins->b);
  emit_mov_r32_imm32(buf, X64_RCX, arg_first);
  emit_mov_r32_imm32(buf, X64_R8, nargs);
  emit_call_fn(buf, (uintptr_t)jit_callr_push);
  emit_test_r64_r64(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // JE: refused
  // The target waits in X64_STAGE while JIT_REG_BASE moves to the callee's frame.
  emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);
  emit_reload_jit_reg_base_x64(buf);
  if(ctx->entries) {
    patch_jmp_rel32(buf, j_call, (int32_t)(buf->len - (j_call + 5)));
    emit_bridge_when_deep(ctx, X64_STAGE);
  }
  emit_call_jit_code(buf, X64_STAGE);
  emit_after_jit_call(ctx, ins->a);
}

/** TRY pushes a scope on the frame (vm_try_push, which traps when there is no memory for it);
 * the handler pc is absolute. */
static void emit_try_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_imm32(buf, X64_RSI, (uint32_t)((int32_t)ins->bc_pc + 1 + (int32_t)ins->imm));
  emit_mov_r32_imm32(buf, X64_RDX, ins->a);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_try_push));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // JE: no memory
}

static void emit_endtry_insn(emit_ctx* ctx) {
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(ctx->buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_try_pop));
}

/** THROW leaves either the unwind sentinel or an uncaught-throw trap; both leave through the
 * passthrough exit, whose dispatch catches the former when the handler is in this activation. */
static void emit_throw_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_imm32(buf, X64_RSI, ins->a);
  emit_mov_r32_imm32(buf, X64_RDX, ctx->fi);
  emit_mov_r32_imm32(buf, X64_RCX, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_throw);
  size_t at = emit_jmp_rel32(buf, 0);
  push_trap_jump(ctx, at, JIT_TRAP_PASSTHROUGH);
}

/** In a function with handlers, every passthrough exit (a callee's trap or unwind, THROW, a
 * handler-run op) first checks for a throw unwinding to this activation; the sentinel with
 * call_frames_len == unwind_len. A recursive activation without the scope sees a different
 * length and keeps unwinding. vm_throw_find already set the frame's pc to the handler; the code
 * dispatches on it. Registers are all in their homes. */
static void emit_catch_dispatch(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_cmp_r32_imm32(buf, X64_RAX, (uint32_t)JELLO_TRAP_INTERNAL_UNWIND);
  size_t not_unwind = emit_jcc_rel32(buf, 0x85, 0);
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, unwind_len));
  emit_cmp_r32_r32(buf, X64_RAX, X64_RCX);
  size_t not_here = emit_jcc_rel32(buf, 0x85, 0);
  emit_store_imm32_to_vm_field(buf, (uint32_t)offsetof(jello_vm, trap_code),
                               (uint32_t)JELLO_TRAP_NONE);
  emit_store_imm32_to_vm_field(buf, JIT_TRAP_SITE_OFF, 0u);
  emit_reload_jit_reg_base_x64(buf); // leaves the top call_frame's address in R8
  emit_mov_r32_from_r64_disp(buf, X64_RAX, X64_R8, (uint32_t)offsetof(call_frame, pc));
  for(uint32_t i = 0; i < ctx->ncatch; i++) {
    emit_cmp_r32_imm32(buf, X64_RAX, ctx->catch_pcs[i]);
    size_t at = emit_jcc_rel32(buf, 0x84, 0);
    push_branch(ctx, at, ctx->catch_pcs[i], 1);
  }
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_unwind_lost);
  patch_jcc_rel32(buf, not_unwind, (int32_t)(buf->len - (not_unwind + 6)));
  patch_jcc_rel32(buf, not_here, (int32_t)(buf->len - (not_here + 6)));
  emit_trap_tail_x64(ctx);
}

static void emit_imul_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  size_t n = 0;
  if((dst >= 8) || (src >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xAF;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_imul_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[1] = 0x0F;
  bytes[2] = 0xAF;
  bytes[3] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_imul_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  // imul dst32, dst32, imm32 (69 /r): dst is in both reg and rm, so r8-r15 need REX.R and REX.B
  uint8_t bytes[8];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x45;
  bytes[n++] = 0x69;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** mov edi, [r13 + vreg*8]: loads a register's home, which is always current. */
static void emit_load_u32_slot_to_edi(jit_exec_buf* buf, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  bytes[0] = 0x41; // REX.B: base R13
  bytes[1] = 0x8B;
  bytes[2] = (uint8_t)(0x80u | (X64_RDI << 3) | 5u);
  memcpy(&bytes[3], &disp, 4);
  jit_exec_buf_write(buf, bytes, 7);
}

/** add rsp, imm8 or sub rsp, imm8 (REX.W 83 /0 or /5). */
static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  uint8_t bytes[4];
  bytes[0] = 0x48;
  bytes[1] = 0x83;
  bytes[2] = (uint8_t)(0xC0u | (is_add ? 0x00u : 0x28u) | (unsigned)X64_RSP);
  bytes[3] = imm8;
  jit_exec_buf_write(buf, bytes, 4);
}

/** movss [rsp + disp8], xmm. */
static void emit_movss_xmm_mem_rsp(jit_exec_buf* buf, unsigned xmm, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(xmm >= 8) bytes[n++] = 0x44;
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x44u | ((xmm & 7u) << 3)); // mod=01, rm=RSP -> SIB
  bytes[n++] = 0x24;                                 // SIB: base=RSP, index=none
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movss_xmm_from_mem_rsp(jit_exec_buf* buf, unsigned xmm, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(xmm >= 8) bytes[n++] = 0x44;
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x44u | ((xmm & 7u) << 3));
  bytes[n++] = 0x24;
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

/** mov [rsp + disp8], src64. With emit_mov_r64_from_mem_rsp, keeps a value in a 16-byte stack
 * slot across a C call, between `sub rsp, 16` and `add rsp, 16`. */
static void emit_store_r64_to_mem_rsp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x44u | ((src & 7u) << 3));
  bytes[n++] = 0x24;
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r64_from_mem_rsp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x44u | ((dst & 7u) << 3));
  bytes[n++] = 0x24;
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

/** An F16 binary op, computed in f32 with SSE op `sse_op` between C conversions. */
static void emit_f16_binop(emit_ctx* ctx, const jello_jit_ir_insn* ins, uint8_t sse_op) {
  // Every XMM register is caller-saved, so the first operand waits on the stack.
  emit_rsp_adjust_local(ctx->buf, 0, 16); // sub rsp, 16
  emit_load_u32_slot_to_edi(ctx->buf, ins->b);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
  emit_movss_xmm_mem_rsp(ctx->buf, 0, 0); // movss [rsp], xmm0
  emit_load_u32_slot_to_edi(ctx->buf, ins->c);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
  emit_movss_xmm_from_mem_rsp(ctx->buf, 1, 0); // movss xmm1, [rsp]
  emit_binss_xmm_xmm(ctx->buf, sse_op, 1, 0);
  emit_movss_xmm_xmm(ctx->buf, 0, 1);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f32_to_f16));
  emit_rsp_adjust_local(ctx->buf, 1, 16); // add rsp, 16
  jit_reg_state_clobber_call(ctx->ra);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(ctx->buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bin_f32(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 0);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_movss_xmm_xmm(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_binss_xmm_xmm(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_binss_xmm_xmm(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_binss_xmm_xmm(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_binss_xmm_xmm(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm32(ctx->ra, ins->a, dx);
}

static void emit_bin_f64(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 1);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_movsd_xmm_xmm(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_binsd_xmm_xmm(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_binsd_xmm_xmm(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_binsd_xmm_xmm(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_binsd_xmm_xmm(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
}

static void emit_cmp_float(emit_ctx* ctx, const jello_jit_ir_insn* ins, int is_f64) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, is_f64);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, is_f64);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  int is_eq = (jello_jir_cmp)ins->imm == JIR_CMP_EQ;
  /* ucomiss/ucomisd set only ZF, PF and CF, and an unordered (NaN) compare sets all three. EQ is
   * ZF and not PF (sete, setnp through AL); LT compares the operands swapped and is seta (CF and
   * ZF clear). Both are then false for NaN, as C's == and < are. */
  unsigned a = is_eq ? li : ri, b = is_eq ? ri : li;
  if(is_f64) emit_ucomisd(ctx->buf, a, b);
  else emit_ucomiss(ctx->buf, a, b);
  int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_setcc(ctx->buf, is_eq ? 0x94 : 0x97, (unsigned)d);
  if(is_eq) {
    emit_setcc(ctx->buf, 0x9B, X64_RAX);
    emit_alu_r32_r32(ctx->buf, 0x21, (unsigned)d, X64_RAX); // and d, eax
  }
  emit_movzx_r32_r8(ctx->buf, (unsigned)d, (unsigned)d);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
}

static void emit_movsx_r32_r16(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  if(dst >= 8 || src >= 8)
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xBF;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

/** Loads the double `x` into xmm`dd` through RAX. */
static void emit_load_f64(jit_exec_buf* buf, unsigned dd, double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof bits);
  emit_mov_r64_imm64(buf, X64_RAX, bits);
  emit_movq_xmm_r64(buf, dd, X64_RAX);
}

/** Traps ARITHMETIC unless xmm`v` (an F32 when `!is_f64`) converts to I64 when `to_i64`, else
 * I32; it must lie strictly between that type's VM_*_FROM_FLOAT bounds, so NaN never does.
 * UCOMISD sets CF and ZF when unordered, so JBE traps NaN too. Writes RAX, xmm6 and xmm7, which
 * are outside the pool. */
static void emit_float_conv_check(emit_ctx* ctx, unsigned v, int is_f64, int to_i64) {
  jit_exec_buf* buf = ctx->buf;
  if(!is_f64) {
    emit_cvtss2sd(buf, 7, v);
    v = 7;
  }
  emit_load_f64(buf, 6, to_i64 ? VM_I64_FROM_FLOAT_LO : VM_I32_FROM_FLOAT_LO);
  emit_ucomisd(buf, v, 6);
  emit_jcc_trap(ctx, 0x86, JIT_TRAP_ARITH); // JBE: v <= lo, or unordered
  emit_load_f64(buf, 6, to_i64 ? VM_I64_FROM_FLOAT_HI : VM_I32_FROM_FLOAT_HI);
  emit_ucomisd(buf, 6, v);
  emit_jcc_trap(ctx, 0x86, JIT_TRAP_ARITH); // JBE: hi <= v, or unordered
}

static int emit_conv(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jir_conv)ins->imm) {
    case JIR_CONV_TRUNC_I8: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_TRUNC_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_movsx_r32_r16(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_I32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_movsxd_r64_r32(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_cvtsi2ss(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_cvtsi2sd(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 1, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttsd2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_cvtss2sd(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0),
                    (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_cvtsd2ss(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0),
                    (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F64_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_cvtsi2sd(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 1, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttsd2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_cvtsi2ss(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 0, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_r64_r64(ctx->buf, X64_RDI, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_movss_xmm_xmm(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_movss_xmm_xmm(ctx->buf, 0, (unsigned)(sx - (int)JIT_PREG_XMM0));
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f32_to_f16));
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_cvtsi2ss(ctx->buf, 0, (unsigned)s, 0);
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f32_to_f16));
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_I32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_r64_r64(ctx->buf, X64_RDI, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
      emit_float_conv_check(ctx, 0, 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, 0, 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    default: return 0;
  }
}

static void emit_alu_r64_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  uint8_t bytes[7];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x81;
  bytes[n++] = (uint8_t)(0xC0u | ((op_ext & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

/** Traps NULL when reference register `gpr` holds null. */
static void emit_null_check(emit_ctx* ctx, unsigned gpr) {
  emit_test_r64_r64(ctx->buf, gpr, gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
}

/** Traps NULL when the Bytes in `gpr` is null, and TYPE when it is no buffer, as only a buffer
 * may be written. Clobbers RAX. */
static void emit_buffer_check(emit_ctx* ctx, unsigned gpr) {
  emit_null_check(ctx, gpr);
  emit_mov_r32_from_r64_disp(ctx->buf, X64_RAX, gpr, (uint32_t)offsetof(jello_bytes, h.type_id));
  emit_cmp_r32_imm32(ctx->buf, X64_RAX, JELLO_BYTES_BUFFER);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_TYPE); // JNE: no buffer
}

/** shl (`ext` 4) or shr (`ext` 5) r64, imm8 (REX.W C1 /ext). */
static void emit_shift_r64_imm8(jit_exec_buf* buf, uint8_t ext, unsigned dst, uint8_t imm8) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xC1;
  bytes[2] = (uint8_t)(0xC0u | ((ext & 7u) << 3) | (dst & 7u));
  bytes[3] = imm8;
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_shl_r64_imm8(jit_exec_buf* buf, unsigned dst, uint8_t imm8) {
  emit_shift_r64_imm8(buf, 4, dst, imm8);
}

/** Traps BOUNDS unless `idx`, read as unsigned, is below the 32-bit length at `len_off` from
 * `ptr`. Clobbers RAX. */
static void emit_bounds_trap(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off) {
  emit_mov_r32_from_r64_disp(ctx->buf, X64_RAX, ptr, len_off);
  emit_cmp_r32_r32(ctx->buf, idx, X64_RAX);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); // JAE: idx >= length
}

/** Leaves in RCX the address of element `idx` of the Bytes or Array in register `ptr`, whose
 * length and data pointer are at `len_off` and `data_off`, its elements 1 << `shift` bytes;
 * traps BOUNDS when `idx`, read as unsigned, is not below the length. Clobbers RAX and RDX. */
static void emit_indexed_addr(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off,
                              uint32_t data_off, uint8_t shift) {
  jit_exec_buf* buf = ctx->buf;
  emit_bounds_trap(ctx, ptr, idx, len_off);
  emit_mov_r64_from_r64_disp(buf, X64_RCX, ptr, data_off);
  emit_mov_r32_r32(buf, X64_RDX, idx);
  if(shift) emit_shl_r64_imm8(buf, X64_RDX, shift);
  emit_add_r64_r64(buf, X64_RCX, X64_RDX);
}

static void emit_bytes_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_from_r64_disp(buf, (unsigned)dst, (unsigned)bp,
                             (uint32_t)offsetof(jello_bytes, length));
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bytes_get_u8(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int idx = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_indexed_addr(ctx, (unsigned)bp, (unsigned)idx, (uint32_t)offsetof(jello_bytes, length),
                    (uint32_t)offsetof(jello_bytes, data), 0);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_movzx_r32_m8_r64_disp(buf, (unsigned)dst, X64_RCX, 0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bytes_set_u8(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int idx = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_indexed_addr(ctx, (unsigned)bp, (unsigned)idx, (uint32_t)offsetof(jello_bytes, length),
                    (uint32_t)offsetof(jello_bytes, data), 0);
  int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_cmp_r32_imm32(buf, (unsigned)val, 255u);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // JA: val > 255
  emit_store_r8_to_r64_disp(buf, (unsigned)val, X64_RCX, 0);
}

static void emit_bytes_eq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);

  emit_cmp_r64_r64(buf, (unsigned)b, (unsigned)c);
  size_t j_same = emit_jcc_rel32(buf, 0x84, 0); // JE -> true

  emit_test_r64_r64(buf, (unsigned)b, (unsigned)b);
  size_t j_b_null = emit_jcc_rel32(buf, 0x84, 0); // -> false
  emit_test_r64_r64(buf, (unsigned)c, (unsigned)c);
  size_t j_c_null = emit_jcc_rel32(buf, 0x84, 0); // -> false

  emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_r32_from_r64_disp(buf, X64_RCX, (unsigned)c, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_r32_r32(buf, X64_RAX, X64_RCX);
  size_t j_len_ne = emit_jcc_rel32(buf, 0x85, 0); // JNE -> false

  emit_mov_r64_from_r64_disp(buf, X64_RAX, (unsigned)b, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_r64_from_r64_disp(buf, X64_RCX, (unsigned)c, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_r32_from_r64_disp(buf, X64_RDX, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_mov_r64_r64(buf, X64_RSI, X64_RCX);
  emit_call_fn(buf, (uintptr_t)JIT_C(memcmp));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_setcc_from_flags(buf, 0x94, (unsigned)dst); // dst = (memcmp == 0)
  size_t j_done1 = emit_jmp_rel32(buf, 0);

  size_t false_lbl = buf->len;
  patch_jcc_rel32(buf, j_b_null, (int32_t)(false_lbl - (j_b_null + 6)));
  patch_jcc_rel32(buf, j_c_null, (int32_t)(false_lbl - (j_c_null + 6)));
  patch_jcc_rel32(buf, j_len_ne, (int32_t)(false_lbl - (j_len_ne + 6)));
  emit_mov_r32_imm32(buf, (unsigned)dst, 0u);
  size_t j_done2 = emit_jmp_rel32(buf, 0);

  size_t true_lbl = buf->len;
  patch_jcc_rel32(buf, j_same, (int32_t)(true_lbl - (j_same + 6)));
  emit_mov_r32_imm32(buf, (unsigned)dst, 1u);

  size_t done_lbl = buf->len;
  patch_jmp_rel32(buf, j_done1, (int32_t)(done_lbl - (j_done1 + 5)));
  patch_jmp_rel32(buf, j_done2, (int32_t)(done_lbl - (j_done2 + 5)));

  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static int bytes_bitop_kind(uint32_t imm) {
  jello_jir_bin op = (jello_jir_bin)imm;
  if(op == JIR_BIN_AND) return 0;
  if(op == JIR_BIN_OR) return 1;
  return 2; // JIR_BIN_XOR
}

static void emit_bytes_bitop(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  int op_kind = bytes_bitop_kind(ins->imm);

  emit_mov_r64_r64(buf, X64_RAX, (unsigned)b);
  emit_mov_r64_r64(buf, X64_RCX, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_r64(buf, X64_RSI, X64_RAX);
  emit_mov_r64_r64(buf, X64_RDX, X64_RCX);
  emit_mov_r32_imm32(buf, X64_RCX, (uint32_t)op_kind);
  emit_call_fn(buf, (uintptr_t)jit_bytes_bitop);
  emit_test_r64_r64(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // NULL: trap_code already set
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void bytes_rw_info(jello_jir_bytes_rw rw, uint32_t* width, int* be, int* is_float) {
  switch(rw) {
    case JIR_BRW_U16_LE:
      *width = 2;
      *be = 0;
      *is_float = 0;
      return;
    case JIR_BRW_U16_BE:
      *width = 2;
      *be = 1;
      *is_float = 0;
      return;
    case JIR_BRW_U32_LE:
      *width = 4;
      *be = 0;
      *is_float = 0;
      return;
    case JIR_BRW_U32_BE:
      *width = 4;
      *be = 1;
      *is_float = 0;
      return;
    case JIR_BRW_I32_LE:
      *width = 4;
      *be = 0;
      *is_float = 0;
      return;
    case JIR_BRW_I32_BE:
      *width = 4;
      *be = 1;
      *is_float = 0;
      return;
    case JIR_BRW_F32_LE:
      *width = 4;
      *be = 0;
      *is_float = 1;
      return;
    case JIR_BRW_F32_BE:
      *width = 4;
      *be = 1;
      *is_float = 1;
      return;
    case JIR_BRW_I64_LE:
      *width = 8;
      *be = 0;
      *is_float = 0;
      return;
    case JIR_BRW_I64_BE:
      *width = 8;
      *be = 1;
      *is_float = 0;
      return;
    case JIR_BRW_F64_LE:
      *width = 8;
      *be = 0;
      *is_float = 1;
      return;
    case JIR_BRW_F64_BE:
      *width = 8;
      *be = 1;
      *is_float = 1;
      return;
  }
  *width = 4;
  *be = 0;
  *is_float = 0;
}

/** Traps BOUNDS unless offset + `width` <= length. The sum is 64-bit, as a 32-bit one could wrap
 * a large unsigned offset back into range. */
static void emit_bytes_bounds_check(emit_ctx* ctx, int bytes_gpr, int off_gpr, uint32_t width) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)bytes_gpr,
                             (uint32_t)offsetof(jello_bytes, length));
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)off_gpr);
  emit_alu_r64_imm32(buf, 0x00, X64_RCX, width);
  emit_cmp_r64_r64(buf, X64_RCX, X64_RAX);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // JA: off + width > length
}

static void emit_bytes_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width;
  int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  emit_mov_r64_from_r64_disp(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)off);
  emit_add_r64_r64(buf, X64_RAX, X64_RCX);

  if(!is_float) {
    int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
    if(width == 2) emit_movzx_r32_m16_r64_disp(buf, (unsigned)dst, X64_RAX, 0);
    else if(width == 4) emit_mov_r32_from_r64_disp(buf, (unsigned)dst, X64_RAX, 0);
    else emit_mov_r64_from_r64_disp(buf, (unsigned)dst, X64_RAX, 0);
    if(be) {
      if(width == 8) emit_bswap_r64(buf, (unsigned)dst);
      else if(width == 4) emit_bswap_r32(buf, (unsigned)dst);
      else emit_rol16_imm8(buf, (unsigned)dst, 8);
    }
    if(width == 8) jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
    else jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
  } else {
    if(width == 4) emit_mov_r32_from_r64_disp(buf, X64_RCX, X64_RAX, 0);
    else emit_mov_r64_from_r64_disp(buf, X64_RCX, X64_RAX, 0);
    if(be) {
      if(width == 8) emit_bswap_r64(buf, X64_RCX);
      else emit_bswap_r32(buf, X64_RCX);
    }
    int dst = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, width == 8);
    if(width == 4) {
      emit_movd_xmm_r32(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), X64_RCX);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dst);
    } else {
      emit_movq_xmm_r64(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), X64_RCX);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dst);
    }
  }
}

static void emit_bytes_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width;
  int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  if(!is_float) {
    int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
    emit_mov_r64_r64(buf, X64_RCX, (unsigned)val);
  } else {
    int val = jit_reg_get_xmm(ctx->ra, ins->a, 1, width == 8);
    if(width == 4) emit_movd_r32_xmm(buf, X64_RCX, (unsigned)(val - (int)JIT_PREG_XMM0));
    else emit_movq_r64_xmm(buf, X64_RCX, (unsigned)(val - (int)JIT_PREG_XMM0));
  }
  if(be) {
    if(width == 8) emit_bswap_r64(buf, X64_RCX);
    else if(width == 4) emit_bswap_r32(buf, X64_RCX);
    else emit_rol16_imm8(buf, X64_RCX, 8);
  }

  emit_mov_r64_from_r64_disp(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_r32_r32(buf, X64_RDX, (unsigned)off);
  emit_add_r64_r64(buf, X64_RAX, X64_RDX);

  if(width == 2) emit_store_r16_to_r64_disp(buf, X64_RCX, X64_RAX, 0);
  else if(width == 4) emit_store_r32_to_r64_disp(buf, X64_RCX, X64_RAX, 0);
  else emit_store_r64_to_r64_disp(buf, X64_RCX, X64_RAX, 0);
}

/** Copies all of `b` into `a` at offset `c`, with memmove since `a` and `b` may be the same
 * Bytes. */
static void emit_bytes_blit(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int dst_bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)dst_bp);
  int src_bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)src_bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);

  emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)src_bp,
                             (uint32_t)offsetof(jello_bytes, length));
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)off);
  emit_add_r64_r64(buf, X64_RCX, X64_RAX);
  emit_mov_r32_from_r64_disp(buf, X64_RDX, (unsigned)dst_bp,
                             (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_r64_r64(buf, X64_RCX, X64_RDX);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // JA: off + src->length > dst->length

  // RAX = dst data + off, RCX = src data, RDX = src length
  emit_mov_r64_from_r64_disp(buf, X64_RAX, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)off);
  emit_add_r64_r64(buf, X64_RAX, X64_RCX);
  emit_mov_r64_from_r64_disp(buf, X64_RCX, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_r32_from_r64_disp(buf, X64_RDX, (unsigned)src_bp,
                             (uint32_t)offsetof(jello_bytes, length));

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_mov_r64_r64(buf, X64_RSI, X64_RCX);
  emit_call_fn(buf, (uintptr_t)JIT_C(memmove));
}

static void emit_bits_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int val = jit_reg_get_gpr(ctx->ra, ins->b, 1);

  emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_r32_imm32(buf, X64_RCX, 3u);
  emit_shl_cl_r64(buf, X64_RAX);
  emit_mov_r64_imm64(buf, X64_RDX, end_bit);
  emit_cmp_r64_r64(buf, X64_RDX, X64_RAX);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // JA: end_bit > length*8

  emit_mov_r64_r64(buf, X64_RAX, (unsigned)bp);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)val);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_mov_r32_imm32(buf, X64_RSI, width);
  emit_mov_r32_imm32(buf, X64_RDX, bit_off);
  emit_call_fn(buf, (uintptr_t)jit_bits_write);
}

/** Clears the buffer mark of the Bytes in `a`, as op_bytes_freeze does; any other Bytes may be
 * shared, so it is not written. */
static void emit_bytes_freeze(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  const uint32_t mark = (uint32_t)offsetof(jello_bytes, h.type_id);
  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_null_check(ctx, (unsigned)bp);
  emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)bp, mark);
  emit_cmp_r32_imm32(buf, X64_RAX, JELLO_BYTES_BUFFER);
  size_t shared = emit_jcc_rel8(buf, 0x85); // JNE: no buffer
  emit_mov_r32_imm32(buf, X64_RAX, 0);
  emit_store_r32_to_r64_disp(buf, X64_RAX, (unsigned)bp, mark);
  patch_jcc_rel8_here(buf, shared);
}

static void emit_bytes_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int len = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_r32_r32(buf, X64_RAX, (unsigned)len);
  // A negative length, read unsigned, is past INT32_MAX, as in op_bytes_new.
  emit_cmp_r32_imm32(buf, X64_RAX, (uint32_t)INT32_MAX);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // JA: len > INT32_MAX
  // As in op_bytes_new, a max_bytes_len of 0 means no limit.
  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, max_bytes_len));
  emit_test_r32_r32(buf, X64_RCX, X64_RCX);
  size_t limit_skip = emit_jcc_rel32(buf, 0x84, 0); // JE: max_bytes_len == 0
  emit_cmp_r32_r32(buf, X64_RAX, X64_RCX);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_LIMIT); // JA: len > max_bytes_len
  patch_jcc_rel32(buf, limit_skip, (int32_t)(buf->len - (limit_skip + 6)));
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_r32(buf, X64_RSI, X64_RAX);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_buffer_new));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bytes_concat2(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)b);
  emit_mov_r64_r64(buf, X64_RCX, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_r64(buf, X64_RSI, X64_RAX);
  emit_mov_r64_r64(buf, X64_RDX, X64_RCX);
  emit_call_fn(buf, (uintptr_t)jit_bytes_concat2);
  emit_test_r64_r64(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // NULL: trap_code already set (BOUNDS)
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bits_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);

  emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_r32_imm32(buf, X64_RCX, 3u);
  emit_shl_cl_r64(buf, X64_RAX);
  emit_mov_r64_imm64(buf, X64_RDX, end_bit);
  emit_cmp_r64_r64(buf, X64_RDX, X64_RAX);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS);

  emit_mov_r64_r64(buf, X64_RAX, (unsigned)bp);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_mov_r32_imm32(buf, X64_RSI, width);
  emit_mov_r32_imm32(buf, X64_RDX, bit_off);
  emit_call_fn(buf, (uintptr_t)jit_bits_read);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_movss_xmm_from_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base,
                                         uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((xmm >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movss_to_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((xmm >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movsd_xmm_from_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base,
                                         uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((xmm >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movsd_to_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((xmm >= 8) || (base >= 8))
    bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static int is_xmm_field_kind(jello_type_kind k) { return k == JELLO_T_F32 || k == JELLO_T_F64; }

static void emit_struct_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  uint32_t struct_sig_id = ctx->m->types[tid].as.strct.struct_sig_id;

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r32_imm32(buf, X64_RDX, struct_sig_id);
  emit_call_fn(buf, (uintptr_t)JIT_C(jello_struct_new));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

/** Loads register `r`, a slot of kind `k`, from `disp` past `base`. */
static void emit_load_slot(emit_ctx* ctx, uint8_t r, jello_type_kind k, unsigned base,
                           uint32_t disp) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = (uint32_t)jello_slot_size(k);
  if(is_xmm_field_kind(k)) {
    int dst = jit_reg_alloc_xmm(ctx->ra, r, 0, width == 8);
    if(width == 8) {
      emit_movsd_xmm_from_r64_disp(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm64(ctx->ra, r, dst);
    } else {
      emit_movss_xmm_from_r64_disp(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm32(ctx->ra, r, dst);
    }
  } else {
    int dst = jit_reg_alloc_gpr(ctx->ra, r, 0);
    if(width == 8) {
      emit_mov_r64_from_r64_disp(buf, (unsigned)dst, base, disp);
      jit_reg_store_home_gpr64(ctx->ra, r, dst);
    } else {
      emit_mov_r32_from_r64_disp(buf, (unsigned)dst, base, disp);
      jit_reg_store_home_gpr32(ctx->ra, r, dst);
    }
  }
}

/** Stores register `r`, a slot of kind `k`, at `disp` past `base`. */
static void emit_store_slot(emit_ctx* ctx, uint8_t r, jello_type_kind k, unsigned base,
                            uint32_t disp) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = (uint32_t)jello_slot_size(k);
  if(is_xmm_field_kind(k)) {
    int val = jit_reg_get_xmm(ctx->ra, r, 1, width == 8);
    if(width == 8) emit_movsd_to_r64_disp(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
    else emit_movss_to_r64_disp(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
  } else {
    int val = jit_reg_get_gpr(ctx->ra, r, 1);
    if(width == 8) emit_store_r64_to_r64_disp(buf, (unsigned)val, base, disp);
    else emit_store_r32_to_r64_disp(buf, (unsigned)val, base, disp);
  }
}

/** The struct register `r`, null-checked, and the kind and offset of its field `field`; a
 * struct register's declared type fixes its struct_sig, as Jello has no struct subtyping. */
static int emit_struct_field(emit_ctx* ctx, uint8_t r, uint32_t field, jello_type_kind* kind,
                             uint32_t* disp) {
  const jello_struct_sig* sig =
      &ctx->m->struct_sigs[ctx->m->types[ctx->f->reg_types[r]].as.strct.struct_sig_id];
  *kind = ctx->m->types[sig->field_type[field]].kind;
  *disp = (uint32_t)offsetof(jello_struct, fields) + sig->field_offset[field];
  int sp = jit_reg_get_gpr(ctx->ra, r, 1);
  emit_null_check(ctx, (unsigned)sp);
  return sp;
}

static void emit_struct_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jello_type_kind kind;
  uint32_t disp;
  int sp = emit_struct_field(ctx, ins->b, ins->imm, &kind, &disp);
  emit_load_slot(ctx, ins->a, kind, (unsigned)sp, disp);
}

static void emit_struct_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jello_type_kind kind;
  uint32_t disp;
  int sp = emit_struct_field(ctx, ins->b, ins->imm, &kind, &disp);
  emit_store_slot(ctx, ins->a, kind, (unsigned)sp, disp);
}

/** Leaves in RCX the address of the element of array register `arr` at index register `idx`,
 * trapping NULL or BOUNDS; returns the elements' kind. An array register's declared type is its
 * array's (FROM_DYN checks the type id), so that kind is fixed. Clobbers RAX and RDX. */
static jello_type_kind emit_array_elem_addr(emit_ctx* ctx, uint8_t arr, uint8_t idx) {
  jello_type_kind kind = ctx->m->types[ctx->m->types[ctx->f->reg_types[arr]].as.unary.elem].kind;
  int ap = jit_reg_get_gpr(ctx->ra, arr, 1);
  emit_null_check(ctx, (unsigned)ap);
  int ip = jit_reg_get_gpr(ctx->ra, idx, 1);
  emit_indexed_addr(ctx, (unsigned)ap, (unsigned)ip, (uint32_t)offsetof(jello_array, length),
                    (uint32_t)offsetof(jello_array, data), jello_slot_size(kind) == 8 ? 3 : 2);
  return kind;
}

static void emit_array_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int ap = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)ap);
  emit_load_slot(ctx, ins->a, JELLO_T_I32, (unsigned)ap, (uint32_t)offsetof(jello_array, length));
}

static void emit_array_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_load_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), X64_RCX, 0);
}

static void emit_array_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), X64_RCX, 0);
}

/** Leaves in RCX the running VM's globals, a table of 8-byte slots, and returns the kind of
 * global `g`. */
static jello_type_kind emit_globals_base(emit_ctx* ctx, uint32_t g) {
  emit_mov_r64_from_r12_disp(ctx->buf, X64_RCX, (uint32_t)offsetof(jello_vm, globals));
  return ctx->m->types[ctx->m->global_types[g]].kind;
}

static void emit_global_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_load_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), X64_RCX, ins->imm * 8u);
}

static void emit_global_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), X64_RCX, ins->imm * 8u);
}

/** R11 = &top call frame's rf, the reg_frame* that vm_box_from_typed and vm_store_from_boxed
 * take. R11 is caller-saved, so it holds only until the next C call. Clobbers RAX, RCX and RDX. */
static void emit_current_rf_ptr_into_r11(jit_exec_buf* buf) {
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R11, X64_RAX);
  emit_imul_r64_r64(buf, X64_R11, X64_RDX);
  emit_add_r64_r64(buf, X64_R11, X64_RCX);
  emit_alu_r64_imm32(buf, 0x00, X64_R11, (uint32_t)offsetof(call_frame, rf));
}

/** RDI, RSI, RDX, RCX and R8 = the arguments vm_box_from_typed and vm_store_from_boxed share for
 * register `r`: the VM, module, function, top frame's reg_frame and `r`. */
static void emit_typed_helper_args(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_current_rf_ptr_into_r11(buf);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_r64_r64(buf, X64_RCX, X64_R11);
  emit_mov_r32_imm32(buf, X64_R8, r);
}

/** vm_store_from_boxed of X64_STAGE into register `dst`'s home, trapping where it fails. */
static void emit_store_from_boxed_stage(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  emit_typed_helper_args(ctx, dst);
  emit_mov_r64_r64(buf, X64_R9, X64_STAGE);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_store_from_boxed));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // 0: trap_code already set
}

/** Compares the tag of the Dynamic in `src` with `tag`, through EAX. */
static void emit_tag_cmp(jit_exec_buf* buf, unsigned src, uint32_t tag) {
  emit_mov_r32_r32(buf, X64_RAX, src);
  emit_alu_r32_imm8(buf, 0x04, X64_RAX, 7u); // and eax, 7
  emit_cmp_r32_imm32(buf, X64_RAX, tag);
}

/** Unboxes the value in X64_STAGE into register `dst`, as vm_store_from_boxed does, and binds it.
 * A Dynamic is a move, and an F64 box or an immediate (I8 to I32, Bool, Atom) is read inline.
 * Anything else, an I32 bound for an F64 included, takes vm_store_from_boxed, which converts or
 * traps. Runs after jit_reg_state_discard_currents. */
static void emit_unbox_stage_into(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, dst);
  if(k == JELLO_T_DYNAMIC) {
    int r = jit_reg_alloc_gpr(ctx->ra, dst, 0);
    emit_mov_r64_r64(buf, (unsigned)r, X64_STAGE);
    jit_reg_store_home_gpr64(ctx->ra, dst, r);
    return;
  }
  int tag = vm_kind_immediate_tag(k);
  int f64 = k == JELLO_T_F64;
  if(!f64 && tag < 0) {
    emit_store_from_boxed_stage(ctx, dst);
    if(k == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, dst, 1, 0);
    else (void)jit_reg_get_gpr(ctx->ra, dst, 1);
    return;
  }
  // Both paths leave the value in p, bound to `dst`; the slow path reloads it from home.
  size_t slow[3];
  size_t nslow = 0;
  int p = f64 ? jit_reg_alloc_xmm(ctx->ra, dst, 0, 1) : jit_reg_alloc_gpr(ctx->ra, dst, 0);
  unsigned pi = f64 ? (unsigned)(p - (int)JIT_PREG_XMM0) : (unsigned)p;
  if(f64) {
    emit_obj_kind_guard(buf, X64_STAGE, (uint32_t)JELLO_OBJ_BOX_F64, slow, &nslow);
    emit_movsd_xmm_from_r64_disp(buf, pi, X64_STAGE, (uint32_t)offsetof(jello_box_f64, value));
    jit_reg_store_home_xmm64(ctx->ra, dst, p);
  } else {
    uint32_t bits = vm_kind_immediate_bits(k);
    emit_tag_cmp(buf, X64_STAGE, (uint32_t)tag);
    slow[nslow++] = emit_jcc_rel32(buf, 0x85, 0); // JNE: another tag
    emit_mov_r64_r64(buf, pi, X64_STAGE);
    emit_shift_r64_imm8(buf, 5, pi, 3); // shr
    if(bits == 32u) emit_mov_r32_r32(buf, pi, pi);
    else emit_alu_r32_imm32(buf, 0x04, pi, (1u << bits) - 1u); // and
    jit_reg_store_home_gpr32(ctx->ra, dst, p);
  }
  size_t j_done = emit_jmp_rel32(buf, 0);
  for(size_t i = 0; i < nslow; i++) {
    patch_jcc_rel32(buf, slow[i], (int32_t)(buf->len - (slow[i] + 6)));
  }
  emit_store_from_boxed_stage(ctx, dst);
  if(f64) jit_spill_load_xmm64(buf, pi, dst);
  else jit_spill_load_gpr64(buf, pi, dst);
  patch_jmp_rel32_here(buf, j_done);
}

/** Leaves register `r` boxed in RAX, as vm_box_from_typed boxes it: a Dynamic is loaded, an
 * immediate (I8 to I32, Bool, Atom) is tagged inline and an F64 goes straight to
 * jello_box_f64_new. It may call out, so it runs after jit_reg_state_discard_currents and keeps
 * nothing in pool registers. */
static void emit_box_into_rax(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, r);
  int tag = vm_kind_immediate_tag(k);
  if(k == JELLO_T_DYNAMIC) {
    jit_spill_load_gpr64(buf, X64_RAX, r);
  } else if(tag >= 0) {
    emit_mov_r32_from_r64_disp(buf, X64_RAX, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
    if(k == JELLO_T_BOOL) {
      emit_test_r32_r32(buf, X64_RAX, X64_RAX);
      emit_setcc_from_flags(buf, 0x95, X64_RAX); // setne
    }
    emit_shl_r64_imm8(buf, X64_RAX, 3);
    emit_alu_r64_imm32(buf, 1, X64_RAX, (uint32_t)tag); // or rax, tag
  } else if(k == JELLO_T_F64) {
    emit_movsd_xmm_from_r64_disp(buf, 0, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
    emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, (uintptr_t)JIT_C(jello_box_f64_new));
  } else {
    emit_typed_helper_args(ctx, r);
    emit_call_fn(buf, (uintptr_t)JIT_C(vm_box_from_typed));
  }
}

/** Runs the current instruction through its interpreter handler (jit_run_handler). The handler
 * reads and writes register homes, so every binding is dropped first and reloaded afterwards. */
static void emit_handler_call(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r32_imm32(buf, X64_RDX, ctx->fi);
  emit_mov_r32_imm32(buf, X64_RCX, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_run_handler);
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH); // JNE: trapped, attributed by jit_run_handler
}

/** Traps NULL, or TYPE unless the object in `ep` is an enum of register `r`'s declared type,
 * as op_enum_tag and op_enum_get check. Clobbers RAX. */
static void emit_enum_guard(emit_ctx* ctx, unsigned ep, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_null_check(ctx, ep);
  emit_movzx_r32_m8_r64_disp(buf, X64_RAX, ep, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp_r32_imm32(buf, X64_RAX, (uint32_t)JELLO_OBJ_ENUM);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_TYPE);
  emit_mov_r32_from_r64_disp(buf, X64_RAX, ep, (uint32_t)offsetof(jello_obj_header, type_id));
  emit_cmp_r32_imm32(buf, X64_RAX, ctx->f->reg_types[r]);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_TYPE);
}

static void emit_enum_tag(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int ep = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_enum_guard(ctx, (unsigned)ep, ins->b);
  emit_load_slot(ctx, ins->a, JELLO_T_I32, (unsigned)ep, (uint32_t)offsetof(jello_enum, tag));
}

/** ENUM_GET; the boxed field, bounds-checked against the enum's field count, is unboxed into
 * the destination as op_enum_get's vm_store_from_boxed does. */
static void emit_enum_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int ep = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_enum_guard(ctx, (unsigned)ep, ins->b);
  int ip = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bounds_trap(ctx, (unsigned)ep, (unsigned)ip, (uint32_t)offsetof(jello_enum, nfields));
  emit_mov_r32_r32(buf, X64_RDX, (unsigned)ip);
  emit_shl_r64_imm8(buf, X64_RDX, 3);
  emit_add_r64_r64(buf, X64_RDX, (unsigned)ep);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_from_r64_disp(buf, X64_STAGE, X64_RDX, (uint32_t)offsetof(jello_enum, fields));
  emit_unbox_stage_into(ctx, ins->a);
}

static void emit_to_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_box_into_rax(ctx, ins->b);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_from_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(ctx->buf, X64_STAGE, (unsigned)src);
  emit_unbox_stage_into(ctx, ins->a);
}

/** IS_ENUM calls the interpreter's own predicate, so the two cannot disagree. */
static void emit_is_enum(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)src);
  emit_mov_r32_imm32(buf, X64_RSI, ins->imm);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_dynamic_is_enum));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/** Sets ZF exactly when register `r` holds null; a Dynamic's tag is the null tag (jello_is_null),
 * a reference is the null pointer. */
static void emit_null_flags(emit_ctx* ctx, uint8_t r) {
  int s = jit_reg_get_gpr(ctx->ra, r, 1);
  if(vm_reg_kind(ctx->m, ctx->f, r) == JELLO_T_DYNAMIC) {
    emit_tag_cmp(ctx->buf, (unsigned)s, (uint32_t)JELLO_TAG_NULL);
  } else {
    emit_test_r64_r64(ctx->buf, (unsigned)s, (unsigned)s);
  }
}

static void emit_kindof(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_kindof_dynamic));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/** PHYSEQ; boxes both operands and compares the words. The first waits in a stack slot. */
static void emit_physeq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_rsp_adjust_local(buf, 0, 16); // sub rsp, 16
  emit_box_into_rax(ctx, ins->b);
  emit_store_r64_to_mem_rsp(buf, X64_RAX, 0);
  emit_box_into_rax(ctx, ins->c);
  emit_mov_r64_from_mem_rsp(buf, X64_RCX, 0);
  emit_rsp_adjust_local(buf, 1, 16); // add rsp, 16

  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_cmp_r64_r64(buf, X64_RCX, X64_RAX);
  emit_setcc_from_flags(buf, 0x94, (unsigned)dst);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/* The object ops write pool registers (RSI, RDI, R8-R11) directly only after
 * jit_reg_state_discard_currents, so no vreg is left bound to a register they overwrite; operands
 * are fetched and checked first. With two operands, each one is copied out before a move can
 * overwrite its register; into RAX, RCX or RDX, outside the pool, ahead of any pool register
 * write. A value needed after a C call waits in X64_STAGE or a stack slot. */
static void emit_obj_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_imm32(buf, X64_RSI, tid);
  emit_call_fn(buf, (uintptr_t)JIT_C(jello_object_new));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

/** Leaves through the helper trap when the helper just called set vm->trap_code. Clobbers rcx. */
static void emit_helper_trap_check(emit_ctx* ctx) {
  emit_mov_r32_from_r12_disp(ctx->buf, X64_RCX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test_r32_r32(ctx->buf, X64_RCX, X64_RCX);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_HELPER); // JNE: trap_code set
}

static void emit_obj_has_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->m->proto_enabled || ins->imm == JELLO_ATOM___PROTO__) {
    // No prototype chain to follow: `o`'s own slots answer.
    emit_mov_r64_r64(buf, X64_RDI, (unsigned)o);
    emit_mov_r32_imm32(buf, X64_RSI, ins->imm);
    emit_call_fn(buf, (uintptr_t)JIT_C(jello_object_has));
    emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);
  } else {
    emit_mov_r64_r64(buf, X64_RDX, (unsigned)o);
    emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
    emit_mov_r32_imm32(buf, X64_RCX, ins->imm);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_STAGE);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/** jit_obj_chain_get's `keep`; only a Dynamic destination keeps the boxed value. */
static int obj_get_keeps(const emit_ctx* ctx, uint8_t dst) {
  return vm_reg_kind(ctx->m, ctx->f, dst) == JELLO_T_DYNAMIC;
}

/** Calls jit_obj_set_f64 for `o` in X64_STAGE and the atom in ecx, with the F64 register `r`. */
static void emit_obj_set_f64_call(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_movsd_xmm_from_r64_disp(buf, 0, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RDX, X64_STAGE);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_f64);
  emit_helper_trap_check(ctx);
}

/** Whether storing `atom` also sets the prototype link, so that it cannot be a plain store. */
static int obj_atom_links(const emit_ctx* ctx, uint32_t atom) {
  return ctx->m->proto_enabled && atom == JELLO_ATOM___PROTO__;
}

/** Prototype links JIT code follows inline before it leaves the rest of a chain to C. */
#define OBJ_INLINE_LINKS 8u

/** Probes the own table of the Object in X64_STAGE for the constant `atom`, as object.c's
 * lookups do. A hit falls through with the slot's index in r9d, its state in edx and the
 * table's states in r10; a miss, `atom` being absent though the prototype chain may have it,
 * takes the returned rel32 jcc. Clobbers rdx, rsi, rdi and r8 to r11, never rax or rcx. */
static size_t emit_obj_probe(jit_exec_buf* buf, uint32_t atom) {
  emit_mov_r32_from_r64_disp(buf, X64_R8, X64_STAGE, (uint32_t)offsetof(jello_object, cap));
  emit_alu_r32_imm32(buf, 5, X64_R8, 1u);                   // the mask
  emit_mov_r32_imm32(buf, X64_R9, vm_atom_hash(atom) - 1u); // the loop steps first
  emit_mov_r64_from_r64_disp(buf, X64_R10, X64_STAGE, (uint32_t)offsetof(jello_object, states));
  emit_mov_r64_from_r64_disp(buf, X64_R11, X64_STAGE, (uint32_t)offsetof(jello_object, keys));
  size_t loop = buf->len;
  emit_alu_r32_imm32(buf, 0, X64_R9, 1u);
  emit_alu_r32_r32(buf, 0x21, X64_R9, X64_R8);
  emit_mov_r64_r64(buf, X64_RDI, X64_R10);
  emit_add_r64_r64(buf, X64_RDI, X64_R9);
  emit_movzx_r32_m8_r64_disp(buf, X64_RDX, X64_RDI, 0);
  emit_test_r32_r32(buf, X64_RDX, X64_RDX);
  size_t miss = emit_jcc_rel32(buf, 0x84, 0);
  emit_cmp_r32_imm32(buf, X64_RDX, JELLO_OBJ_SLOT_TOMB);
  (void)emit_jcc_rel32(buf, 0x84, (int32_t)(loop - (buf->len + 6)));
  emit_mov_r64_r64(buf, X64_RDI, X64_R9);
  emit_shl_r64_imm8(buf, X64_RDI, 2);
  emit_add_r64_r64(buf, X64_RDI, X64_R11);
  emit_mov_r32_from_r64_disp(buf, X64_RSI, X64_RDI, 0);
  emit_cmp_r32_imm32(buf, X64_RSI, atom);
  (void)emit_jcc_rel32(buf, 0x85, (int32_t)(loop - (buf->len + 6)));
  return miss;
}

/** Leaves in rdi the address of the value of the slot emit_obj_probe hit. */
static void emit_obj_probed_value_addr(jit_exec_buf* buf) {
  emit_mov_r64_r64(buf, X64_RDI, X64_R9);
  emit_shl_r64_imm8(buf, X64_RDI, 3);
  emit_mov_r64_from_r64_disp(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(jello_object, vals));
  emit_add_r64_r64(buf, X64_RDI, X64_RSI);
}

/** Stores `state` in the slot emit_obj_probe hit. Clobbers rdi. */
static void emit_obj_probed_state_set(jit_exec_buf* buf, uint8_t state) {
  emit_mov_r64_r64(buf, X64_RDI, X64_R10);
  emit_add_r64_r64(buf, X64_RDI, X64_R9);
  emit_store_imm8_to_r64_disp(buf, X64_RDI, 0, state);
}

/** OBJ_GET_ATOM. A field is read inline (emit_obj_probe) from the object or, through up to
 * OBJ_INLINE_LINKS prototype links, the first one that has it, a Dynamic destination taking the
 * box from a slot that owns it; anything else takes jit_obj_chain_get from the last object
 * probed, which reports failure only through vm->trap_code. Either way X64_STAGE holds the
 * object, then the boxed value to unbox. */
static void emit_obj_get_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_STAGE, (unsigned)o);

  int chained = ctx->m->proto_enabled && !obj_atom_links(ctx, ins->imm);
  if(chained) emit_mov_r32_imm32(buf, X64_RCX, OBJ_INLINE_LINKS);
  size_t probe = buf->len;
  size_t miss = emit_obj_probe(buf, ins->imm);
  if(obj_get_keeps(ctx, ins->a)) {
    emit_cmp_r32_imm32(buf, X64_RDX, JELLO_OBJ_SLOT_OWNED);
    size_t plain = emit_jcc_rel8(buf, 0x85);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    patch_jcc_rel8_here(buf, plain);
  }
  emit_obj_probed_value_addr(buf);
  emit_mov_r64_from_r64_disp(buf, X64_STAGE, X64_RDI, 0);
  size_t j_done = emit_jmp_rel32(buf, 0);
  patch_jcc_rel32_here(buf, miss);
  if(chained) {
    emit_test_r32_r32(buf, X64_RCX, X64_RCX);
    size_t last = emit_jcc_rel32(buf, 0x84, 0);
    emit_mov_r64_from_r64_disp(buf, X64_RAX, X64_STAGE, (uint32_t)offsetof(jello_object, proto));
    emit_test_r64_r64(buf, X64_RAX, X64_RAX);
    size_t end = emit_jcc_rel32(buf, 0x84, 0); // no prototype
    emit_alu_r32_imm32(buf, 5, X64_RCX, 1u);
    emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);
    (void)emit_jmp_rel32(buf, (int32_t)(probe - (buf->len + 5)));
    patch_jcc_rel32_here(buf, last);
    patch_jcc_rel32_here(buf, end);
  }

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RDX, X64_STAGE);
  emit_mov_r32_imm32(buf, X64_RCX, ins->imm);
  emit_mov_r32_imm32(buf, X64_R8, (uint32_t)obj_get_keeps(ctx, ins->a));
  emit_call_fn(buf, (uintptr_t)jit_obj_chain_get);
  emit_mov_r64_r64(buf, X64_STAGE, X64_RAX); // the boxed value; `o` is dead

  emit_helper_trap_check(ctx);

  patch_jmp_rel32_here(buf, j_done);
  emit_unbox_stage_into(ctx, ins->a);
}

/** OBJ_SET_ATOM. An F64 goes into the box of an own slot that owns one, and another value into
 * an own unsealed slot, inline (emit_obj_probe); anything else, and __proto__ when it links
 * prototypes, takes jit_obj_set_f64 or jit_obj_set_direct. */
static void emit_obj_set_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_STAGE, (unsigned)o);
  if(vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_F64) {
    size_t miss = emit_obj_probe(buf, ins->imm);
    emit_cmp_r32_imm32(buf, X64_RDX, JELLO_OBJ_SLOT_OWNED);
    size_t unowned = emit_jcc_rel32(buf, 0x85, 0);
    emit_obj_probed_value_addr(buf);
    emit_mov_r64_from_r64_disp(buf, X64_RDI, X64_RDI, 0);
    emit_movsd_xmm_from_r64_disp(buf, 0, (unsigned)JIT_REG_BASE, (uint32_t)ins->a * REG_SLOT_BYTES);
    emit_movsd_to_r64_disp(buf, 0, X64_RDI, (uint32_t)offsetof(jello_box_f64, value));
    size_t j_done = emit_jmp_rel32(buf, 0);
    patch_jcc_rel32_here(buf, miss);
    patch_jcc_rel32_here(buf, unowned);
    emit_mov_r32_imm32(buf, X64_RCX, ins->imm);
    emit_obj_set_f64_call(ctx, ins->a);
    patch_jmp_rel32_here(buf, j_done);
    return;
  }
  emit_box_into_rax(ctx, ins->a);
  int inline_set = !obj_atom_links(ctx, ins->imm);
  size_t j_done = 0;
  if(inline_set) {
    size_t miss = emit_obj_probe(buf, ins->imm);
    emit_cmp_r32_imm32(buf, X64_RDX, JELLO_OBJ_SLOT_SEALED);
    size_t sealed = emit_jcc_rel32(buf, 0x84, 0); // JE: the helper traps
    emit_obj_probed_value_addr(buf);
    emit_store_r64_to_r64_disp(buf, X64_RAX, X64_RDI, 0);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    j_done = emit_jmp_rel32(buf, 0);
    patch_jcc_rel32_here(buf, miss);
    patch_jcc_rel32_here(buf, sealed);
  }

  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_mov_r32_imm32(buf, X64_RCX, ins->imm);
  emit_mov_r64_r64(buf, X64_RDX, X64_STAGE);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_direct);
  emit_helper_trap_check(ctx);
  if(inline_set) patch_jmp_rel32_here(buf, j_done);
}

static void emit_obj_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); // JAE: atom_id >= natoms
  emit_mov_r64_r64(buf, X64_RDX, (unsigned)o);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r32_imm32(buf, X64_R8, (uint32_t)obj_get_keeps(ctx, ins->a));
  emit_call_fn(buf, (uintptr_t)jit_obj_chain_get);
  emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);

  emit_helper_trap_check(ctx);

  emit_unbox_stage_into(ctx, ins->a);
}

static void emit_obj_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_STAGE, (unsigned)o);
  if(vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_F64) {
    emit_obj_set_f64_call(ctx, ins->a); // ecx already holds `atom`
    return;
  }
  // `atom` waits on the stack across the boxing, for jit_obj_set_direct.
  emit_rsp_adjust_local(buf, 0, 16); // sub rsp, 16
  emit_store_r64_to_mem_rsp(buf, X64_RCX, 0);
  emit_box_into_rax(ctx, ins->a);
  emit_mov_r64_from_mem_rsp(buf, X64_RCX, 0);
  emit_rsp_adjust_local(buf, 1, 16); // add rsp, 16

  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_mov_r64_r64(buf, X64_RDX, X64_STAGE);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_direct);
  emit_helper_trap_check(ctx);
}

static void emit_obj_has(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_r64_r64(buf, X64_RDX, (unsigned)o);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->m->proto_enabled) {
    // No prototype chain to follow: `o`'s own slots answer.
    emit_mov_r64_r64(buf, X64_RDI, X64_RDX);
    emit_mov_r32_r32(buf, X64_RSI, X64_RCX);
    emit_call_fn(buf, (uintptr_t)JIT_C(jello_object_has));
    emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);
  } else {
    emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_mov_r64_r64(buf, X64_STAGE, X64_RAX);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_STAGE);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_remove(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)o);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RSI, X64_RAX);
  emit_mov_r32_r32(buf, X64_RDX, X64_RCX);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_remove);
  emit_helper_trap_check(ctx);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_clear(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RSI, X64_RAX);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_clear);
  emit_helper_trap_check(ctx);
}

static void emit_obj_copy(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)src, (unsigned)src);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_r64(buf, X64_RSI, X64_RAX);
  emit_call_fn(buf, (uintptr_t)JIT_C(jello_object_copy));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static int emit_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jit_ir_op)ins->op) {
    case JIR_NOP: break;
    case JIR_MOV: {
      jello_type_kind dk = vm_reg_kind(ctx->m, ctx->f, ins->a);
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      if(jello_slot_size(dk) == 8u) {
        emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      }
      break;
    }
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_I64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_imm64(ctx->buf, (unsigned)d, (uint64_t)ctx->m->const_i64[ins->imm]);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F16: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm & 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t bits;
      memcpy(&bits, &ctx->m->const_f64[ins->imm], 8);
      emit_mov_r64_imm64(ctx->buf, (unsigned)d, bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_BOOL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm ? 1u : 0u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_BIN_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_r32_r32(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_r32_r32(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_imul_r32_r32(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_r32_r32(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_r32_r32(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_r32_r32(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV:
        case JIR_BIN_MOD: {
          emit_divmod_guard_i32(ctx, (unsigned)rhs, (unsigned)lhs);
          jit_reg_unbind_preg(ctx->ra, X64_RAX);
          jit_reg_unbind_preg(ctx->ra, X64_RDX);
          emit_mov_r32_r32(ctx->buf, X64_RAX, (unsigned)lhs);
          emit_cdq(ctx->buf);
          emit_idiv_r32(ctx->buf, (unsigned)rhs);
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD)
            emit_mov_r32_r32(ctx->buf, (unsigned)dst, X64_RDX);
          else emit_mov_r32_r32(ctx->buf, (unsigned)dst, X64_RAX);
          break;
        }
        case JIR_BIN_SHL:
        case JIR_BIN_SHR: {
          emit_mov_r32_r32(ctx->buf, X64_RCX, (unsigned)rhs);
          emit_alu_r32_imm8(ctx->buf, 0x04, X64_RCX, (ins->imm == JIR_BIN_SHL) ? 31 : 31);
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_shl_cl_r32(ctx->buf, (unsigned)dst);
          else emit_shr_cl_r32(ctx->buf, (unsigned)dst);
          break;
        }
        default: return 0;
      }
      jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
      break;
    }
    case JIR_BIN_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_r64(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_r64_r64(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_r64_r64(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_imul_r64_r64(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_r64_r64(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_r64_r64(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_r64_r64(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV:
        case JIR_BIN_MOD: {
          emit_divmod_guard_i64(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_mov_r64_r64(ctx->buf, X64_RAX, (unsigned)lhs);
          emit_cqo(ctx->buf);
          emit_idiv_r64(ctx->buf, (unsigned)rhs);
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD)
            emit_mov_r64_r64(ctx->buf, (unsigned)dst, X64_RDX);
          else emit_mov_r64_r64(ctx->buf, (unsigned)dst, X64_RAX);
          break;
        }
        case JIR_BIN_SHL:
        case JIR_BIN_SHR: {
          emit_mov_r32_r32(ctx->buf, X64_RCX, (unsigned)rhs);
          emit_alu_r32_imm8(ctx->buf, 0x04, X64_RCX, 63);
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_shl_cl_r64(ctx->buf, (unsigned)dst);
          else emit_shr_cl_r64(ctx->buf, (unsigned)dst);
          break;
        }
        default: return 0;
      }
      jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
      break;
    }
    case JIR_BIN_F16:
      if((jello_jir_bin)ins->imm == JIR_BIN_ADD) emit_f16_binop(ctx, ins, 0x58);
      else if((jello_jir_bin)ins->imm == JIR_BIN_SUB) emit_f16_binop(ctx, ins, 0x5C);
      else if((jello_jir_bin)ins->imm == JIR_BIN_MUL) emit_f16_binop(ctx, ins, 0x59);
      else return 0;
      break;
    case JIR_BIN_F32: emit_bin_f32(ctx, ins); break;
    case JIR_BIN_F64: emit_bin_f64(ctx, ins); break;
    case JIR_ADD_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x00, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_SUB_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x05, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_MUL_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_imul_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_r32(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_r64(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_xor_r32_imm32(ctx->buf, (unsigned)d, 0x80000000u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mov_r64_imm64(ctx->buf, X64_RAX, 0x8000000000000000ULL);
      emit_alu_r64_r64(ctx->buf, 0x31, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NOT_BOOL: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_xor_r32_imm32(ctx->buf, (unsigned)d, 1u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r32_r32(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C,
                            (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_EQ_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r32_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, 0x94, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r64_r64(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C,
                            (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_LT_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r32_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, 0x9C, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_F32: emit_cmp_float(ctx, ins, 0); break;
    case JIR_CMP_F64: emit_cmp_float(ctx, ins, 1); break;
    case JIR_CONV:
      if(!emit_conv(ctx, ins)) return 0;
      break;
    case JIR_BYTES_NEW: emit_bytes_new(ctx, ins); break;
    case JIR_BYTES_LEN: emit_bytes_len(ctx, ins); break;
    case JIR_BYTES_GET_U8: emit_bytes_get_u8(ctx, ins); break;
    case JIR_BYTES_SET_U8: emit_bytes_set_u8(ctx, ins); break;
    case JIR_BYTES_EQ: emit_bytes_eq(ctx, ins); break;
    case JIR_BYTES_CONCAT2: emit_bytes_concat2(ctx, ins); break;
    case JIR_BYTES_BITOP: emit_bytes_bitop(ctx, ins); break;
    case JIR_BYTES_READ: emit_bytes_read(ctx, ins); break;
    case JIR_BYTES_WRITE: emit_bytes_write(ctx, ins); break;
    case JIR_BYTES_BLIT: emit_bytes_blit(ctx, ins); break;
    case JIR_BITS_WRITE: emit_bits_write(ctx, ins); break;
    case JIR_BYTES_FREEZE: emit_bytes_freeze(ctx, ins); break;
    case JIR_BITS_READ: emit_bits_read(ctx, ins); break;
    case JIR_STRUCT_NEW: emit_struct_new(ctx, ins); break;
    case JIR_STRUCT_GET: emit_struct_get(ctx, ins); break;
    case JIR_STRUCT_SET: emit_struct_set(ctx, ins); break;
    case JIR_CONST_NULL: {
      // Null is the null tag in a Dynamic register, the null pointer in a reference register.
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t null_bits =
          vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_DYNAMIC ? (uint64_t)JELLO_TAG_NULL : 0u;
      emit_mov_r64_imm64(ctx->buf, (unsigned)d, null_bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_ATOM: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_TO_DYN: emit_to_dyn(ctx, ins); break;
    case JIR_FROM_DYN: emit_from_dyn(ctx, ins); break;
    case JIR_KINDOF: emit_kindof(ctx, ins); break;
    case JIR_PHYSEQ: emit_physeq(ctx, ins); break;
    case JIR_IS_ENUM: emit_is_enum(ctx, ins); break;
    case JIR_IS_NULL: {
      emit_null_flags(ctx, ins->b);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_setcc_from_flags(ctx->buf, 0x94, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NULL_CHECK:
      emit_null_flags(ctx, ins->a);
      emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
      break;
    case JIR_OBJ_NEW: emit_obj_new(ctx, ins); break;
    case JIR_OBJ_HAS_ATOM: emit_obj_has_atom(ctx, ins); break;
    case JIR_OBJ_GET_ATOM: emit_obj_get_atom(ctx, ins); break;
    case JIR_OBJ_SET_ATOM: emit_obj_set_atom(ctx, ins); break;
    case JIR_OBJ_GET: emit_obj_get(ctx, ins); break;
    case JIR_OBJ_SET: emit_obj_set(ctx, ins); break;
    case JIR_OBJ_HAS: emit_obj_has(ctx, ins); break;
    case JIR_OBJ_REMOVE: emit_obj_remove(ctx, ins); break;
    case JIR_OBJ_CLEAR: emit_obj_clear(ctx, ins); break;
    case JIR_OBJ_COPY: emit_obj_copy(ctx, ins); break;
    case JIR_ARRAY_LEN: emit_array_len(ctx, ins); break;
    case JIR_ARRAY_GET: emit_array_get(ctx, ins); break;
    case JIR_ARRAY_SET: emit_array_set(ctx, ins); break;
    case JIR_ENUM_TAG: emit_enum_tag(ctx, ins); break;
    case JIR_ENUM_GET: emit_enum_get(ctx, ins); break;
    case JIR_GLOBAL_GET: emit_global_get(ctx, ins); break;
    case JIR_GLOBAL_SET: emit_global_set(ctx, ins); break;
    case JIR_SLOW:
    case JIR_CALL_NATIVE: emit_handler_call(ctx); break;
    case JIR_RET: break;
    default: return 0;
  }
  return 1;
}

/** Bytes to reserve for a function compiled alone, which makes no calls. */
static size_t emit_buf_cap(uint32_t ninsns) {
  size_t cap = 256u + (size_t)ninsns * JIT_EMIT_PER_INSN_BUDGET + 512u;
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
}

int jello_jit_emit_function_into(const jello_bc_module* m, uint32_t fi, jit_exec_buf* buf,
                                 jello_jit_func_code* out_fc, const void* const* entries) {
  if(fi >= m->nfuncs) return 0;
  const jello_bc_function* f = &m->funcs[fi];

  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return jit_decline(out_fc, JIT_DECLINE_IR_BUILD, f, UINT32_MAX);
  if(!jello_jit_ir_verify(ir, f)) {
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_VERIFY, f, UINT32_MAX);
  }

  jello_type_kind ret_kind = JELLO_T_VOID;
  uint32_t bad_pc = UINT32_MAX;
  if(!jit_ir_shape_ok(ir, m, f, fi, &ret_kind, entries != NULL, &bad_pc)) {
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_SHAPE, f, bad_pc);
  }

  uint8_t* is_target = (uint8_t*)calloc(f->ninsns, 1);
  uint8_t* discarded = (uint8_t*)calloc(f->ninsns, 1);
  size_t* native_off = (size_t*)malloc(sizeof(size_t) * (size_t)f->ninsns);
  uint8_t* is_loop_anchor = (uint8_t*)calloc(f->ninsns, 1);
  if(!is_target || !discarded || !native_off || !is_loop_anchor) {
    free(is_target);
    free(discarded);
    free(native_off);
    free(is_loop_anchor);
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_OOM, f, UINT32_MAX);
  }
  for(uint32_t i = 0; i < f->ninsns; i++) native_off[i] = (size_t)-1;

  uint32_t nloop_anchors = 0, ncatch = 0;
  uint32_t* catch_pcs = NULL;
  if(!jit_scan_targets(ir, f->ninsns, is_target, is_loop_anchor, &nloop_anchors, &catch_pcs,
                       &ncatch)) {
    free(is_target);
    free(discarded);
    free(native_off);
    free(is_loop_anchor);
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_OOM, f, UINT32_MAX);
  }

  // Branch targets bound the call sites' survive windows.
  jit_survive_plan splan;
  if(!jit_survive_plan_build(ir, m, f, is_target, &splan)) {
    free(is_target);
    free(discarded);
    free(native_off);
    free(is_loop_anchor);
    free(catch_pcs);
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_SURVIVE, f, UINT32_MAX);
  }

  size_t blob_start = buf->len;
  // A declined function leaves no code behind.
  int buf_overflowed_before = buf->overflowed;
  buf->overflowed = 0;
  emit_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = buf;
  ctx.m = m;
  ctx.f = f;
  ctx.fi = fi;
  ctx.entries = entries;
  ctx.catch_pcs = catch_pcs;
  ctx.ncatch = ncatch;
  ctx.ret_kind = ret_kind;
  ctx.survive_any_armed = splan.any_armed;

  jit_reg_state ra;
  jit_reg_emit_ctx re;
  re.buf = buf;
  jit_reg_state_init(&ra, ir->nregs, re);
  ctx.ra = &ra;

  size_t body_off = 0;
  emit_prologue(buf, &body_off, splan.any_armed);

  int ok = 1;
  uint32_t fail_pc = UINT32_MAX;
  for(uint32_t i = 0; i < ir->ninsns && fail_pc == UINT32_MAX; i++) {
    const jello_jit_ir_insn* ins = &ir->insns[i];
    uint32_t bc = ins->bc_pc;
    ctx.cur_bc_pc = bc;
    // A branch target is reached with unknown bindings, so it starts with none.
    if(bc < f->ninsns && is_target[bc] && !discarded[bc]) {
      jit_reg_state_discard_currents(&ra);
      discarded[bc] = 1;
    }
    if(bc < f->ninsns && native_off[bc] == (size_t)-1) native_off[bc] = buf->len;

    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if(op == JIR_RET) {
      emit_ret_insn(&ctx, ins->a);
      goto insn_done;
    }
    if(op == JIR_JMP) {
      uint32_t tgt = 0;
      (void)jit_resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt);
      size_t at = emit_jmp_rel32(buf, 0);
      push_branch(&ctx, at, tgt, 0);
      goto insn_done;
    }
    if(op == JIR_JMP_IF) {
      uint32_t tgt = 0;
      (void)jit_resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt);
      int cond = jit_reg_get_gpr(&ra, ins->a, 1);
      emit_test_r32_r32(buf, (unsigned)cond, (unsigned)cond);
      size_t at = emit_jcc_rel32(buf, 0x85, 0);
      push_branch(&ctx, at, tgt, 1);
      goto insn_done;
    }
    if(op == JIR_SAFEPOINT) {
      /* vm_safepoint_check returns at once unless a stop-the-world is requested, so the call is
       * skipped when heap->stop_requested reads 0; an x86-64 load is already sequentially
       * consistent, as its atomic_load is. The bindings are dropped either way, for the path
       * that calls. */
      emit_mov_r64_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, heap));
      emit_cmp_m32_r64_disp_imm8(buf, X64_RAX, (uint32_t)offsetof(jello_heap, stop_requested), 0);
      size_t no_stop = emit_jcc_rel8(buf, 0x84); // JE: stop_requested == 0
      emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (uintptr_t)JIT_C(vm_safepoint_check));
      patch_jcc_rel8_here(buf, no_stop);
      jit_reg_state_clobber_call(&ra);
      // A reduction; the charge that ends the window calls vm_reductions_out.
      emit_sub_m32_r12_disp_imm8(buf, (uint32_t)offsetof(jello_vm, reductions), 1);
      size_t in_window = emit_jcc_rel32(buf, 0x8F, 0); // JG: reductions left
      emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (uintptr_t)JIT_C(vm_reductions_out));
      emit_test_r32_r32(buf, X64_RAX, X64_RAX);
      emit_jcc_trap(&ctx, 0x85, JIT_TRAP_HELPER); // JNE: out of fuel
      patch_jcc_rel32_here(buf, in_window);
      goto insn_done;
    }
    if(op == JIR_CALLR) {
      emit_callr_insn(&ctx, ins);
      goto insn_done;
    }
    if(op == JIR_TRY) {
      emit_try_insn(&ctx, ins);
      goto insn_done;
    }
    if(op == JIR_ENDTRY) {
      emit_endtry_insn(&ctx);
      goto insn_done;
    }
    if(op == JIR_THROW) {
      emit_throw_insn(&ctx, ins);
      goto insn_done;
    }
    if(op == JIR_CALL_SELF || op == JIR_CALL_DIRECT) {
      // An armed call keeps its candidate in R15, loaded from the always-current home.
      if(splan.armed[i]) jit_reg_survive_hold(&ra, splan.candidate[i]);
      ok = emit_call_insn(&ctx, ins, op);
      goto insn_done;
    }
    ok = emit_insn(&ctx, ins);
  insn_done:
    /* The consumer read the held value through jit_reg_get_gpr, which knows about the hold; the
     * hold ends after it. */
    if(splan.is_consumer[i] && jit_reg_survive_is_held(&ra)) jit_reg_survive_release(&ra);
    jit_reg_unpin_all(&ra);
    if(!ok || ctx.oom || buf->overflowed) fail_pc = bc;
  }

  if(fail_pc != UINT32_MAX) ok = 0;
  size_t epilogue_ok_off = 0;
  size_t stack_overflow_off = 0;
  size_t trap_off_by_kind[JIT_TRAP_KIND_COUNT];
  if(ok) {
    epilogue_ok_off = buf->len;
    emit_epilogue_ok_tail(&ctx);
    trap_off_by_kind[JIT_TRAP_ARITH] = buf->len;
    emit_epilogue_trap_generic(&ctx, (uintptr_t)jit_trap_arithmetic);
    trap_off_by_kind[JIT_TRAP_NULL] = buf->len;
    emit_epilogue_trap_generic(&ctx, (uintptr_t)jit_trap_null_deref);
    trap_off_by_kind[JIT_TRAP_BOUNDS] = buf->len;
    emit_epilogue_trap_generic(&ctx, (uintptr_t)jit_trap_bounds);
    trap_off_by_kind[JIT_TRAP_TYPE] = buf->len;
    emit_epilogue_trap_generic(&ctx, (uintptr_t)jit_trap_type_mismatch);
    trap_off_by_kind[JIT_TRAP_PASSTHROUGH] = buf->len;
    if(ctx.ncatch) emit_catch_dispatch(&ctx);
    else emit_epilogue_trap_generic(&ctx, 0);
    trap_off_by_kind[JIT_TRAP_CALLEE] = trap_off_by_kind[JIT_TRAP_PASSTHROUGH];
    trap_off_by_kind[JIT_TRAP_HELPER] = trap_off_by_kind[JIT_TRAP_PASSTHROUGH];
    trap_off_by_kind[JIT_TRAP_LIMIT] = buf->len;
    emit_epilogue_trap_generic(&ctx, (uintptr_t)jit_trap_limit);
    stack_overflow_off = buf->len;
    emit_epilogue_stack_overflow(&ctx);
    patch_trap_jumps(&ctx, trap_off_by_kind);
    patch_stack_overflow_jumps(&ctx, stack_overflow_off);
    for(size_t i = 0; i < ctx.ret_jump_count; i++) {
      size_t at = ctx.ret_jumps[i];
      patch_jmp_rel32(buf, at, (int32_t)(epilogue_ok_off - (at + 5)));
    }
    for(size_t i = 0; i < ctx.branch_count; i++) {
      size_t at = ctx.branches[i].at;
      uint32_t tgt = ctx.branches[i].bc_tgt;
      if(tgt >= f->ninsns || native_off[tgt] == (size_t)-1) {
        ok = 0;
        break;
      }
      size_t tgt_off = native_off[tgt];
      if(ctx.branches[i].is_jcc) patch_jcc_rel32(buf, at, (int32_t)(tgt_off - (at + 6)));
      else patch_jmp_rel32(buf, at, (int32_t)(tgt_off - (at + 5)));
    }
  }

  if(ok && nloop_anchors > 0) {
    out_fc->loop_anchor_pcs = (uint32_t*)malloc(sizeof(uint32_t) * nloop_anchors);
    out_fc->loop_anchor_offs = (size_t*)malloc(sizeof(size_t) * nloop_anchors);
    if(!out_fc->loop_anchor_pcs || !out_fc->loop_anchor_offs) {
      free(out_fc->loop_anchor_pcs);
      free(out_fc->loop_anchor_offs);
      out_fc->loop_anchor_pcs = NULL;
      out_fc->loop_anchor_offs = NULL;
      ok = 0;
    } else {
      uint32_t w = 0;
      for(uint32_t pc = 0; pc < f->ninsns; pc++) {
        if(!is_loop_anchor[pc]) continue;
        out_fc->loop_anchor_pcs[w] = pc;
        out_fc->loop_anchor_offs[w] = native_off[pc] - blob_start;
        w++;
      }
      out_fc->nloop_anchors = w;
    }
  } else {
    out_fc->loop_anchor_pcs = NULL;
    out_fc->loop_anchor_offs = NULL;
    out_fc->nloop_anchors = 0;
  }

  jit_reg_state_free(&ra);
  jit_survive_plan_free(&splan);
  jello_jit_ir_free(ir);
  free(ctx.trap_jumps);
  free(ctx.branches);
  free(ctx.ret_jumps);
  free(ctx.stack_overflow_jumps);
  free(is_target);
  free(discarded);
  free(native_off);
  free(is_loop_anchor);
  free(catch_pcs);

  if(buf->overflowed || ctx.oom) ok = 0;
  buf->overflowed = buf_overflowed_before;
  if(!ok) {
    free(out_fc->loop_anchor_pcs);
    free(out_fc->loop_anchor_offs);
    out_fc->loop_anchor_pcs = NULL;
    out_fc->loop_anchor_offs = NULL;
    out_fc->nloop_anchors = 0;
    buf->len = blob_start;
    return jit_decline(out_fc, ctx.oom ? JIT_DECLINE_OOM : JIT_DECLINE_EMIT, f, fail_pc);
  }

  out_fc->compiled = 1;
  out_fc->decline_reason = JIT_DECLINE_NONE;
  out_fc->decline_pc = UINT32_MAX;
  out_fc->decline_op = 0;
  out_fc->blob_off = blob_start;
  out_fc->entry_off = 0;
  out_fc->body_off = body_off - blob_start;
  out_fc->size = buf->len - blob_start;
  return 1;
}

int jello_jit_emit_mixed_stubs(jit_exec_buf* buf, size_t* enter_off, size_t* bridge_off,
                               size_t* lazy_off) {
  /* enter(vm, frame_base, out, body): the interpreter's way into a compiled function whose frame
   * it already pushed (has_caller set). The body's nested RET and trap tails pop that frame,
   * write the result into the caller's frame and return here without restoring what the entry
   * prologue saves, so this stub saves and restores the same registers itself. JIT_REG_SURVIVE
   * needs no save; an armed body keeps it in its own slot. */
  *enter_off = buf->len;
  size_t unused;
  emit_prologue(buf, &unused, 0);
  emit_call_jit_code(buf, X64_RCX);
  emit_restore_callee_saved(buf, 0);
  emit_ret(buf);

  /* bridge: the target of a JIT call to a declined function. The caller already pushed the
   * callee frame; this runs it in the interpreter (jit_run_interp_top), then points JIT_REG_BASE
   * back at the caller's frame, as a compiled callee's RET or trap tail would. The caller checks
   * vm->trap_code after the call either way. RSP is 16-byte aligned here, as at a body_off. */
  *bridge_off = buf->len;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_run_interp_top);
  emit_reload_jit_reg_base_x64(buf);
  emit_ret(buf);

  /* lazy: the entry-table target of a function not yet compiled. The caller already pushed its
   * frame; jit_lazy_target compiles it when it is due and returns its body or the bridge, which
   * this jumps to with the caller's return address still on the stack, so the callee returns
   * straight to the call site. */
  *lazy_off = buf->len;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_lazy_target);
  emit_jmp_r64(buf, X64_RAX);
  return !buf->overflowed;
}

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code) {
  out_code->buf.mem = NULL;
  out_code->buf.cap = 0;
  out_code->buf.len = 0;
  out_code->buf.finalized = 0;
  out_code->buf.overflowed = 0;
  out_code->entry_off = 0;
  out_code->body_off = 0;
  out_code->loop_anchor_pcs = NULL;
  out_code->loop_anchor_offs = NULL;
  out_code->nloop_anchors = 0;

  if(fi >= m->nfuncs) return 0;
  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return 0;
  uint32_t ninsns = ir->ninsns;
  jello_jit_ir_free(ir);

  jit_exec_buf buf = jit_alloc_exec_buf(emit_buf_cap(ninsns));
  if(!buf.mem) return 0;

  jello_jit_func_code fc;
  memset(&fc, 0, sizeof(fc));
  if(!jello_jit_emit_function_into(m, fi, &buf, &fc, NULL)) {
    jit_free_exec_buf(&buf);
    return 0;
  }

  if(!jit_finalize_exec_buf(&buf)) {
    free(fc.loop_anchor_pcs);
    free(fc.loop_anchor_offs);
    jit_free_exec_buf(&buf);
    return 0;
  }

  out_code->buf = buf;
  out_code->entry_off = fc.entry_off;
  out_code->body_off = fc.body_off;
  out_code->loop_anchor_pcs = fc.loop_anchor_pcs;
  out_code->loop_anchor_offs = fc.loop_anchor_offs;
  out_code->nloop_anchors = fc.nloop_anchors;
  return 1;
}

void jello_jit_code_free(jello_jit_code* code) {
  free(code->loop_anchor_pcs);
  free(code->loop_anchor_offs);
  code->loop_anchor_pcs = NULL;
  code->loop_anchor_offs = NULL;
  code->nloop_anchors = 0;
  jit_free_exec_buf(&code->buf);
}
