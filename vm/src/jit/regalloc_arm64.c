// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * arm64 register-allocation backend; the register pools and the encoders that load and store a
 * register's home slot. */

#include <jello/internal/jit_impl.h>
#include <jello/internal/jit_arm64.h>

/* The GPR pool is x9-x14, caller-saved under AAPCS64, so JIT code need not preserve them for
 * its C caller. FP pool index i is d(8 + i); d8-d15 are callee-saved, so JIT entry code saves
 * them. */
const uint8_t jit_gpr_pool[] = {9, 10, 11, 12, 13, 14};
const uint32_t jit_gpr_pool_len = 6;
const uint32_t jit_xmm_pool_len = 8;

static void emit_u32(jit_exec_buf* buf, uint32_t word) { jit_exec_buf_write(buf, &word, 4); }

static uint8_t pool_dreg(unsigned pool_idx) { return (uint8_t)(8u + pool_idx); }

void jit_spill_load_gpr64(jit_exec_buf* buf, unsigned dst_gpr, uint8_t vreg) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst_gpr, (uint8_t)JIT_REG_BASE, off));
}

void jit_spill_store_gpr32(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_str_w_uimm((uint8_t)src_gpr, (uint8_t)JIT_REG_BASE, off));
}

void jit_spill_store_gpr64(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_str_x_uimm((uint8_t)src_gpr, (uint8_t)JIT_REG_BASE, off));
}

void jit_spill_load_xmm32(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_ldr_s_uimm(pool_dreg(dst_xmm), (uint8_t)JIT_REG_BASE, off));
}

void jit_spill_load_xmm64(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_ldr_d_uimm(pool_dreg(dst_xmm), (uint8_t)JIT_REG_BASE, off));
}

void jit_spill_store_xmm32(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_str_s_uimm(pool_dreg(src_xmm), (uint8_t)JIT_REG_BASE, off));
}

void jit_spill_store_xmm64(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_str_d_uimm(pool_dreg(src_xmm), (uint8_t)JIT_REG_BASE, off));
}
