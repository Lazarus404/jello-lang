// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * arm64 JIT emitter; compiles a function's JIR to native code with its prologue, trap and return
 * exits, and emits the enter, bridge and lazy stubs. */

#include <jello/internal/jit_internal.h>
#include <jello/internal/jit_arm64.h>
#include <jello.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  A64_X0 = 0,
  A64_X1 = 1,
  A64_X2 = 2,
  A64_X3 = 3,
  A64_X4 = 4,
  A64_X5 = 5,
  A64_X9 = 9,
  A64_X10 = 10,
  A64_X11 = 11,
  A64_X12 = 12,
  A64_X13 = 13,
  A64_X14 = 14,
  A64_X15 = 15,
  A64_LAYOUT = 27,
  A64_FP = 29,
  A64_LR = 30,
  A64_SP = 31,
  A64_IP0 = 16,
  A64_IP1 = 17,
  /* Scratch outside the x9-x14 GPR pool, so emitted code writes them without telling the
   * allocator. TMP1 is IP0, which emit_call_fn also uses; TMP2 is IP1. A *_disp load or store
   * writes none of them; emit_disp_base writes only the one its caller names. */
  A64_TMP0 = 15,
  A64_TMP1 = 16,
  A64_TMP2 = 17,
  A64_D_TMP0 = 16,
  A64_D_TMP1 = 17, /**< FP scratch outside the d8-d15 pool. */
  A64_NO_GPR = 32, /**< emit_disp_base's `data` for an FP register. */
};

/** Appends one instruction word. The common case stays inline; jit_exec_buf_write handles a
 * full or finalised buffer and records the overflow. */
static void emit_u32(jit_exec_buf* buf, uint32_t word) {
  if(buf->mem && !buf->finalized && buf->limit - buf->len >= 4u) {
    memcpy(buf->mem + buf->len, &word, 4);
    buf->len += 4;
    return;
  }
  jit_exec_buf_write(buf, &word, 4);
}

/** The FP helpers below take pool indices; index i is d(8 + i). */
static uint8_t pool_dreg_i(unsigned pool_idx) { return (uint8_t)(8u + pool_idx); }

static void emit_mov_x_imm64(jit_exec_buf* buf, uint8_t rd, uint64_t imm) {
  emit_u32(buf, a64_movz_x(rd, (uint16_t)(imm & 0xFFFFu), 0));
  if(imm > 0xFFFFu) emit_u32(buf, a64_movk_x(rd, (uint16_t)((imm >> 16) & 0xFFFFu), 16));
  if(imm > 0xFFFFFFFFu) emit_u32(buf, a64_movk_x(rd, (uint16_t)((imm >> 32) & 0xFFFFu), 32));
  if(imm > 0xFFFFFFFFFFFFu) emit_u32(buf, a64_movk_x(rd, (uint16_t)((imm >> 48) & 0xFFFFu), 48));
}

static void emit_mov_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src);

/** Calls the C function `fn` through IP0, saving LR in a 16-byte stack slot around the BLR; a
 * JIT activation keeps its own return address in LR until its RET. */
static void emit_call_fn(jit_exec_buf* buf, uintptr_t fn) {
  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  emit_mov_x_imm64(buf, A64_IP0, (uint64_t)fn);
  emit_u32(buf, a64_blr_x(A64_IP0));
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
#ifdef JELLO_JIT_SCRIBBLE_CALLS
  /* A checking build (-DJELLO_JIT_SCRIBBLE_CALLS) overwrites x1-x17 after each C call, so
   * emitted code that keeps a value in a caller-saved register across a call fails every time,
   * not only when the callee happens to use it. C helpers therefore return in x0 alone. */
  emit_mov_x_imm64(buf, A64_IP0, 0x5c5c5c5c5c5c5c5cull);
  for(uint8_t r = 1; r < A64_IP0; r++) emit_mov_x_rr(buf, r, A64_IP0);
  emit_mov_x_rr(buf, A64_IP1, A64_IP0);
#endif
}

/** Whether the placeholder at `at` was written. One that did not fit was not (the buffer recorded
 * the overflow and the function will decline), and its offset may lie past the writable end. */
static int placeholder_written(const jit_exec_buf* buf, size_t at) { return at + 4u <= buf->len; }

static int32_t off_insns_at(size_t at, size_t target) {
  return (int32_t)((int64_t)target - (int64_t)at) / 4;
}

static size_t emit_b_placeholder(jit_exec_buf* buf) {
  size_t at = buf->len;
  emit_u32(buf, a64_b(0));
  return at;
}

static void patch_b(jit_exec_buf* buf, size_t at, size_t target) {
  if(!placeholder_written(buf, at)) return;
  int32_t off = off_insns_at(at, target);
  uint32_t word = a64_b(off);
  memcpy(buf->mem + at, &word, 4);
}

/** Emits a B.cond placeholder and returns its offset. `x64_cc` is an x64 Jcc condition byte
 * (0F 8x) or 0x9C (setl); 0x84 EQ, 0x85 NE, 0x82 LO, 0x83 HS, 0x86 LS, 0x87 HI, 0x8C and 0x9C LT;
 * any other value becomes EQ. */
static size_t emit_b_cond_placeholder(jit_exec_buf* buf, uint8_t x64_cc, int32_t rel_unused) {
  (void)rel_unused;
  size_t at = buf->len;
  a64_cond cond;
  switch(x64_cc) {
    case 0x84: cond = A64_EQ; break;
    case 0x85: cond = A64_NE; break;
    case 0x82: cond = A64_LO; break;
    case 0x83: cond = A64_HS; break;
    case 0x86: cond = A64_LS; break;
    case 0x87: cond = A64_HI; break;
    case 0x8C: cond = A64_LT; break;
    case 0x8F: cond = A64_GT; break;
    case 0x9C: cond = A64_LT; break;
    default: cond = A64_EQ; break;
  }
  emit_u32(buf, a64_b_cond(cond, 0));
  return at;
}

static void patch_b_cond(jit_exec_buf* buf, size_t at, size_t target) {
  if(!placeholder_written(buf, at)) return;
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t cond = old & 0xFu;
  uint32_t word = 0x54000000u | (((uint32_t)off & 0x7FFFFu) << 5) | cond;
  memcpy(buf->mem + at, &word, 4);
}

static size_t emit_cbz_w_placeholder(jit_exec_buf* buf, unsigned reg) {
  size_t at = buf->len;
  emit_u32(buf, a64_cbz_w((uint8_t)reg, 0));
  return at;
}

static size_t emit_cbnz_w_placeholder(jit_exec_buf* buf, unsigned reg) {
  size_t at = buf->len;
  emit_u32(buf, a64_cbnz_w((uint8_t)reg, 0));
  return at;
}

static void patch_cbz_w(jit_exec_buf* buf, size_t at, size_t target) {
  if(!placeholder_written(buf, at)) return;
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t rt = old & 0x1Fu;
  uint32_t word = 0x34000000u | (((uint32_t)off & 0x7FFFFu) << 5) | rt;
  memcpy(buf->mem + at, &word, 4);
}

static void patch_cbnz_w(jit_exec_buf* buf, size_t at, size_t target) {
  if(!placeholder_written(buf, at)) return;
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t rt = old & 0x1Fu;
  uint32_t word = 0x35000000u | (((uint32_t)off & 0x7FFFFu) << 5) | rt;
  memcpy(buf->mem + at, &word, 4);
}

static void emit_ret_insn_a64(jit_exec_buf* buf) { emit_u32(buf, a64_ret()); }

static void emit_mov_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_w_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_uxtw_x_w(jit_exec_buf* buf, unsigned xd, unsigned ws) {
  emit_u32(buf, a64_uxtw_x_w((uint8_t)xd, (uint8_t)ws));
}

static void emit_mov_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_x_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_mov_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_u32(buf, a64_movz_w((uint8_t)dst, (uint16_t)(imm & 0xFFFFu), 0));
  if(imm > 0xFFFFu) emit_u32(buf, a64_movk_w((uint8_t)dst, (uint16_t)(imm >> 16), 16));
}

static void emit_mov_w0_imm32(jit_exec_buf* buf, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_X0, imm);
}

static void emit_mov_x_imm64_reg(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  emit_mov_x_imm64(buf, (uint8_t)dst, imm);
}

static void emit_store_out_as_w(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_OUT, disp));
}

static void emit_store_out_kind_imm32(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP1, imm);
  emit_store_out_as_w(buf, disp, A64_TMP1);
}

static void emit_store_out_as_x(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)JIT_REG_OUT, disp));
}

static void emit_ldr_x_vm(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, (uint8_t)JIT_REG_VM, disp));
}

static void emit_ldr_w_vm(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, (uint8_t)JIT_REG_VM, disp));
}

static void emit_str_w_vm(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_VM, disp));
}

/** Stores `imm` to the jello_vm field at `disp`. AArch64 has no store-immediate form, so the
 * constant goes through A64_TMP1 (x16), outside the register pool. */
static void emit_store_imm32_to_vm_field(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP1, imm);
  emit_str_w_vm(buf, A64_TMP1, disp);
}

/** Whether an access of `size` bytes can encode `disp` as its scaled 12-bit offset. */
static int disp_encodes(uint32_t disp, uint32_t size) {
  return disp % size == 0 && disp / size <= 0xFFFu;
}

/** Returns a base register from which an access of `size` bytes reaches `base` + `*disp` with an
 * encodable offset. A displacement that does not encode is added into `scratch`, which is returned
 * with `*disp` zeroed. The caller names `scratch`, so it cannot hold a live value unawares; it must
 * be neither `base` nor `data` (the GPR loaded or stored, or A64_NO_GPR). */
static unsigned emit_disp_base(jit_exec_buf* buf, unsigned base, unsigned data, unsigned scratch,
                               uint32_t* disp, uint32_t size) {
  if(disp_encodes(*disp, size)) return base;
  assert(scratch != base && scratch != data);
  emit_mov_w_imm32(buf, scratch, *disp);
  emit_u32(buf, a64_add_x_rr((uint8_t)scratch, (uint8_t)base, (uint8_t)scratch));
  *disp = 0;
  return scratch;
}

/* The *_disp loads and stores below take a displacement that encodes; a larger one goes through
 * emit_disp_base first. */
static void emit_ldr_x_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_ldr_w_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_str_w_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_str_x_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_str_w_base(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_BASE, disp));
}

static void emit_str_x_base(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)JIT_REG_BASE, disp));
}

static void emit_add_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_sub_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

/** dst = dst op src, where `opcode` is the x64 ALU opcode byte; 0x01 add, 0x29 sub, 0x21 and,
 * 0x09 orr, 0x31 eor. Other values emit nothing. emit_alu_x_rr is the 64-bit form. */
static void emit_alu_w_rr(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  (void)opcode;
  if(opcode == 0x01) emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x29) emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x21) emit_u32(buf, a64_and_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x09) emit_u32(buf, a64_orr_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x31) emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu_x_rr(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  if(opcode == 0x01) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x29) emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x21) emit_u32(buf, a64_and_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x09) emit_u32(buf, a64_orr_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x31) emit_u32(buf, a64_eor_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

/** dst = dst op imm, where `op_ext` is the x64 /digit; 0 add, 5 sub, 4 and, 6 eor. AND, EOR and
 * immediates above 0xFFF go through A64_TMP0, never a pool register, so `dst` must not be
 * A64_TMP0. */
static void emit_alu_w_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  if(op_ext == 0x00 && imm <= 0xFFFu) emit_u32(buf, a64_add_w_imm((uint8_t)dst, (uint8_t)dst, imm));
  else if(op_ext == 0x05 && imm <= 0xFFFu)
    emit_u32(buf, a64_sub_w_imm((uint8_t)dst, (uint8_t)dst, imm));
  else if(op_ext == 0x04) {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    emit_u32(buf, a64_and_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  } else if(op_ext == 0x06) {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  } else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    if(op_ext == 0x00) emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  }
}

/** The 64-bit form for add (`op_ext` 0) and sub (any other value). */
static void emit_alu_x_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  if(op_ext == 0x00 && imm <= 0xFFFu) emit_u32(buf, a64_add_x_imm((uint8_t)dst, (uint8_t)dst, imm));
  else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    if(op_ext == 0x00) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_neg_w(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_neg_w((uint8_t)dst, (uint8_t)dst));
}

static void emit_neg_x(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_neg_x((uint8_t)dst, (uint8_t)dst));
}

static void emit_cmp_w_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_subs_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

/** Compares `dst` with `imm`. Immediates above 0xFFF go through A64_TMP0, outside the pool, so
 * no live value is overwritten. */
static void emit_cmp_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  if(imm <= 0xFFFu) emit_u32(buf, a64_cmp_w_imm((uint8_t)dst, (uint16_t)imm));
  else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    emit_u32(buf, a64_subs_w_rr(0x1F, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_cmp_x_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_subs_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

/** Sets Z exactly when `reg` == -1 (`cmn xN, #1`), with no scratch register. */
static void emit_cmp_x_eq_neg1(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_cmn_x_imm((uint8_t)reg, 1));
}

static void emit_tst_w_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_tst_x_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

/** Sets `dst` to 1 when the x64 setcc byte `cc_opcode` holds, else 0; 0x9C (setl) is LT
 * (N != V); any other value, such as 0x94 (sete), is EQ. */
static void emit_setcc_from_flags(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  emit_u32(buf, a64_cset_w((uint8_t)dst, cc_opcode == 0x9C ? A64_LT : A64_EQ));
}

static void emit_sxtw_x_w(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sxtw_x_w((uint8_t)dst, (uint8_t)src));
}

static void emit_sxth_w(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sxth_w((uint8_t)dst, (uint8_t)src));
}

/** dst ^= imm, through A64_TMP0. */
static void emit_eor_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP0, imm);
  emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_mul_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_w((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_mul_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_x((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_mul_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP0, imm);
  emit_u32(buf, a64_mul_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_lsl_x_imm3(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_lsl_x_imm((uint8_t)dst, (uint8_t)src, 3));
}

/** The register-shift forms below shift `dst` by the count in A64_TMP0, which the caller loads
 * there; a pool register could hold another live value. */
static void emit_lslv_w(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_lslv_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_asrv_w(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_asrv_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_lslv_x(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_lslv_x((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_asrv_x(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_asrv_x((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_ldrb_w_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 1));
  emit_u32(buf, a64_ldrb_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_ldrh_w_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 2));
  emit_u32(buf, a64_ldrh_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_strb_w_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 1));
  emit_u32(buf, a64_strb_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_strh_w_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 2));
  emit_u32(buf, a64_strh_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_rev_w(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_rev_w((uint8_t)reg, (uint8_t)reg));
}

static void emit_rev_x(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_rev_x((uint8_t)reg, (uint8_t)reg));
}

/** REV16 swaps the bytes within each halfword, so the low 16 bits hold the byte-swapped u16 that
 * BYTES_READ/WRITE U16_BE need; REV would reverse all four bytes. `imm8` is unused. */
static void emit_rev16_w(jit_exec_buf* buf, unsigned reg, uint8_t imm8) {
  (void)imm8;
  emit_u32(buf, a64_rev16_w((uint8_t)reg, (uint8_t)reg));
}

static void emit_fmov_s(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fmov_s(pool_dreg_i(dst), pool_dreg_i(src)));
}

/** The *_s0 helpers move between a pool register and raw s0, the AAPCS64 float argument and
 * return register. */
static void emit_fmov_s_from_s0(jit_exec_buf* buf, unsigned dst_pool) {
  emit_u32(buf, a64_fmov_s(pool_dreg_i(dst_pool), 0));
}

static void emit_fmov_s_to_s0(jit_exec_buf* buf, unsigned src_pool) {
  emit_u32(buf, a64_fmov_s(0, pool_dreg_i(src_pool)));
}

static void emit_fcvtzs_w_from_s0(jit_exec_buf* buf, unsigned dst_gpr) {
  emit_u32(buf, a64_fcvtzs_w_s((uint8_t)dst_gpr, 0));
}

static void emit_scvtf_s_to_s0(jit_exec_buf* buf, unsigned src_gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_s_x(0, (uint8_t)src_gpr));
  else emit_u32(buf, a64_scvtf_s_w(0, (uint8_t)src_gpr));
}

/** dst = dst op src on pool registers, where `op` is the SSE opcode byte; 0x58 add, 0x5C sub,
 * 0x59 mul, 0x5E div. emit_fbin_d is the double form. */
static void emit_fbin_s(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t dd = pool_dreg_i(dst), dn = pool_dreg_i(dst), dm = pool_dreg_i(src);
  if(op == 0x58) emit_u32(buf, a64_fadd_s(dd, dn, dm));
  else if(op == 0x5C) emit_u32(buf, a64_fsub_s(dd, dn, dm));
  else if(op == 0x59) emit_u32(buf, a64_fmul_s(dd, dn, dm));
  else if(op == 0x5E) emit_u32(buf, a64_fdiv_s(dd, dn, dm));
}

static void emit_fmov_d(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fmov_d(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fbin_d(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t dd = pool_dreg_i(dst), dn = pool_dreg_i(dst), dm = pool_dreg_i(src);
  if(op == 0x58) emit_u32(buf, a64_fadd_d(dd, dn, dm));
  else if(op == 0x5C) emit_u32(buf, a64_fsub_d(dd, dn, dm));
  else if(op == 0x59) emit_u32(buf, a64_fmul_d(dd, dn, dm));
  else if(op == 0x5E) emit_u32(buf, a64_fdiv_d(dd, dn, dm));
}

static void emit_fcmp_s(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_fcmp_s(pool_dreg_i(a), pool_dreg_i(b)));
}

static void emit_fcmp_d(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_fcmp_d(pool_dreg_i(a), pool_dreg_i(b)));
}

static void emit_scvtf_s(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_s_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_s_w(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_scvtf_d(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_d_w(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fcvtzs_w_s(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  if(wide) emit_u32(buf, a64_fcvtzs_x_s((uint8_t)gpr, pool_dreg_i(xmm)));
  else emit_u32(buf, a64_fcvtzs_w_s((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fcvtzs_w_d(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  if(wide) emit_u32(buf, a64_fcvtzs_x_d((uint8_t)gpr, pool_dreg_i(xmm)));
  else emit_u32(buf, a64_fcvtzs_w_d((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fcvt_d_s(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fcvt_d_s(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fcvt_s_d(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fcvt_s_d(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fmov_s_w(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  emit_u32(buf, a64_fmov_s_x(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fmov_w_s(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  emit_u32(buf, a64_fmov_x_s((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fmov_d_x(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  emit_u32(buf, a64_fmov_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fmov_x_d(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  emit_u32(buf, a64_fmov_x_d((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_str_x_sp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, A64_SP, disp));
}

static void emit_ldr_x_sp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, A64_SP, disp));
}

static void emit_sp_adjust(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  if(is_add) emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, imm8));
  else emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, imm8));
}

static void emit_ldr_s_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_ldr_s_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_str_s_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_str_s_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_ldr_d_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_ldr_d_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_str_d_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_str_d_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_load_u32_slot_to_w1(jit_exec_buf* buf, uint8_t vreg) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_ldr_w_uimm(A64_X1, (uint8_t)JIT_REG_BASE, off));
}

typedef struct jit_branch_patch {
  size_t at;
  uint32_t bc_tgt;
  uint8_t is_jcc; /**< 0 for B, 1 for B.cond, 2 for CBNZ. */
} jit_branch_patch;

/** The trap exits a compiled function branches to. Most kinds call a jit_trap_* setter for their
 * trap code. PASSTHROUGH is for a trap already set and located (a callee's, or jit_run_handler's),
 * so it only unwinds. CALLEE and HELPER share that exit: CALLEE, a callee's trap, records the call
 * as this frame's site in the trace; HELPER records this instruction as the location, since the C
 * helper set only the code. */
typedef enum jit_trap_kind {
  JIT_TRAP_ARITH = 0,
  JIT_TRAP_NULL,
  JIT_TRAP_BOUNDS,
  JIT_TRAP_TYPE,
  JIT_TRAP_PASSTHROUGH,
  JIT_TRAP_CALLEE,
  JIT_TRAP_HELPER,
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
  uint32_t cur_bc_pc; /**< Bytecode pc being emitted; trap exits record it as the location. */
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
  /** Whether any call site holds a value in JIT_REG_SURVIVE. If so, body_off saves the caller's
   * x28 in a 16-byte stack slot and every return and trap exit restores it. */
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

/** Records the trap location and branches to the stack-overflow exit. Callers branch around it
 * when their check passes. */
static void emit_jmp_stack_overflow(emit_ctx* ctx) {
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  size_t at = emit_b_placeholder(ctx->buf);
  if(!jit_list_push(&ctx->stack_overflow_jumps, &ctx->stack_overflow_jump_count,
                    &ctx->stack_overflow_jump_cap, &at, sizeof(at)))
    ctx->oom = 1;
}

/** Branches, through the placeholder it returns, when sp is at or above vm->native_stack_limit,
 * so a call may nest another activation on the native stack. Clobbers x9. */
static size_t emit_branch_if_native_room(jit_exec_buf* buf) {
  emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, native_stack_limit));
  emit_u32(buf, 0xEB2063FFu | ((uint32_t)A64_X9 << 16)); // cmp sp, x9, uxtx
  return emit_b_cond_placeholder(buf, 0x83, 0);          // HS: at or above the limit
}

/** Traps STACK_OVERFLOW when sp is below vm->native_stack_limit, before a call nests another
 * activation on the native stack. Clobbers x9. */
static void emit_native_stack_check(emit_ctx* ctx) {
  size_t fits = emit_branch_if_native_room(ctx->buf);
  emit_jmp_stack_overflow(ctx);
  patch_b_cond(ctx->buf, fits, ctx->buf->len);
}

/** Points `target`, a call's target, at the bridge when sp is below vm->native_stack_limit, so
 * that a call nested too deep for the native stack runs its callee in the interpreter, which
 * nests no native frame per call, rather than trapping. The bridge is the entry past the
 * functions'. Clobbers x9. */
static void emit_bridge_when_deep(emit_ctx* ctx, unsigned target) {
  jit_exec_buf* buf = ctx->buf;
  size_t fits = emit_branch_if_native_room(buf);
  emit_mov_x_imm64_reg(buf, target, (uint64_t)(uintptr_t)&ctx->entries[ctx->m->nfuncs]);
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)target, (uint8_t)target, 0));
  patch_b_cond(buf, fits, buf->len);
}

static void patch_stack_overflow_jumps(emit_ctx* ctx, size_t so_off) {
  for(size_t i = 0; i < ctx->stack_overflow_jump_count; i++) {
    size_t at = ctx->stack_overflow_jumps[i];
    patch_b(ctx->buf, at, so_off);
  }
}

/** Traps when the flags satisfy `cc` (an x64 Jcc byte, as emit_b_cond_placeholder takes); stores
 * the trap location and branches to the `kind` exit. A B.cond on `cc ^ 1` skips both on the common
 * path; EQ/NE, LO/HS and LS/HI differ only in the low bit of their x64 bytes. PASSTHROUGH and
 * CALLEE skip the location, since their trap is already located and this instruction must not take
 * the blame; CALLEE stores this call as the frame's site instead. */
static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc, uint8_t kind) {
  size_t skip = emit_b_cond_placeholder(ctx->buf, (uint8_t)(cc ^ 1u), 0);
  if(kind == JIT_TRAP_CALLEE) {
    emit_store_imm32_to_vm_field(ctx->buf, JIT_TRAP_SITE_OFF, ctx->cur_bc_pc + 1u);
  } else if(kind != JIT_TRAP_PASSTHROUGH) {
    emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
    emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  }
  size_t at = emit_b_placeholder(ctx->buf);
  patch_b_cond(ctx->buf, skip, ctx->buf->len);
  push_trap_jump(ctx, at, kind);
}

/** Points each recorded trap jump at its kind's exit, re-encoding it as a B (bits 31:26 are
 * 0b000101) or else as a B.cond. */
static void patch_trap_jumps(emit_ctx* ctx, const size_t* trap_off_by_kind) {
  for(size_t i = 0; i < ctx->trap_jump_count; i++) {
    size_t at = ctx->trap_jumps[i].at;
    if(!placeholder_written(ctx->buf, at)) continue;
    size_t trap_off = trap_off_by_kind[ctx->trap_jumps[i].kind];
    uint32_t word;
    memcpy(&word, ctx->buf->mem + at, 4);
    if((word >> 26) == 0x5u) patch_b(ctx->buf, at, trap_off);
    else patch_b_cond(ctx->buf, at, trap_off);
  }
}

static void push_ret_jump(emit_ctx* ctx, size_t at) {
  if(!jit_list_push(&ctx->ret_jumps, &ctx->ret_jump_count, &ctx->ret_jump_cap, &at, sizeof(at)))
    ctx->oom = 1;
}

/** Traps ARITH on division by zero and on INT32_MIN / -1, whose quotient overflows. */
static void emit_divmod_guard_i32(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_tst_w_rr(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  emit_cmp_w_imm32(ctx->buf, div_gpr, (uint32_t)-1);
  size_t jne = ctx->buf->len;
  emit_b_cond_placeholder(ctx->buf, 0x85, 0);
  emit_cmp_w_imm32(ctx->buf, lhs_gpr, (uint32_t)INT32_MIN);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_b_cond(ctx->buf, jne, ok);
}

/** Traps ARITH on division by zero and on INT64_MIN / -1. */
static void emit_divmod_guard_i64(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_tst_x_rr(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  emit_cmp_x_eq_neg1(ctx->buf, div_gpr);
  size_t jne = ctx->buf->len;
  emit_b_cond_placeholder(ctx->buf, 0x85, 0);
  emit_mov_x_imm64_reg(ctx->buf, A64_TMP0, (uint64_t)INT64_MIN);
  emit_cmp_x_rr(ctx->buf, lhs_gpr, A64_TMP0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_b_cond(ctx->buf, jne, ok);
}

/** Saves d8-d15, callee-saved under AAPCS64. */
static void emit_save_fp_pool(jit_exec_buf* buf) {
  for(uint8_t d = 8; d < 16; d = (uint8_t)(d + 2u))
    emit_u32(buf, a64_stp_d_pre(d, (uint8_t)(d + 1u), A64_SP, -2));
}

static void emit_restore_fp_pool(jit_exec_buf* buf) {
  for(uint8_t d = 14; d >= 8; d = (uint8_t)(d - 2u))
    emit_u32(buf, a64_ldp_d_post(d, (uint8_t)(d + 1u), A64_SP, 2));
}

/** Emits the code at entry_off, which saves FP/LR, the chain-wide registers (OUT, VM, BASE,
 * x27) and the FP pool, then sets VM, BASE and OUT from x0-x2. `*out_body_off` follows it;
 * JIT-to-JIT calls and the enter stub start there, with those registers already set. When
 * `any_armed`, body_off saves the caller's JIT_REG_SURVIVE, which belongs to one activation, in
 * a 16-byte slot that keeps sp aligned for C helper calls. */
static void emit_prologue(jit_exec_buf* buf, size_t* out_body_off, int any_armed) {
  emit_u32(buf, a64_stp_x_pre(A64_FP, A64_LR, A64_SP, -2));
  emit_u32(buf, a64_mov_x_rr(A64_FP, A64_SP));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, -2));
  // x27 (A64_LAYOUT) holds values across C helper calls.
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, -2));
  emit_save_fp_pool(buf);
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_VM, A64_X0));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_BASE, A64_X1));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_OUT, A64_X2));
  *out_body_off = buf->len;
  if(any_armed) {
    emit_sp_adjust(buf, 0, 16);
    emit_str_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    /* Every C helper call needs sp 16-byte aligned; a misaligned one stops here, at a BRK.
     * x16 (TMP1) takes sp through ADD, as ORR would read register 31 as zero. */
    emit_u32(buf, a64_add_x_imm(A64_TMP1, A64_SP, 0));
    emit_u32(buf, 0xF2400C1Fu | ((uint32_t)A64_TMP1 << 5)); // tst x16, #0xf
    emit_u32(buf, a64_b_cond(A64_EQ, 2));
    emit_u32(buf, 0xD4200000u); // brk #0
  }
}

static void emit_restore_callee_saved(jit_exec_buf* buf, int any_armed) {
  if(any_armed) {
    emit_ldr_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_restore_fp_pool(buf);
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post(A64_FP, A64_LR, A64_SP, 2));
}

/** Writes the return value and its kind to the jello_numeric_result at JIT_REG_OUT; the kind
 * stays JELLO_NUM_VOID for a return kind not listed. */
static void emit_store_out(emit_ctx* ctx, jello_type_kind ret_kind, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                            (uint32_t)JELLO_NUM_VOID);
  if(ret_kind == JELLO_T_BOOL) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_BOOL);
    emit_store_out_as_w(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_I32);
    emit_store_out_as_w(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_I64);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_F32);
    emit_store_out_as_w(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_F64);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_BYTES || ret_kind == JELLO_T_STRUCT || ret_kind == JELLO_T_OBJECT) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_PTR);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_DYNAMIC) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                              (uint32_t)JELLO_NUM_DYN);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  }
}

/** The outermost return's exit; pops the frame, restores the callee-saved registers and
 * returns JELLO_EXEC_OK. */
static void emit_epilogue_ok_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)vm_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_OK);
  emit_ret_insn_a64(buf);
}

static void emit_reload_jit_reg_base(jit_exec_buf* buf) {
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_current_frame_mem);
  emit_mov_x_rr(buf, (unsigned)JIT_REG_BASE, A64_X0);
}

/** The shared tail of every trap exit. It branches on has_caller like emit_ret_insn; a nested
 * activation pops its frame, reloads JIT_REG_BASE and undoes its survive slot, while the outermost
 * one pops its frame and restores the callee-saved registers. Both return JELLO_EXEC_TRAP. */
static void emit_trap_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X14, 1u);
  emit_sub_w_rr(buf, A64_TMP0, A64_X14);
  emit_mov_x_imm64_reg(buf, A64_X14, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_TMP0, A64_TMP0);
  emit_mul_x_rr(buf, A64_TMP0, A64_X14);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_ldrb_w_disp(buf, A64_X14, A64_TMP0, (uint32_t)offsetof(call_frame, has_caller));
  size_t j_boundary = emit_cbz_w_placeholder(buf, A64_X14);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_reload_jit_reg_base(buf);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  if(ctx->survive_any_armed) {
    emit_ldr_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_ret_insn_a64(buf);

  size_t boundary = buf->len;
  patch_cbz_w(buf, j_boundary, boundary);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret_insn_a64(buf);
}

static void emit_epilogue_trap_generic(emit_ctx* ctx, uintptr_t trap_fn) {
  jit_exec_buf* buf = ctx->buf;
  if(trap_fn) {
    emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, trap_fn);
  }
  emit_trap_tail(ctx);
}

static void emit_epilogue_stack_overflow(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_stack_overflow);
  emit_trap_tail(ctx);
}

/** Emits RET. A nested activation (has_caller) stores the result in the caller's `caller_dst`
 * register, pops its frame, points JIT_REG_BASE at the caller's frame and returns to the BLR site.
 * The outermost one writes the result to JIT_REG_OUT and branches to the shared ok exit. */
static void emit_ret_insn(emit_ctx* ctx, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = jello_slot_size(rk) == 8u; // pointers too, not only I64/F64
  int ret_gpr = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);

  /* Copy the result to A64_TMP2 (x17), outside the pool, before the has_caller probe below
   * overwrites x14. */
  if(wide) emit_mov_x_rr(buf, A64_TMP2, (unsigned)ret_gpr);
  else emit_mov_w_rr(buf, A64_TMP2, (unsigned)ret_gpr);
  jit_reg_bind(ctx->ra, ret_vreg, A64_TMP2);
  ret_gpr = A64_TMP2;

  emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X14, 1u);
  emit_sub_w_rr(buf, A64_TMP0, A64_X14);
  emit_mov_x_imm64_reg(buf, A64_X14, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_TMP0, A64_TMP0);
  emit_mul_x_rr(buf, A64_TMP0, A64_X14);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_ldrb_w_disp(buf, A64_X14, A64_TMP0, (uint32_t)offsetof(call_frame, has_caller));
  size_t j_boundary = emit_cbz_w_placeholder(buf, A64_X14);

  // emit_mul_w_imm32 writes TMP0, so the frame address moves to x13 first.
  emit_mov_x_rr(buf, A64_X13, A64_TMP0);
  emit_ldr_w_disp(buf, A64_X14, A64_X13, (uint32_t)offsetof(call_frame, caller_dst));
  emit_mul_w_imm32(buf, A64_X14, REG_SLOT_BYTES);
  emit_uxtw_x_w(buf, A64_X14, A64_X14);

  if(wide) emit_mov_x_rr(buf, A64_X15, (unsigned)ret_gpr);
  else emit_mov_w_rr(buf, A64_X15, (unsigned)ret_gpr);

  /* A frame without pop_work or spill slots, the common case, pops inline as vm_pop_frame
   * would; the caller's frame is the call_frame before it. */
  emit_ldr_w_disp(buf, A64_X9, A64_X13, (uint32_t)offsetof(call_frame, pop_work));
  size_t j_try = emit_cbnz_w_placeholder(buf, A64_X9);
  emit_ldr_x_disp(buf, A64_X9, A64_X13, (uint32_t)offsetof(call_frame, spill_slots));
  emit_tst_x_rr(buf, A64_X9, A64_X9);
  size_t j_spill = emit_b_cond_placeholder(buf, 0x85, 0); // NE: spill slots to free
  emit_ldr_w_disp(buf, A64_X9, A64_X13,
                  (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_sub_w_rr(buf, A64_X10, A64_X9);
  emit_str_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_alu_w_imm32(buf, 0x05, A64_X10, 1u);
  emit_str_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_u32(buf, a64_sub_x_imm(A64_X13, A64_X13, (uint32_t)sizeof(call_frame)));
  emit_ldr_x_disp(buf, (unsigned)JIT_REG_BASE, A64_X13,
                  (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));
  size_t j_popped = emit_b_placeholder(buf);
  patch_cbnz_w(buf, j_try, buf->len);
  patch_b_cond(buf, j_spill, buf->len);

  emit_sp_adjust(buf, 0, 16);
  emit_str_x_sp(buf, A64_X14, 0);
  emit_str_x_sp(buf, A64_X15, 8);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)vm_pop_frame);
  emit_reload_jit_reg_base(buf);

  emit_ldr_x_sp(buf, A64_X14, 0);
  emit_ldr_x_sp(buf, A64_X15, 8);
  emit_sp_adjust(buf, 1, 16);
  patch_b(buf, j_popped, buf->len);
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)JIT_REG_BASE);
  emit_add_x_rr(buf, A64_TMP1, A64_X14);
  if(wide) emit_str_x_disp(buf, A64_X15, A64_TMP1, 0);
  else emit_str_w_disp(buf, A64_X15, A64_TMP1, 0);

  /* A nested activation entered at body_off, so the registers the prologue saved belong to the
   * outermost activation and stay on the stack. Its own survive slot is undone, or the caller's
   * LR reload after its BLR would read the wrong slot. */
  if(ctx->survive_any_armed) {
    emit_ldr_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_ret_insn_a64(buf);

  size_t boundary = buf->len;
  patch_cbz_w(buf, j_boundary, boundary);

  emit_store_out(ctx, rk, ret_vreg);
  size_t at = emit_b_placeholder(buf);
  push_ret_jump(ctx, at);
}

/** Fills call_frames[call_frames_len] for a callee frame whose registers are at x15, as
 * push_frame_shell fills it, and stores call_frames_len + 1. A known callee `f` comes with its
 * index `fi`, `has_pointer` and `total`; for a NULL one they are read from the frame_layout in
 * x27, and its index from w14. Clobbers x9 to x12. */
static void emit_fill_call_frame(jit_exec_buf* buf, const jello_bc_function* f, uint32_t fi,
                                 int has_pointer, uint32_t total, uint32_t caller_dst) {
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames));
  emit_uxtw_x_w(buf, A64_X12, A64_X10);
  emit_mov_x_imm64_reg(buf, A64_X11, sizeof(call_frame));
  emit_mul_x_rr(buf, A64_X12, A64_X11);
  emit_add_x_rr(buf, A64_X9, A64_X12);

  if(f) emit_mov_x_imm64_reg(buf, A64_X11, (uint64_t)(uintptr_t)f);
  else emit_ldr_x_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, f));
  emit_str_x_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, f));
  unsigned fi_reg = A64_X14;
  if(f) {
    emit_mov_w_imm32(buf, A64_X11, fi);
    fi_reg = A64_X11;
  }
  emit_str_w_disp(buf, fi_reg, A64_X9, (uint32_t)offsetof(call_frame, func_index));
  emit_str_w_disp(buf, 31u, A64_X9, (uint32_t)offsetof(call_frame, pc)); // wzr
  _Static_assert(sizeof(((call_frame*)0)->has_caller) == 1 &&
                     sizeof(((call_frame*)0)->has_pointer) == 1,
                 "the frame's flags are stored as bytes");
  emit_mov_w_imm32(buf, A64_X11, 1u);
  emit_strb_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, has_caller));
  if(f) emit_mov_w_imm32(buf, A64_X11, (uint32_t)has_pointer);
  else emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, has_pointer));
  emit_strb_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, has_pointer));
  emit_mov_w_imm32(buf, A64_X11, caller_dst);
  emit_str_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, caller_dst));

  emit_ldr_x_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, kind));
  emit_str_x_disp(buf, A64_X11, A64_X9,
                  (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, kind));
  emit_ldr_x_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, wide));
  emit_str_x_disp(buf, A64_X11, A64_X9,
                  (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, wide));
  if(f) emit_mov_w_imm32(buf, A64_X11, total);
  else emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
  emit_str_w_disp(buf, A64_X11, A64_X9,
                  (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_str_x_disp(buf, A64_X15, A64_X9,
                  (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));

  emit_alu_w_imm32(buf, 0x00, A64_X10, 1u);
  emit_str_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
}

/** Moves JIT_REG_BASE to the callee frame at x15 and copies `nargs` arguments into it from the
 * caller's registers from `arg_first`, at the widths of `kinds_f`'s registers from
 * `kinds_first`. Clobbers x9 and x12. */
static void emit_copy_args(emit_ctx* ctx, const jello_bc_function* kinds_f, uint32_t kinds_first,
                           uint32_t arg_first, uint32_t nargs) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_x_rr(buf, A64_X12, (unsigned)JIT_REG_BASE);
  emit_mov_x_rr(buf, (unsigned)JIT_REG_BASE, A64_X15);
  for(uint32_t ai = 0; ai < nargs; ai++) {
    uint32_t src_disp = (arg_first + ai) * REG_SLOT_BYTES;
    uint32_t dst_disp = ai * REG_SLOT_BYTES;
    if(jello_slot_size(vm_reg_kind(ctx->m, kinds_f, kinds_first + ai)) == 8u) {
      emit_ldr_x_disp(buf, A64_X9, A64_X12, src_disp);
      emit_str_x_base(buf, A64_X9, dst_disp);
    } else {
      emit_ldr_w_disp(buf, A64_X9, A64_X12, src_disp);
      emit_str_w_base(buf, A64_X9, dst_disp);
    }
  }
}

/** Calls JIT code at `target`, a register. BLR overwrites LR, so LR is kept in a 16-byte stack
 * slot around it, as emit_call_fn does; the slot keeps sp 16-byte aligned. */
static void emit_call_jit_code(jit_exec_buf* buf, unsigned target) {
  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_blr_x((uint8_t)target));
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
}

/** After a JIT call; a callee's trap returns with vm->trap_code set, which passes through rather
 * than continuing; otherwise the result is reloaded from `dst`'s home. */
static void emit_after_jit_call(emit_ctx* ctx, uint8_t dst) {
  emit_ldr_w_vm(ctx->buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_tst_w_rr(ctx->buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_CALLEE);
  jit_reg_state_discard_currents(ctx->ra);
  (void)jit_reg_get_gpr(ctx->ra, dst, 1);
}

/** Loads frame_stack_top into x11, and it plus `total`, a frame's bytes, into x12. */
static void emit_frame_tops(jit_exec_buf* buf, uint32_t total) {
  emit_ldr_w_vm(buf, A64_X11, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_x_rr(buf, A64_X12, A64_X11);
  emit_alu_w_imm32(buf, 0x00, A64_X12, total);
}

/** Emits CALL_SELF or CALL_DIRECT; pushes the callee's frame inline, as vm_push_frame does,
 * copies the arguments and calls through the entry table. A missing layout or a frame that does
 * not fit traps as stack overflow. Returns 0, declining the function, without an entry table. */
static int emit_call_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins, jello_jit_ir_op op) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t callee_fi = (op == JIR_CALL_SELF) ? ctx->fi : ins->imm;
  uint32_t caller_dst = ins->a;
  uint32_t arg_first = ins->b;
  uint32_t nargs = ins->c;
  const jello_bc_function* callee_f = &ctx->m->funcs[callee_fi];
  if(!ctx->entries) return 0;

  jit_reg_state_discard_currents(ctx->ra);

  /* The callee's frame_layout, from vm->jit_layout_by_func, stays in x27 (A64_LAYOUT),
   * callee-saved, across the C calls below. */
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
  emit_cmp_w_imm32(buf, A64_X10, callee_fi);
  size_t j_indexed = emit_b_cond_placeholder(buf, 0x87, 0); // HI: callee_fi < len
  emit_jmp_stack_overflow(ctx);
  patch_b_cond(buf, j_indexed, buf->len);
  emit_ldr_x_vm(buf, A64_LAYOUT, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
  uint32_t layout_disp = callee_fi * (uint32_t)sizeof(frame_layout*);
  unsigned layout_base = emit_disp_base(buf, A64_LAYOUT, A64_LAYOUT, A64_TMP0, &layout_disp, 8);
  emit_ldr_x_disp(buf, A64_LAYOUT, layout_base, layout_disp);
  emit_tst_x_rr(buf, A64_LAYOUT, A64_LAYOUT);
  size_t j_no_layout = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  patch_b_cond(buf, j_no_layout, buf->len);

  /* The frame's size and whether it holds a reference depend only on the callee. */
  uint32_t total = callee_f->nregs * REG_SLOT_BYTES;
  int has_pointer = vm_func_has_pointer(ctx->m, callee_f);

  /* Room on the frame stack and a free call_frame slot, as push_frame_shell makes them; when
   * either must grow, a C call grows them and the tops are loaded again. */
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_limit));
  emit_cmp_w_rr(buf, A64_X10, A64_TMP0);
  size_t j_frames = emit_b_cond_placeholder(buf, 0x82, 0); // LO: below the frame limit
  emit_jmp_stack_overflow(ctx);
  patch_b_cond(buf, j_frames, buf->len);
  emit_frame_tops(buf, total);
  emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, frame_stack_limit));
  emit_cmp_w_rr(buf, A64_X12, A64_TMP0);
  size_t j_grow = emit_b_cond_placeholder(buf, 0x87, 0); // HI: past what is committed
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_cap));
  emit_cmp_w_rr(buf, A64_X10, A64_X9);
  size_t j_room = emit_b_cond_placeholder(buf, 0x82, 0); // LO: a free slot
  patch_b_cond(buf, j_grow, buf->len);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_X12);
  emit_call_fn(buf, (uintptr_t)jit_try_grow_push);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  size_t j_grown = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  patch_b_cond(buf, j_grown, buf->len);
  emit_frame_tops(buf, total);
  patch_b_cond(buf, j_room, buf->len);

  // The frame stack never moves; x15 = the new frame's registers.
  emit_str_w_vm(buf, A64_X12, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_uxtw_x_w(buf, A64_X15, A64_X11);
  emit_add_x_rr(buf, A64_X15, A64_X9);

  /* As vm_push_frame does, a frame with a reference register starts as the layout's init
   * image; memcpy returns the frame's registers, as the call clobbers x15. */
  if(has_pointer) {
    emit_mov_x_rr(buf, A64_X0, A64_X15);
    emit_ldr_x_disp(buf, A64_X1, A64_LAYOUT, (uint32_t)offsetof(frame_layout, init));
    emit_mov_w_imm32(buf, A64_X2, total);
    emit_call_fn(buf, (uintptr_t)memcpy);
    emit_mov_x_rr(buf, A64_X15, A64_X0);
  }

  emit_fill_call_frame(buf, callee_f, callee_fi, has_pointer, total, caller_dst);
  emit_copy_args(ctx, callee_f, 0, arg_first, nargs);

  // The callee's current target: its body, the bridge, or the lazy stub that compiles it.
  emit_mov_x_imm64_reg(buf, A64_IP0, (uint64_t)(uintptr_t)&ctx->entries[callee_fi]);
  emit_u32(buf, a64_ldr_x_uimm(A64_IP0, A64_IP0, 0));
  emit_bridge_when_deep(ctx, A64_IP0);
  emit_call_jit_code(buf, A64_IP0);
  emit_after_jit_call(ctx, ins->a);
  return 1;
}

/** Branches, through placeholders appended to `slow`, unless `reg` holds a non-NULL pointer to
 * an object of header kind `kind`. Clobbers x9. */
static void emit_obj_kind_guard(jit_exec_buf* buf, unsigned reg, uint32_t kind, size_t* slow,
                                size_t* nslow) {
  emit_mov_w_imm32(buf, A64_X9, 7u);
  emit_tst_x_rr(buf, reg, A64_X9);
  slow[(*nslow)++] = emit_b_cond_placeholder(buf, 0x85, 0); // NE: not a pointer
  emit_tst_x_rr(buf, reg, reg);
  slow[(*nslow)++] = emit_b_cond_placeholder(buf, 0x84, 0); // EQ: NULL
  emit_ldrb_w_disp(buf, A64_X9, reg, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp_w_imm32(buf, A64_X9, kind);
  slow[(*nslow)++] = emit_b_cond_placeholder(buf, 0x85, 0);
}

/** CALLR. The common case is inline: a Function value whose frame has a layout and fits is
 * pushed as emit_call_insn pushes one, from the layout and the Function read at run time, and its
 * captures, always Dynamic and so 8 bytes each, follow its arguments. Anything else (null, not a
 * Function, no layout, a full call-frame array or frame stack) takes jit_callr_push, which pushes
 * or traps as op_callr does, located at this CALLR. Either way the callee's entry (its body, the
 * bridge or the lazy stub) ends in x27; a trap inside the callee passes through. As for
 * CALL, the layout index is warmed for the running module before any JIT code runs. */
static int emit_callr_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
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
    // x13 = the Function, x14 = its function index, x27 = its frame_layout.
    emit_ldr_x_disp(buf, A64_X13, (unsigned)JIT_REG_BASE, callee_disp);
    emit_obj_kind_guard(buf, A64_X13, (uint32_t)JELLO_OBJ_FUNCTION, slow, &nslow);
    emit_ldr_w_disp(buf, A64_X14, A64_X13, (uint32_t)offsetof(jello_function, func_index));
    emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
    emit_cmp_w_rr(buf, A64_X10, A64_X14);
    slow[nslow++] = emit_b_cond_placeholder(buf, 0x86, 0); // LS: no layout index
    emit_ldr_x_vm(buf, A64_LAYOUT, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
    emit_uxtw_x_w(buf, A64_X9, A64_X14);
    emit_lsl_x_imm3(buf, A64_X9, A64_X9);
    emit_add_x_rr(buf, A64_LAYOUT, A64_X9);
    emit_ldr_x_disp(buf, A64_LAYOUT, A64_LAYOUT, 0);
    emit_tst_x_rr(buf, A64_LAYOUT, A64_LAYOUT);
    slow[nslow++] = emit_b_cond_placeholder(buf, 0x84, 0);
    emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
    emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_limit));
    emit_cmp_w_rr(buf, A64_X10, A64_TMP0);
    slow[nslow++] = emit_b_cond_placeholder(buf, 0x83, 0); // HS
    emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_cap));
    emit_cmp_w_rr(buf, A64_X10, A64_X9);
    slow[nslow++] = emit_b_cond_placeholder(buf, 0x83, 0);
    emit_ldr_w_vm(buf, A64_X11, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_ldr_w_disp(buf, A64_X12, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
    emit_u32(buf, a64_add_w_rr(A64_X12, A64_X12, A64_X11));
    emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, frame_stack_limit));
    emit_cmp_w_rr(buf, A64_X12, A64_TMP0);
    slow[nslow++] = emit_b_cond_placeholder(buf, 0x87, 0); // HI: does not fit

    // The push cannot fail from here; x15 = the new frame's registers.
    emit_str_w_vm(buf, A64_X12, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack));
    emit_uxtw_x_w(buf, A64_X15, A64_X11);
    emit_add_x_rr(buf, A64_X15, A64_X9);
    emit_ldr_w_disp(buf, A64_X9, A64_LAYOUT, (uint32_t)offsetof(frame_layout, has_pointer));
    emit_tst_w_rr(buf, A64_X9, A64_X9);
    size_t j_no_ptr = emit_b_cond_placeholder(buf, 0x84, 0);
    emit_mov_x_rr(buf, A64_X0, A64_X15);
    emit_ldr_x_disp(buf, A64_X1, A64_LAYOUT, (uint32_t)offsetof(frame_layout, init));
    emit_ldr_w_disp(buf, A64_X2, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
    emit_call_fn(buf, (uintptr_t)memcpy);
    emit_mov_x_rr(buf, A64_X15, A64_X0);
    emit_ldr_x_disp(buf, A64_X13, (unsigned)JIT_REG_BASE, callee_disp);
    emit_ldr_w_disp(buf, A64_X14, A64_X13, (uint32_t)offsetof(jello_function, func_index));
    patch_b_cond(buf, j_no_ptr, buf->len);

    emit_fill_call_frame(buf, NULL, 0, 0, 0, caller_dst);

    // The captures, from x12 into the registers from cap_start at x11, x10 at a time.
    emit_ldr_w_disp(buf, A64_X10, A64_X13, (uint32_t)offsetof(jello_function, ncaps));
    emit_tst_w_rr(buf, A64_X10, A64_X10);
    size_t j_no_caps = emit_b_cond_placeholder(buf, 0x84, 0);
    emit_ldr_x_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, f));
    emit_ldr_w_disp(buf, A64_X11, A64_X11, (uint32_t)offsetof(jello_bc_function, cap_start));
    emit_uxtw_x_w(buf, A64_X11, A64_X11);
    emit_lsl_x_imm3(buf, A64_X11, A64_X11);
    emit_add_x_rr(buf, A64_X11, A64_X15);
    emit_u32(buf, a64_add_x_imm(A64_X12, A64_X13, (uint32_t)offsetof(jello_function, captures)));
    size_t loop = buf->len;
    emit_u32(buf, a64_ldr_x_uimm(A64_X9, A64_X12, 0));
    emit_u32(buf, a64_add_x_imm(A64_X12, A64_X12, 8));
    emit_u32(buf, a64_str_x_uimm(A64_X9, A64_X11, 0));
    emit_u32(buf, a64_add_x_imm(A64_X11, A64_X11, 8));
    emit_alu_w_imm32(buf, 0x05, A64_X10, 1u);
    emit_tst_w_rr(buf, A64_X10, A64_X10);
    emit_u32(buf, a64_b_cond(A64_NE, off_insns_at(buf->len, loop)));
    patch_b_cond(buf, j_no_caps, buf->len);

    // The arguments, at the caller's widths, which validation makes the callee's.
    emit_copy_args(ctx, caller_f, arg_first, arg_first, nargs);
    emit_mov_x_imm64_reg(buf, A64_X9, (uint64_t)(uintptr_t)ctx->entries);
    emit_uxtw_x_w(buf, A64_X14, A64_X14);
    emit_lsl_x_imm3(buf, A64_X14, A64_X14);
    emit_add_x_rr(buf, A64_X9, A64_X14);
    emit_ldr_x_disp(buf, A64_LAYOUT, A64_X9, 0);
    j_call = emit_b_placeholder(buf);
  }

  for(size_t i = 0; i < nslow; i++) patch_b_cond(buf, slow[i], buf->len);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_imm32(buf, A64_X1, caller_dst);
  emit_mov_w_imm32(buf, A64_X2, ins->b);
  emit_mov_w_imm32(buf, A64_X3, arg_first);
  emit_mov_w_imm32(buf, A64_X4, nargs);
  emit_call_fn(buf, (uintptr_t)jit_callr_push);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X0);
  emit_tst_x_rr(buf, A64_LAYOUT, A64_LAYOUT);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // EQ: refused
  emit_reload_jit_reg_base(buf);
  if(ctx->entries) {
    patch_b(buf, j_call, buf->len);
    emit_bridge_when_deep(ctx, A64_LAYOUT);
  }
  emit_call_jit_code(buf, A64_LAYOUT);
  emit_after_jit_call(ctx, ins->a);
  return 1;
}

/** TRY pushes a scope on the frame (vm_try_push, which traps when there is no memory for it);
 * the handler pc is absolute. */
static void emit_try_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_imm32(buf, A64_X1, (uint32_t)((int32_t)ins->bc_pc + 1 + (int32_t)ins->imm));
  emit_mov_w_imm32(buf, A64_X2, ins->a);
  emit_call_fn(buf, (uintptr_t)vm_try_push);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // EQ: no memory
}

static void emit_endtry_insn(emit_ctx* ctx) {
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(ctx->buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(ctx->buf, (uintptr_t)vm_try_pop);
}

/** THROW leaves either the unwind sentinel or an uncaught-throw trap; both leave through the
 * passthrough exit, whose dispatch catches the former when the handler is in this activation. */
static void emit_throw_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_imm32(buf, A64_X1, ins->a);
  emit_mov_w_imm32(buf, A64_X2, ctx->fi);
  emit_mov_w_imm32(buf, A64_X3, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_throw);
  size_t at = emit_b_placeholder(buf);
  push_trap_jump(ctx, at, JIT_TRAP_PASSTHROUGH);
}

/** In a function with handlers, every passthrough exit (a callee's trap or unwind, THROW, a
 * handler-run op) first checks for a throw unwinding to this activation; the sentinel with
 * call_frames_len == unwind_len. A recursive activation without the scope sees a different
 * length and keeps unwinding. vm_throw_find already set the frame's pc to the handler; the code
 * dispatches on it. Registers are all in their homes. */
static void emit_catch_dispatch(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_cmp_w_imm32(buf, A64_X9, (uint32_t)JELLO_TRAP_INTERNAL_UNWIND);
  size_t not_unwind = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, unwind_len));
  emit_cmp_w_rr(buf, A64_X9, A64_X10);
  size_t not_here = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_store_imm32_to_vm_field(buf, (uint32_t)offsetof(jello_vm, trap_code),
                               (uint32_t)JELLO_TRAP_NONE);
  emit_store_imm32_to_vm_field(buf, JIT_TRAP_SITE_OFF, 0u);
  emit_reload_jit_reg_base(buf);
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X11, 1u);
  emit_sub_w_rr(buf, A64_X9, A64_X11);
  emit_mov_x_imm64_reg(buf, A64_X11, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_X15, A64_X9);
  emit_mul_x_rr(buf, A64_X15, A64_X11);
  emit_add_x_rr(buf, A64_X15, A64_X10);
  emit_ldr_w_disp(buf, A64_X9, A64_X15, (uint32_t)offsetof(call_frame, pc));
  for(uint32_t i = 0; i < ctx->ncatch; i++) {
    emit_cmp_w_imm32(buf, A64_X9, ctx->catch_pcs[i]);
    size_t at = emit_b_cond_placeholder(buf, 0x84, 0);
    push_branch(ctx, at, ctx->catch_pcs[i], 1);
  }
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_unwind_lost);
  patch_b_cond(buf, not_unwind, buf->len);
  patch_b_cond(buf, not_here, buf->len);
  emit_trap_tail(ctx);
}

/** Emits an F16 binary op (`sse_op` as for emit_fbin_s; anything else divides) by converting
 * both operands, read from their homes, to f32 with vm_f16_to_f32 and the result back with
 * vm_f32_to_f16. The first operand waits in a 16-byte stack slot across the second call. It works
 * on physical s9 and s0, not pool indices, so it calls the a64_f*_s encoders directly. */
static void emit_f16_binop(emit_ctx* ctx, const jello_jit_ir_insn* ins, uint8_t sse_op) {
  jit_exec_buf* buf = ctx->buf;
  emit_sp_adjust(buf, 0, 16);
  emit_load_u32_slot_to_w1(buf, ins->b);
  emit_mov_w_rr(buf, A64_X0, A64_X1);
  emit_call_fn(buf, (uintptr_t)vm_f16_to_f32);
  emit_u32(buf, a64_str_s_uimm(0, A64_SP, 0)); // str s0, [sp]
  emit_load_u32_slot_to_w1(buf, ins->c);
  emit_mov_w_rr(buf, A64_X0, A64_X1);
  emit_call_fn(buf, (uintptr_t)vm_f16_to_f32); // s0 = second operand
  emit_u32(buf, a64_ldr_s_uimm(9, A64_SP, 0)); // ldr s9, [sp]
  if(sse_op == 0x58) emit_u32(buf, a64_fadd_s(9, 9, 0));
  else if(sse_op == 0x5C) emit_u32(buf, a64_fsub_s(9, 9, 0));
  else if(sse_op == 0x59) emit_u32(buf, a64_fmul_s(9, 9, 0));
  else emit_u32(buf, a64_fdiv_s(9, 9, 0));
  emit_u32(buf, a64_fmov_s(0, 9));
  emit_call_fn(buf, (uintptr_t)vm_f32_to_f16);
  emit_sp_adjust(buf, 1, 16);
  jit_reg_state_clobber_call(ctx->ra);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bin_f32(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 0);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_fmov_s(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_fbin_s(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_fbin_s(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_fbin_s(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_fbin_s(ctx->buf, 0x5E, di, ri); break;
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
  emit_fmov_d(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_fbin_d(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_fbin_d(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_fbin_d(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_fbin_d(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
}

static void emit_cmp_float(emit_ctx* ctx, const jello_jit_ir_insn* ins, int is_f64) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, is_f64);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, is_f64);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  if(is_f64) emit_fcmp_d(ctx->buf, li, ri);
  else emit_fcmp_s(ctx->buf, li, ri);
  int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  /* An unordered (NaN) FCMP sets NZCV to 0011, so EQ and MI (N set) are both false then, as
   * C's == and < are. */
  a64_cond cond = (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? A64_EQ : A64_MI;
  emit_u32(ctx->buf, a64_cset_w((uint8_t)d, cond));
  jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
}

/** Loads the double `x` into FP register `dd` through TMP1. */
static void emit_load_f64(jit_exec_buf* buf, uint8_t dd, double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof bits);
  emit_mov_x_imm64(buf, A64_TMP1, bits);
  emit_u32(buf, a64_fmov_d_x(dd, A64_TMP1));
}

/** Traps ARITHMETIC unless FP register `v` (an F32 when `!is_f64`) converts to I64 when
 * `to_i64`, else I32; it must lie strictly between that type's VM_*_FROM_FLOAT bounds, so NaN
 * never does. FCMP sets C for ">=" and for unordered, so HS traps both. Writes TMP1, d16 and
 * d17. */
static void emit_float_conv_check(emit_ctx* ctx, uint8_t v, int is_f64, int to_i64) {
  jit_exec_buf* buf = ctx->buf;
  if(!is_f64) {
    emit_u32(buf, a64_fcvt_d_s(A64_D_TMP0, v));
    v = A64_D_TMP0;
  }
  emit_load_f64(buf, A64_D_TMP1, to_i64 ? VM_I64_FROM_FLOAT_LO : VM_I32_FROM_FLOAT_LO);
  emit_u32(buf, a64_fcmp_d(A64_D_TMP1, v));
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_ARITH); // HS: lo >= v, or unordered
  emit_load_f64(buf, A64_D_TMP1, to_i64 ? VM_I64_FROM_FLOAT_HI : VM_I32_FROM_FLOAT_HI);
  emit_u32(buf, a64_fcmp_d(v, A64_D_TMP1));
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_ARITH); // HS: v >= hi, or unordered
}

static int emit_conv(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jir_conv)ins->imm) {
    case JIR_CONV_TRUNC_I8: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_TRUNC_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_sxth_w(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_I32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_sxtw_x_w(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_scvtf_s(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_s(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_scvtf_d(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 1, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_d(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_fcvt_d_s(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0),
                    (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_fcvt_s_d(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0),
                    (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F64_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_scvtf_d(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 1, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_d(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_scvtf_s(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 0, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_s(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_x_rr(ctx->buf, A64_X0, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)vm_f16_to_f32);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_fmov_s_from_s0(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_fmov_s_to_s0(ctx->buf, (unsigned)(sx - (int)JIT_PREG_XMM0));
      emit_call_fn(ctx->buf, (uintptr_t)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, A64_X0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_scvtf_s_to_s0(ctx->buf, (unsigned)s, 0);
      emit_call_fn(ctx->buf, (uintptr_t)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, A64_X0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_I32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_x_rr(ctx->buf, A64_X0, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)vm_f16_to_f32);
      emit_float_conv_check(ctx, 0, 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_from_s0(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    default: return 0;
  }
}

/* The Bytes ops use A64_TMP0-TMP2 (x15-x17) as scratch without telling the allocator, which never
 * hands them out. Before a C call they run jit_reg_state_discard_currents, since the call clobbers
 * the caller-saved pool registers. */

/** Traps NULL when reference register `gpr` holds null. */
static void emit_null_check(emit_ctx* ctx, unsigned gpr) {
  emit_tst_x_rr(ctx->buf, gpr, gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
}

/** Traps NULL when the Bytes in `gpr` is null, and TYPE when it is no buffer, as only a buffer
 * may be written. Clobbers x16. */
static void emit_buffer_check(emit_ctx* ctx, unsigned gpr) {
  emit_null_check(ctx, gpr);
  emit_ldr_w_disp(ctx->buf, A64_TMP1, gpr, (uint32_t)offsetof(jello_bytes, h.type_id));
  emit_cmp_w_imm32(ctx->buf, A64_TMP1, JELLO_BYTES_BUFFER);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_TYPE); // NE: no buffer
}

/** Traps BOUNDS unless `idx`, read as unsigned, is below the 32-bit length at `len_off` from
 * `ptr`. */
static void emit_bounds_trap(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off) {
  emit_ldr_w_disp(ctx->buf, A64_TMP0, ptr, len_off);
  emit_cmp_w_rr(ctx->buf, idx, A64_TMP0);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); // HS: idx >= length, unsigned
}

/** Leaves in A64_TMP1 the address of element `idx` of the Bytes or Array in register `ptr`,
 * whose length and data pointer are at `len_off` and `data_off`, its elements 1 << `shift`
 * bytes; traps BOUNDS when `idx`, read as unsigned, is not below the length. */
static void emit_indexed_addr(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off,
                              uint32_t data_off, uint8_t shift) {
  jit_exec_buf* buf = ctx->buf;
  emit_bounds_trap(ctx, ptr, idx, len_off);
  emit_ldr_x_disp(buf, A64_TMP1, ptr, data_off);
  emit_u32(buf, a64_add_x_uxtw(A64_TMP1, A64_TMP1, (uint8_t)idx, shift));
}

/** Compares the tag of the Dynamic in `src` with `tag`, through x16. */
static void emit_tag_cmp(jit_exec_buf* buf, unsigned src, uint32_t tag) {
  emit_u32(buf, a64_and_w_imm7(A64_TMP1, (uint8_t)src));
  emit_cmp_w_imm32(buf, A64_TMP1, tag);
}

/** Sets Z exactly when register `r` holds null; a Dynamic's tag is the null tag (jello_is_null),
 * a reference is the null pointer. */
static void emit_null_flags(emit_ctx* ctx, uint8_t r) {
  int s = jit_reg_get_gpr(ctx->ra, r, 1);
  if(vm_reg_kind(ctx->m, ctx->f, r) == JELLO_T_DYNAMIC) {
    emit_tag_cmp(ctx->buf, (unsigned)s, (uint32_t)JELLO_TAG_NULL);
  } else {
    emit_tst_x_rr(ctx->buf, (unsigned)s, (unsigned)s);
  }
}

static void emit_bytes_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_ldr_w_disp(buf, (unsigned)dst, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
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
  emit_ldrb_w_disp(buf, (unsigned)dst, A64_TMP1, 0);
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
  emit_cmp_w_imm32(buf, (unsigned)val, 255u);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // HI: val > 255
  emit_strb_w_disp(buf, (unsigned)val, A64_TMP1, 0);
}

static void emit_bytes_eq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);

  emit_cmp_x_rr(buf, (unsigned)b, (unsigned)c);
  size_t j_same = emit_b_cond_placeholder(buf, 0x84, 0); // EQ: same object, true

  emit_tst_x_rr(buf, (unsigned)b, (unsigned)b);
  size_t j_b_null = emit_b_cond_placeholder(buf, 0x84, 0); // null: false
  emit_tst_x_rr(buf, (unsigned)c, (unsigned)c);
  size_t j_c_null = emit_b_cond_placeholder(buf, 0x84, 0); // null: false

  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  emit_ldr_w_disp(buf, A64_TMP1, (unsigned)c, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_w_rr(buf, A64_TMP0, A64_TMP1);
  size_t j_len_ne = emit_b_cond_placeholder(buf, 0x85, 0); // NE: lengths differ, false

  emit_ldr_x_disp(buf, A64_TMP0, (unsigned)b, (uint32_t)offsetof(jello_bytes, data));
  emit_ldr_x_disp(buf, A64_TMP1, (unsigned)c, (uint32_t)offsetof(jello_bytes, data));
  emit_ldr_w_disp(buf, A64_TMP2, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_x_rr(buf, A64_X1, A64_TMP1);
  emit_mov_x_rr(buf, A64_X2, A64_TMP2);
  emit_call_fn(buf, (uintptr_t)memcmp);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_setcc_from_flags(buf, 0x94, (unsigned)dst); // dst = (memcmp == 0)
  emit_mov_w_rr(buf, (unsigned)dst, (unsigned)dst);
  size_t j_done1 = emit_b_placeholder(buf);

  size_t false_lbl = buf->len;
  patch_b_cond(buf, j_b_null, false_lbl);
  patch_b_cond(buf, j_c_null, false_lbl);
  patch_b_cond(buf, j_len_ne, false_lbl);
  emit_mov_w_imm32(buf, (unsigned)dst, 0u);
  size_t j_done2 = emit_b_placeholder(buf);

  size_t true_lbl = buf->len;
  patch_b_cond(buf, j_same, true_lbl);
  emit_mov_w_imm32(buf, (unsigned)dst, 1u);

  size_t done_lbl = buf->len;
  patch_b(buf, j_done1, done_lbl);
  patch_b(buf, j_done2, done_lbl);

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

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)b);
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_TMP0);
  emit_mov_x_rr(buf, A64_X2, A64_TMP1);
  emit_mov_w_imm32(buf, A64_X3, (uint32_t)op_kind);
  emit_call_fn(buf, (uintptr_t)jit_bytes_bitop);
  emit_tst_x_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // NULL: the helper set trap_code
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
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

/** Traps BOUNDS unless `off` + `width` <= length, summed in 64 bits so that a large offset cannot
 * wrap back into range, as in bytes_access_in_range. */
static void emit_bytes_bounds_check(emit_ctx* ctx, int bytes_gpr, int off_gpr, uint32_t width) {
  jit_exec_buf* buf = ctx->buf;
  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)bytes_gpr, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off_gpr);
  emit_alu_x_imm32(buf, 0x00, A64_TMP1, width);
  emit_cmp_x_rr(buf, A64_TMP1, A64_TMP0);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // HI: off + width > length
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

  emit_ldr_x_disp(buf, A64_TMP0, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);

  if(!is_float) {
    int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
    if(width == 2) emit_ldrh_w_disp(buf, (unsigned)dst, A64_TMP0, 0);
    else if(width == 4) emit_ldr_w_disp(buf, (unsigned)dst, A64_TMP0, 0);
    else emit_ldr_x_disp(buf, (unsigned)dst, A64_TMP0, 0);
    if(be) {
      if(width == 8) emit_rev_x(buf, (unsigned)dst);
      else if(width == 4) emit_rev_w(buf, (unsigned)dst);
      else emit_rev16_w(buf, (unsigned)dst, 8);
    }
    if(width == 8) jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
    else jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
  } else {
    if(width == 4) emit_ldr_w_disp(buf, A64_TMP1, A64_TMP0, 0);
    else emit_ldr_x_disp(buf, A64_TMP1, A64_TMP0, 0);
    if(be) {
      if(width == 8) emit_rev_x(buf, A64_TMP1);
      else emit_rev_w(buf, A64_TMP1);
    }
    int dst = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, width == 8);
    if(width == 4) {
      emit_fmov_s_w(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), A64_TMP1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dst);
    } else {
      emit_fmov_d_x(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), A64_TMP1);
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
    emit_mov_x_rr(buf, A64_TMP2, (unsigned)val);
  } else {
    int val = jit_reg_get_xmm(ctx->ra, ins->a, 1, width == 8);
    if(width == 4) emit_fmov_w_s(buf, A64_TMP2, (unsigned)(val - (int)JIT_PREG_XMM0));
    else emit_fmov_x_d(buf, A64_TMP2, (unsigned)(val - (int)JIT_PREG_XMM0));
  }
  if(be) {
    if(width == 8) emit_rev_x(buf, A64_TMP2);
    else if(width == 4) emit_rev_w(buf, A64_TMP2);
    else emit_rev16_w(buf, A64_TMP2, 8);
  }

  emit_ldr_x_disp(buf, A64_TMP0, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);

  if(width == 2) emit_strh_w_disp(buf, A64_TMP2, A64_TMP0, 0);
  else if(width == 4) emit_str_w_disp(buf, A64_TMP2, A64_TMP0, 0);
  else emit_str_x_disp(buf, A64_TMP2, A64_TMP0, 0);
}

/** Copies with memmove; `a` and `b` may name the same Bytes with overlapping ranges. */
static void emit_bytes_blit(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int dst_bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)dst_bp);
  int src_bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)src_bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);

  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP1, A64_TMP0);
  emit_ldr_w_disp(buf, A64_TMP2, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_x_rr(buf, A64_TMP1, A64_TMP2);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // HI: off + src->length > dst->length

  emit_ldr_x_disp(buf, A64_TMP0, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_ldr_x_disp(buf, A64_TMP1, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_ldr_w_disp(buf, A64_TMP2, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_x_rr(buf, A64_X1, A64_TMP1);
  emit_mov_x_rr(buf, A64_X2, A64_TMP2);
  emit_call_fn(buf, (uintptr_t)memmove);
}

static void emit_bits_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int val = jit_reg_get_gpr(ctx->ra, ins->b, 1);

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)bp);
  emit_ldr_w_disp(buf, A64_TMP1, A64_TMP0, (uint32_t)offsetof(jello_bytes, length));
  emit_lsl_x_imm3(buf, A64_TMP1, A64_TMP1);
  emit_mov_x_imm64_reg(buf, A64_IP1, end_bit);
  emit_cmp_x_rr(buf, A64_IP1, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // HI: end_bit > length * 8

  emit_mov_w_rr(buf, A64_TMP2, (unsigned)val);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_w_imm32(buf, A64_X1, width);
  emit_mov_w_imm32(buf, A64_X2, bit_off);
  emit_mov_w_rr(buf, A64_X3, A64_TMP2);
  emit_call_fn(buf, (uintptr_t)jit_bits_write);
}

/** Clears the buffer mark of the Bytes in `a`, as op_bytes_freeze does; any other Bytes may be
 * shared, so it is not written. */
static void emit_bytes_freeze(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  const uint32_t mark = (uint32_t)offsetof(jello_bytes, h.type_id);
  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_null_check(ctx, (unsigned)bp);
  emit_ldr_w_disp(buf, A64_TMP1, (unsigned)bp, mark);
  emit_cmp_w_imm32(buf, A64_TMP1, JELLO_BYTES_BUFFER);
  size_t shared = emit_b_cond_placeholder(buf, 0x85, 0); // NE: no buffer
  emit_mov_w_imm32(buf, A64_TMP1, 0);
  emit_str_w_disp(buf, A64_TMP1, (unsigned)bp, mark);
  patch_b_cond(buf, shared, buf->len);
}

static void emit_bytes_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int len = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  // Staged outside the pool; before the discard, a pool register may hold another live value.
  emit_mov_w_rr(buf, A64_TMP0, (unsigned)len);
  /* The guards of op_bytes_new; a negative length, read unsigned, is past INT32_MAX;
   * max_bytes_len == 0 disables the limit. */
  emit_mov_w_imm32(buf, A64_TMP1, (uint32_t)INT32_MAX);
  emit_cmp_w_rr(buf, A64_TMP0, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); // HI: len > INT32_MAX
  emit_ldr_w_vm(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, max_bytes_len));
  emit_tst_w_rr(buf, A64_TMP1, A64_TMP1);
  size_t limit_skip = emit_b_cond_placeholder(buf, 0x84, 0); // EQ: max_bytes_len == 0, disabled
  emit_cmp_w_rr(buf, A64_TMP0, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_LIMIT); // HI: len > max_bytes_len
  patch_b_cond(buf, limit_skip, buf->len);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_rr(buf, A64_X1, A64_TMP0);
  emit_call_fn(buf, (uintptr_t)vm_buffer_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bytes_concat2(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  emit_mov_x_rr(buf, A64_TMP0, (unsigned)b);
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_TMP0);
  emit_mov_x_rr(buf, A64_X2, A64_TMP1);
  emit_call_fn(buf, (uintptr_t)jit_bytes_concat2);
  emit_tst_x_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // NULL: the helper set trap_code (BOUNDS)
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bits_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)bp);
  emit_ldr_w_disp(buf, A64_TMP1, A64_TMP0, (uint32_t)offsetof(jello_bytes, length));
  emit_lsl_x_imm3(buf, A64_TMP1, A64_TMP1);
  emit_mov_x_imm64_reg(buf, A64_IP1, end_bit);
  emit_cmp_x_rr(buf, A64_IP1, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS);

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_w_imm32(buf, A64_X1, width);
  emit_mov_w_imm32(buf, A64_X2, bit_off);
  emit_call_fn(buf, (uintptr_t)jit_bits_read);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/* A struct register's declared type fixes its struct_sig (Jello has no struct subtyping), so
 * STRUCT_GET and STRUCT_SET compile to a load or store at a fixed offset. */
static int is_xmm_field_kind(jello_type_kind k) { return k == JELLO_T_F32 || k == JELLO_T_F64; }

/** Loads register `r`, a slot of kind `k`, from `disp` past `base`. */
static void emit_load_slot(emit_ctx* ctx, uint8_t r, jello_type_kind k, unsigned base,
                           uint32_t disp) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = (uint32_t)jello_slot_size(k);
  if(is_xmm_field_kind(k)) {
    int dst = jit_reg_alloc_xmm(ctx->ra, r, 0, width == 8);
    base = emit_disp_base(buf, base, A64_NO_GPR, A64_TMP0, &disp, width);
    if(width == 8) {
      emit_ldr_d_disp(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm64(ctx->ra, r, dst);
    } else {
      emit_ldr_s_disp(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm32(ctx->ra, r, dst);
    }
  } else {
    int dst = jit_reg_alloc_gpr(ctx->ra, r, 0);
    base = emit_disp_base(buf, base, (unsigned)dst, A64_TMP0, &disp, width);
    if(width == 8) {
      emit_ldr_x_disp(buf, (unsigned)dst, base, disp);
      jit_reg_store_home_gpr64(ctx->ra, r, dst);
    } else {
      emit_ldr_w_disp(buf, (unsigned)dst, base, disp);
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
    base = emit_disp_base(buf, base, A64_NO_GPR, A64_TMP0, &disp, width);
    if(width == 8) emit_str_d_disp(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
    else emit_str_s_disp(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
  } else {
    int val = jit_reg_get_gpr(ctx->ra, r, 1);
    base = emit_disp_base(buf, base, (unsigned)val, A64_TMP0, &disp, width);
    if(width == 8) emit_str_x_disp(buf, (unsigned)val, base, disp);
    else emit_str_w_disp(buf, (unsigned)val, base, disp);
  }
}

static void emit_struct_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  uint32_t struct_sig_id = ctx->m->types[tid].as.strct.struct_sig_id;

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_w_imm32(buf, A64_X2, struct_sig_id);
  emit_call_fn(buf, (uintptr_t)jello_struct_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

/** The struct register `r`, null-checked, and the kind and offset of its field `field`. */
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

/** Leaves in A64_TMP1 the address of the element of array register `arr` at index register
 * `idx`, trapping NULL or BOUNDS; returns the elements' kind. An array register's declared type
 * is its array's (FROM_DYN checks the type id), so that kind is fixed. */
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
  emit_load_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), A64_TMP1, 0);
}

static void emit_array_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), A64_TMP1, 0);
}

/** Leaves in A64_TMP1 the running VM's globals, a table of 8-byte slots, and returns the kind of
 * global `g`. */
static jello_type_kind emit_globals_base(emit_ctx* ctx, uint32_t g) {
  emit_ldr_x_vm(ctx->buf, A64_TMP1, (uint32_t)offsetof(jello_vm, globals));
  return ctx->m->types[ctx->m->global_types[g]].kind;
}

static void emit_global_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_load_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), A64_TMP1, ins->imm * 8u);
}

static void emit_global_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), A64_TMP1, ins->imm * 8u);
}

/** Leaves the top call frame's reg_frame pointer, for vm_box_from_typed and vm_store_from_boxed,
 * in x15, despite the x64-style `_r11` name. It writes x9-x11, so it runs only after
 * jit_reg_state_discard_currents. */
static void emit_current_rf_ptr_into_r11(jit_exec_buf* buf) {
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X11, 1u);
  emit_sub_w_rr(buf, A64_X9, A64_X11);
  emit_mov_x_imm64_reg(buf, A64_X11, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_X15, A64_X9);
  emit_mul_x_rr(buf, A64_X15, A64_X11);
  emit_add_x_rr(buf, A64_X15, A64_X10);
  emit_alu_x_imm32(buf, 0x00, A64_X15, (uint32_t)offsetof(call_frame, rf));
}

/** x0-x4 = the arguments vm_box_from_typed and vm_store_from_boxed share for register `r`: the
 * VM, module, function, top frame's reg_frame and `r`. */
static void emit_typed_helper_args(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_current_rf_ptr_into_r11(buf);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, r);
}

/** vm_store_from_boxed of x27 into register `dst`'s home, trapping where it fails. */
static void emit_store_from_boxed_x27(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  emit_typed_helper_args(ctx, dst);
  emit_mov_x_rr(buf, A64_X5, A64_LAYOUT);
  emit_call_fn(buf, (uintptr_t)vm_store_from_boxed);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_HELPER); // 0: the helper set trap_code
}

/** Unboxes the value in x27 into register `dst`, as vm_store_from_boxed does, and binds it. A
 * Dynamic is a move, and an F64 box or an immediate (I8 to I32, Bool, Atom) is read inline.
 * Anything else, an I32 bound for an F64 included, takes vm_store_from_boxed, which converts or
 * traps. Runs after jit_reg_state_discard_currents. */
static void emit_unbox_x27_into(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, dst);
  if(k == JELLO_T_DYNAMIC) {
    int r = jit_reg_alloc_gpr(ctx->ra, dst, 0);
    emit_mov_x_rr(buf, (unsigned)r, A64_LAYOUT);
    jit_reg_store_home_gpr64(ctx->ra, dst, r);
    return;
  }
  int tag = vm_kind_immediate_tag(k);
  int f64 = k == JELLO_T_F64;
  if(!f64 && tag < 0) {
    emit_store_from_boxed_x27(ctx, dst);
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
    emit_obj_kind_guard(buf, A64_LAYOUT, (uint32_t)JELLO_OBJ_BOX_F64, slow, &nslow);
    emit_ldr_d_disp(buf, pi, A64_LAYOUT, (uint32_t)offsetof(jello_box_f64, value));
    jit_reg_store_home_xmm64(ctx->ra, dst, p);
  } else {
    emit_tag_cmp(buf, A64_LAYOUT, (uint32_t)tag);
    slow[nslow++] = emit_b_cond_placeholder(buf, 0x85, 0); // NE: another tag
    emit_u32(buf, a64_ubfx_x((uint8_t)pi, A64_LAYOUT, 3u, vm_kind_immediate_bits(k)));
    jit_reg_store_home_gpr32(ctx->ra, dst, p);
  }
  size_t j_done = emit_b_placeholder(buf);
  for(size_t i = 0; i < nslow; i++) patch_b_cond(buf, slow[i], buf->len);
  emit_store_from_boxed_x27(ctx, dst);
  if(f64) jit_spill_load_xmm64(buf, pi, dst);
  else jit_spill_load_gpr64(buf, pi, dst);
  patch_b(buf, j_done, buf->len);
}

/** Leaves register `r` boxed in x0, as vm_box_from_typed boxes it: a Dynamic is loaded, an
 * immediate (I8 to I32, Bool, Atom) is tagged inline and an F64 goes straight to
 * jello_box_f64_new. It may call out, so it runs after jit_reg_state_discard_currents and keeps
 * nothing in pool registers. */
static void emit_box_into_x0(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, r);
  int tag = vm_kind_immediate_tag(k);
  if(k == JELLO_T_DYNAMIC) {
    jit_spill_load_gpr64(buf, A64_X0, r);
  } else if(tag >= 0) {
    emit_ldr_w_disp(buf, A64_X0, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
    if(k == JELLO_T_BOOL) {
      emit_tst_w_rr(buf, A64_X0, A64_X0);
      emit_u32(buf, a64_cset_w(A64_X0, A64_NE));
    }
    emit_lsl_x_imm3(buf, A64_X0, A64_X0);
    emit_u32(buf, a64_add_x_imm(A64_X0, A64_X0, (uint32_t)tag));
  } else if(k == JELLO_T_F64) {
    emit_u32(buf, a64_ldr_d_uimm(0, (uint8_t)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES));
    emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, (uintptr_t)jello_box_f64_new);
  } else {
    emit_typed_helper_args(ctx, r);
    emit_call_fn(buf, (uintptr_t)vm_box_from_typed);
  }
}

/** Runs the current instruction through its interpreter handler (jit_run_handler). The handler
 * reads and writes register homes, so every binding is dropped first and reloaded from home
 * afterwards. */
static void emit_handler_call(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_w_imm32(buf, A64_X2, ctx->fi);
  emit_mov_w_imm32(buf, A64_X3, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_run_handler);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH); // NE: trapped, located by jit_run_handler
}

/** Traps NULL, or TYPE unless the object in `ep` is an enum of register `r`'s declared type,
 * as op_enum_tag and op_enum_get check. */
static void emit_enum_guard(emit_ctx* ctx, unsigned ep, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_null_check(ctx, ep);
  emit_ldrb_w_disp(buf, A64_TMP1, ep, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp_w_imm32(buf, A64_TMP1, (uint32_t)JELLO_OBJ_ENUM);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_TYPE);
  emit_ldr_w_disp(buf, A64_TMP1, ep, (uint32_t)offsetof(jello_obj_header, type_id));
  emit_cmp_w_imm32(buf, A64_TMP1, ctx->f->reg_types[r]);
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
  emit_u32(buf, a64_add_x_uxtw(A64_TMP1, (uint8_t)ep, (uint8_t)ip, 3));
  jit_reg_state_discard_currents(ctx->ra);
  emit_ldr_x_disp(buf, A64_LAYOUT, A64_TMP1, (uint32_t)offsetof(jello_enum, fields));
  emit_unbox_x27_into(ctx, ins->a);
}

static void emit_to_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_box_into_x0(ctx, ins->b);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_from_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(ctx->buf, A64_LAYOUT, (unsigned)src);
  emit_unbox_x27_into(ctx, ins->a);
}

/** IS_ENUM calls the interpreter's own predicate, so the two cannot disagree. */
static void emit_is_enum(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_x_rr(buf, A64_X0, (unsigned)src);
  emit_mov_w_imm32(buf, A64_X1, ins->imm);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)vm_dynamic_is_enum);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_kindof(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_x_rr(buf, A64_X0, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)vm_kindof_dynamic);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/** Boxes both operands and compares the boxed words; the first waits in a 16-byte stack slot. */
static void emit_physeq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_sp_adjust(buf, 0, 16);
  emit_box_into_x0(ctx, ins->b);
  emit_str_x_sp(buf, A64_X0, 0);
  emit_box_into_x0(ctx, ins->c);
  emit_ldr_x_sp(buf, A64_X10, 0);
  emit_sp_adjust(buf, 1, 16);

  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_cmp_x_rr(buf, A64_X10, A64_X0);
  emit_setcc_from_flags(buf, 0x94, (unsigned)dst);
  emit_mov_w_rr(buf, (unsigned)dst, (unsigned)dst);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/* Until jit_reg_state_discard_currents runs, a pool register (x9-x14) may still be bound to a live
 * vreg, and a raw write bypasses the allocator. So the object ops fetch their operands and run
 * their null and bounds checks first, and write pool registers only after the discard. Values
 * needed across a C call stay in x27 (A64_LAYOUT) or a 16-byte stack slot. */
static void emit_obj_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_imm32(buf, A64_X1, tid);
  emit_call_fn(buf, (uintptr_t)jello_object_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

/** Leaves through the helper trap when the helper just called set vm->trap_code. */
static void emit_helper_trap_check(emit_ctx* ctx) {
  emit_ldr_w_vm(ctx->buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_tst_w_rr(ctx->buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_HELPER); // NE: trap_code set
}

static void emit_obj_has_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->m->proto_enabled || ins->imm == JELLO_ATOM___PROTO__) {
    // No prototype chain to follow: `o`'s own slots answer.
    emit_mov_x_rr(buf, A64_X0, (unsigned)o);
    emit_mov_w_imm32(buf, A64_X1, ins->imm);
    emit_call_fn(buf, (uintptr_t)jello_object_has);
  } else {
    emit_mov_x_rr(buf, A64_X2, (unsigned)o);
    emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
    emit_mov_w_imm32(buf, A64_X3, ins->imm);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/** jit_obj_chain_get's `keep`; only a Dynamic destination keeps the boxed value. */
static int obj_get_keeps(const emit_ctx* ctx, uint8_t dst) {
  return vm_reg_kind(ctx->m, ctx->f, dst) == JELLO_T_DYNAMIC;
}

/** Calls jit_obj_set_f64 for `o` in x27 and the atom in w3, with the F64 register `r`. */
static void emit_obj_set_f64_call(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_u32(buf, a64_ldr_d_uimm(0, (uint8_t)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES));
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_f64);
  emit_helper_trap_check(ctx);
}

/** Whether storing `atom` also sets the prototype link, so that it cannot be a plain store. */
static int obj_atom_links(const emit_ctx* ctx, uint32_t atom) {
  return ctx->m->proto_enabled && atom == JELLO_ATOM___PROTO__;
}

/** Prototype links JIT code follows inline before it leaves the rest of a chain to C. */
#define OBJ_INLINE_LINKS 8u

/** Probes the own table of the Object in x27 for the constant `atom`, as object.c's lookups do.
 * A hit falls through with the slot's index in w10, its state in w13 and the table's states in
 * x11; a miss, `atom` being absent though the prototype chain may have it, takes the returned
 * cbz placeholder. Clobbers x10 to x17. */
static size_t emit_obj_probe(jit_exec_buf* buf, uint32_t atom) {
  emit_ldr_w_disp(buf, A64_TMP1, A64_LAYOUT, (uint32_t)offsetof(jello_object, cap));
  emit_u32(buf, a64_sub_w_imm(A64_TMP1, A64_TMP1, 1u));    // the mask
  emit_mov_w_imm32(buf, A64_X10, vm_atom_hash(atom) - 1u); // the loop steps first
  emit_ldr_x_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(jello_object, states));
  emit_ldr_x_disp(buf, A64_X12, A64_LAYOUT, (uint32_t)offsetof(jello_object, keys));
  size_t loop = buf->len;
  emit_u32(buf, a64_add_w_imm(A64_X10, A64_X10, 1u));
  emit_u32(buf, a64_and_w_rr(A64_X10, A64_X10, A64_TMP1));
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_X11, A64_X10, 0));
  emit_ldrb_w_disp(buf, A64_X13, A64_TMP2, 0);
  size_t miss = emit_cbz_w_placeholder(buf, A64_X13);
  emit_cmp_w_imm32(buf, A64_X13, JELLO_OBJ_SLOT_TOMB);
  emit_u32(buf, a64_b_cond(A64_EQ, off_insns_at(buf->len, loop)));
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_X12, A64_X10, 2));
  emit_ldr_w_disp(buf, A64_X14, A64_TMP2, 0);
  emit_cmp_w_imm32(buf, A64_X14, atom);
  emit_u32(buf, a64_b_cond(A64_NE, off_insns_at(buf->len, loop)));
  return miss;
}

/** Leaves in x17 the address of the value of the slot emit_obj_probe hit. */
static void emit_obj_probed_value_addr(jit_exec_buf* buf) {
  emit_ldr_x_disp(buf, A64_TMP2, A64_LAYOUT, (uint32_t)offsetof(jello_object, vals));
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_TMP2, A64_X10, 3));
}

/** Stores `state` in the slot emit_obj_probe hit. Clobbers x16 and x17. */
static void emit_obj_probed_state_set(jit_exec_buf* buf, uint32_t state) {
  emit_mov_w_imm32(buf, A64_TMP1, state);
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_X11, A64_X10, 0));
  emit_strb_w_disp(buf, A64_TMP1, A64_TMP2, 0);
}

/** OBJ_GET_ATOM. A field is read inline (emit_obj_probe) from the object or, through up to
 * OBJ_INLINE_LINKS prototype links, the first one that has it, a Dynamic destination taking the
 * box from a slot that owns it; anything else takes jit_obj_chain_get from the last object
 * probed. Either way the boxed value ends in x27 to be unboxed. */
static void emit_obj_get_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);

  int chained = ctx->m->proto_enabled && !obj_atom_links(ctx, ins->imm);
  if(chained) emit_mov_w_imm32(buf, A64_X5, OBJ_INLINE_LINKS);
  size_t probe = buf->len;
  size_t miss = emit_obj_probe(buf, ins->imm);
  if(obj_get_keeps(ctx, ins->a)) {
    emit_cmp_w_imm32(buf, A64_X13, JELLO_OBJ_SLOT_OWNED);
    size_t plain = emit_b_cond_placeholder(buf, 0x85, 0);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    patch_b_cond(buf, plain, buf->len);
  }
  emit_obj_probed_value_addr(buf);
  emit_ldr_x_disp(buf, A64_LAYOUT, A64_TMP2, 0);
  size_t j_done = emit_b_placeholder(buf);
  patch_cbz_w(buf, miss, buf->len);
  if(chained) {
    size_t last = emit_cbz_w_placeholder(buf, A64_X5);
    emit_ldr_x_disp(buf, A64_X9, A64_LAYOUT, (uint32_t)offsetof(jello_object, proto));
    emit_tst_x_rr(buf, A64_X9, A64_X9);
    size_t end = emit_b_cond_placeholder(buf, 0x84, 0); // EQ: no prototype
    emit_u32(buf, a64_sub_w_imm(A64_X5, A64_X5, 1u));
    emit_mov_x_rr(buf, A64_LAYOUT, A64_X9);
    emit_u32(buf, a64_b(off_insns_at(buf->len, probe)));
    patch_cbz_w(buf, last, buf->len);
    patch_b_cond(buf, end, buf->len);
  }

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_mov_w_imm32(buf, A64_X3, ins->imm);
  emit_mov_w_imm32(buf, A64_X4, (uint32_t)obj_get_keeps(ctx, ins->a));
  emit_call_fn(buf, (uintptr_t)jit_obj_chain_get);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X0); // x27 = the value; o is dead

  emit_helper_trap_check(ctx);

  patch_b(buf, j_done, buf->len);
  emit_unbox_x27_into(ctx, ins->a);
}

/** OBJ_SET_ATOM. An F64 goes into the box of an own slot that owns one, and another value into
 * an own unsealed slot, inline (emit_obj_probe); anything else, and __proto__ when it links
 * prototypes, takes jit_obj_set_f64 or jit_obj_set_direct. */
static void emit_obj_set_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
  if(vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_F64) {
    size_t miss = emit_obj_probe(buf, ins->imm);
    emit_cmp_w_imm32(buf, A64_X13, JELLO_OBJ_SLOT_OWNED);
    size_t unowned = emit_b_cond_placeholder(buf, 0x85, 0);
    emit_obj_probed_value_addr(buf);
    emit_ldr_x_disp(buf, A64_TMP2, A64_TMP2, 0);
    emit_u32(buf, a64_ldr_d_uimm(0, (uint8_t)JIT_REG_BASE, (uint32_t)ins->a * REG_SLOT_BYTES));
    emit_u32(buf, a64_str_d_uimm(0, A64_TMP2, (uint32_t)offsetof(jello_box_f64, value)));
    size_t j_done = emit_b_placeholder(buf);
    patch_cbz_w(buf, miss, buf->len);
    patch_b_cond(buf, unowned, buf->len);
    emit_mov_w_imm32(buf, A64_X3, ins->imm);
    emit_obj_set_f64_call(ctx, ins->a);
    patch_b(buf, j_done, buf->len);
    return;
  }
  emit_box_into_x0(ctx, ins->a);
  int inline_set = !obj_atom_links(ctx, ins->imm);
  size_t j_done = 0;
  if(inline_set) {
    size_t miss = emit_obj_probe(buf, ins->imm);
    emit_cmp_w_imm32(buf, A64_X13, JELLO_OBJ_SLOT_SEALED);
    size_t sealed = emit_b_cond_placeholder(buf, 0x84, 0); // EQ: the helper traps
    emit_obj_probed_value_addr(buf);
    emit_str_x_disp(buf, A64_X0, A64_TMP2, 0);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    j_done = emit_b_placeholder(buf);
    patch_cbz_w(buf, miss, buf->len);
    patch_b_cond(buf, sealed, buf->len);
  }

  emit_mov_x_rr(buf, A64_X4, A64_X0);
  emit_mov_w_imm32(buf, A64_X3, ins->imm);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_direct);
  emit_helper_trap_check(ctx);
  if(inline_set) patch_b(buf, j_done, buf->len);
}

static void emit_obj_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); // HS: atom_id >= natoms
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
  emit_mov_w_rr(buf, A64_X13, (unsigned)atom);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_mov_w_rr(buf, A64_X3, A64_X13);
  emit_mov_w_imm32(buf, A64_X4, (uint32_t)obj_get_keeps(ctx, ins->a));
  emit_call_fn(buf, (uintptr_t)jit_obj_chain_get);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X0);

  emit_helper_trap_check(ctx);

  emit_unbox_x27_into(ctx, ins->a);
}

static void emit_obj_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
  if(vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_F64) {
    emit_mov_w_rr(buf, A64_X3, (unsigned)atom);
    emit_obj_set_f64_call(ctx, ins->a);
    return;
  }
  // `atom` waits in a 16-byte stack slot across the boxing.
  emit_sp_adjust(buf, 0, 16);
  emit_mov_w_rr(buf, A64_X13, (unsigned)atom);
  emit_str_x_sp(buf, A64_X13, 0);
  emit_box_into_x0(ctx, ins->a);
  emit_ldr_x_sp(buf, A64_X5, 0);
  emit_sp_adjust(buf, 1, 16);

  emit_mov_x_rr(buf, A64_X4, A64_X0);
  emit_mov_w_rr(buf, A64_X3, A64_X5);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_direct);
  emit_helper_trap_check(ctx);
}

static void emit_obj_has(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  if(!ctx->m->proto_enabled) {
    // No prototype chain to follow: `o`'s own slots answer.
    emit_mov_x_rr(buf, A64_X0, (unsigned)o);
    emit_mov_w_rr(buf, A64_X1, (unsigned)atom);
    jit_reg_state_discard_currents(ctx->ra);
    emit_call_fn(buf, (uintptr_t)jello_object_has);
  } else {
    jit_reg_state_discard_currents(ctx->ra);
    emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
    emit_mov_w_rr(buf, A64_X13, (unsigned)atom);
    emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
    emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
    emit_mov_w_rr(buf, A64_X3, A64_X13);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_remove(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_x_rr(buf, A64_X1, (unsigned)o);
  emit_mov_w_rr(buf, A64_X2, (unsigned)atom);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)jit_obj_remove);
  emit_helper_trap_check(ctx);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_clear(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_x_rr(buf, A64_X1, (unsigned)o);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)jit_obj_clear);
  emit_helper_trap_check(ctx);
}

static void emit_obj_copy(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)src, (unsigned)src);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  // Staged outside the pool; before the discard, a pool register may hold another live value.
  emit_mov_x_rr(buf, A64_TMP0, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_TMP0);
  emit_call_fn(buf, (uintptr_t)jello_object_copy);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
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
        emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      }
      break;
    }
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_I64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_x_imm64_reg(ctx->buf, (unsigned)d, (uint64_t)ctx->m->const_i64[ins->imm]);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F16: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm & 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t bits;
      memcpy(&bits, &ctx->m->const_f64[ins->imm], 8);
      emit_mov_x_imm64_reg(ctx->buf, (unsigned)d, bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_BOOL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm ? 1u : 0u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_BIN_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_w_rr(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_w_rr(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_mul_w_rr(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_w_rr(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_w_rr(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_w_rr(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        /* The quotient goes to A64_TMP0, not `dst`; `dst` may share a register with `lhs` (as in
         * `x %= y`), and MSUB still reads the original `lhs`. */
        case JIR_BIN_DIV:
        case JIR_BIN_MOD: {
          emit_divmod_guard_i32(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_u32(ctx->buf, a64_sdiv_w(A64_TMP0, (uint8_t)lhs, (uint8_t)rhs));
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) {
            emit_u32(ctx->buf, a64_msub_w((uint8_t)dst, A64_TMP0, (uint8_t)rhs, (uint8_t)lhs));
          } else {
            emit_mov_w_rr(ctx->buf, (uint8_t)dst, A64_TMP0);
          }
          break;
        }
        /* The count is masked to 5 bits with AND (immediate); emit_alu_w_imm32 would stage the
         * mask in A64_TMP0, which holds the count. */
        case JIR_BIN_SHL:
        case JIR_BIN_SHR: {
          emit_mov_w_rr(ctx->buf, A64_TMP0, (unsigned)rhs);
          emit_u32(ctx->buf, a64_and_w_imm31(A64_TMP0, A64_TMP0));
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_lslv_w(ctx->buf, (unsigned)dst);
          else emit_asrv_w(ctx->buf, (unsigned)dst);
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
      emit_mov_x_rr(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_x_rr(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_x_rr(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_mul_x_rr(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_x_rr(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_x_rr(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_x_rr(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        // The quotient goes to A64_TMP0, as in the I32 case.
        case JIR_BIN_DIV:
        case JIR_BIN_MOD: {
          emit_divmod_guard_i64(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_u32(ctx->buf, a64_sdiv_x(A64_TMP0, (uint8_t)lhs, (uint8_t)rhs));
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) {
            emit_u32(ctx->buf, a64_msub_x((uint8_t)dst, A64_TMP0, (uint8_t)rhs, (uint8_t)lhs));
          } else {
            emit_mov_x_rr(ctx->buf, (uint8_t)dst, A64_TMP0);
          }
          break;
        }
        // The count is masked to 6 bits, as in the I32 case.
        case JIR_BIN_SHL:
        case JIR_BIN_SHR: {
          emit_mov_w_rr(ctx->buf, A64_TMP0, (unsigned)rhs);
          emit_u32(ctx->buf, a64_and_x_imm63(A64_TMP0, A64_TMP0));
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_lslv_x(ctx->buf, (unsigned)dst);
          else emit_asrv_x(ctx->buf, (unsigned)dst);
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
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x00, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_SUB_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x05, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_MUL_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mul_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_w(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_x(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_eor_w_imm32(ctx->buf, (unsigned)d, 0x80000000u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mov_x_imm64_reg(ctx->buf, A64_TMP0, 0x8000000000000000ULL);
      emit_alu_x_rr(ctx->buf, 0x31, (unsigned)d, A64_TMP0);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NOT_BOOL: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_eor_w_imm32(ctx->buf, (unsigned)d, 1u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_w_rr(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C,
                            (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_EQ_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_w_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, 0x94, (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_x_rr(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C,
                            (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_LT_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_w_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, 0x9C, (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
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
      emit_mov_x_imm64_reg(ctx->buf, (unsigned)d, null_bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_ATOM: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_TO_DYN: emit_to_dyn(ctx, ins); break;
    case JIR_FROM_DYN: emit_from_dyn(ctx, ins); break;
    case JIR_KINDOF: emit_kindof(ctx, ins); break;
    case JIR_IS_ENUM: emit_is_enum(ctx, ins); break;
    case JIR_PHYSEQ: emit_physeq(ctx, ins); break;
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

  // The survive plan reads is_target; a hold never spans a branch target.
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
      size_t at = emit_b_placeholder(buf);
      push_branch(&ctx, at, tgt, 0);
      goto insn_done;
    }
    if(op == JIR_JMP_IF) {
      uint32_t tgt = 0;
      (void)jit_resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt);
      int cond = jit_reg_get_gpr(&ra, ins->a, 1);
      size_t at = emit_cbnz_w_placeholder(buf, (unsigned)cond);
      push_branch(&ctx, at, tgt, 2);
      goto insn_done;
    }
    if(op == JIR_SAFEPOINT) {
      /* vm_safepoint_check returns at once unless a stop-the-world is pending, so it is called
       * only when stop_requested is set. A plain load suffices; the check reloads the flag with
       * full ordering before it parks. Either way the pool counts as clobbered after. */
      _Static_assert(offsetof(jello_heap, stop_requested) % 4 == 0 &&
                         offsetof(jello_heap, stop_requested) / 4 <= 0xFFF,
                     "ldr w encodes the offset");
      emit_ldr_x_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, heap));
      emit_ldr_w_disp(buf, A64_TMP0, A64_TMP0, (uint32_t)offsetof(jello_heap, stop_requested));
      size_t no_stop = emit_cbz_w_placeholder(buf, A64_TMP0);
      emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (uintptr_t)vm_safepoint_check);
      patch_cbz_w(buf, no_stop, buf->len);
      jit_reg_state_clobber_call(&ra);
      // A reduction; the charge that ends the window calls vm_reductions_out.
      emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, reductions));
      emit_u32(buf, a64_subs_w_imm(A64_TMP0, A64_TMP0, 1));
      emit_str_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, reductions));
      size_t in_window = emit_b_cond_placeholder(buf, 0x8F, 0); // GT: reductions left
      emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (uintptr_t)vm_reductions_out);
      emit_tst_w_rr(buf, A64_X0, A64_X0);
      emit_jcc_trap(&ctx, 0x85, JIT_TRAP_HELPER); // NE: out of fuel
      patch_b_cond(buf, in_window, buf->len);
      goto insn_done;
    }
    if(op == JIR_CALLR) {
      ok = emit_callr_insn(&ctx, ins);
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
      /* Arming the hold before emit_call_insn's discard is safe; homes are write-through, so the
       * candidate's home is already current. */
      if(splan.armed[i]) jit_reg_survive_hold(&ra, splan.candidate[i]);
      ok = emit_call_insn(&ctx, ins, op);
      goto insn_done;
    }
    ok = emit_insn(&ctx, ins);
  insn_done:
    /* The one release point for every opcode; jit_reg_get_gpr already serves a held vreg to
     * whichever operand the consumer fetches it through. */
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
      patch_b(buf, at, epilogue_ok_off);
    }
    for(size_t i = 0; i < ctx.branch_count; i++) {
      size_t at = ctx.branches[i].at;
      uint32_t tgt = ctx.branches[i].bc_tgt;
      if(tgt >= f->ninsns || native_off[tgt] == (size_t)-1) {
        ok = 0;
        break;
      }
      size_t tgt_off = native_off[tgt];
      if(ctx.branches[i].is_jcc == 2) patch_cbnz_w(buf, at, tgt_off);
      else if(ctx.branches[i].is_jcc) patch_b_cond(buf, at, tgt_off);
      else patch_b(buf, at, tgt_off);
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
   * write the result into the caller's frame and return here without restoring the chain-wide
   * registers, so this stub saves and restores them itself, with the FP pool and x28
   * (JIT_REG_SURVIVE). x16 pads the pair, since LDP may not load one register twice. */
  *enter_off = buf->len;
  emit_u32(buf, a64_stp_x_pre(A64_FP, A64_LR, A64_SP, -2));
  emit_u32(buf, a64_mov_x_rr(A64_FP, A64_SP));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, -2));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, -2));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_SURVIVE, A64_IP0, A64_SP, -2));
  emit_save_fp_pool(buf);
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_VM, A64_X0));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_BASE, A64_X1));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_OUT, A64_X2));
  emit_u32(buf, a64_blr_x(A64_X3));
  emit_restore_fp_pool(buf);
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_SURVIVE, A64_IP0, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post(A64_FP, A64_LR, A64_SP, 2));
  emit_ret_insn_a64(buf);

  /* bridge: the target of a JIT call to a declined function. The caller already pushed the
   * callee frame; this runs it in the interpreter (jit_run_interp_top), then points JIT_REG_BASE
   * back at the caller's frame, as a compiled callee's RET or trap tail would. The caller checks
   * vm->trap_code after the call either way. emit_call_fn keeps LR across each helper call. */
  *bridge_off = buf->len;
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_run_interp_top);
  emit_reload_jit_reg_base(buf);
  emit_ret_insn_a64(buf);

  /* lazy: the entry-table target of a function not yet compiled. The caller already pushed its
   * frame; jit_lazy_target compiles it when it is due and returns its body or the bridge, which
   * this jumps to with the caller's LR, so the callee returns straight to the call site. */
  *lazy_off = buf->len;
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_lazy_target);
  emit_u32(buf, a64_br_x(A64_X0));
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
