// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * AArch64 instruction encoders for the arm64 JIT; each returns one instruction word. Register
 * arguments are register numbers, where 31 is sp or zr as the instruction defines. Branch offsets
 * are signed and counted in instructions. */

#ifndef JELLO_JIT_ARM64_H
#define JELLO_JIT_ARM64_H

#include <stdint.h>

static inline uint32_t a64_movz_w(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0x52800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_movk_w(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0x72800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_movz_x(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0xD2800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_movk_x(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0xF2800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_add_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x0B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_sub_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x4B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_add_w_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0x11000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_sub_w_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0x51000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_subs_w_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0x71000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_subs_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x6B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_add_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_add_x_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0x91000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** AArch64 condition codes, as B.cond and CSET encode them. */
typedef enum a64_cond {
  A64_EQ = 0x0,
  A64_NE = 0x1,
  A64_HS = 0x2,
  A64_LO = 0x3,
  A64_MI = 0x4,
  A64_PL = 0x5,
  A64_VS = 0x6,
  A64_VC = 0x7,
  A64_HI = 0x8,
  A64_LS = 0x9,
  A64_GE = 0xA,
  A64_LT = 0xB,
  A64_GT = 0xC,
  A64_LE = 0xD,
} a64_cond;

/** b.cond to `off_insns` instructions from this one. */
static inline uint32_t a64_b_cond(a64_cond cond, int32_t off_insns) {
  uint32_t imm19 = ((uint32_t)off_insns & 0x7FFFFu) << 5;
  return 0x54000000u | imm19 | (uint32_t)cond;
}

/** cset wd, cond (CSINC wd, wzr, wzr, !cond). */
static inline uint32_t a64_cset_w(uint8_t rd, a64_cond cond) {
  uint8_t inv = (uint8_t)(cond ^ 1u);
  return 0x1A800000u | (1u << 10) | ((uint32_t)31u << 16) | ((uint32_t)inv << 12) |
         ((uint32_t)31u << 5) | (uint32_t)rd;
}

static inline uint32_t a64_cmp_w_imm0(uint8_t rn) {
  return 0x71000000u | ((uint32_t)rn << 5) | 0x1Fu;
}

static inline uint32_t a64_mul_w(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x1B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_sdiv_w(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x1AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_msub_w(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
  return 0x1B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}

static inline uint32_t a64_lslv_w(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x1AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_lsrv_w(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x1AC02400u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_neg_w(uint8_t rd, uint8_t rm) {
  return 0x4B0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

static inline uint32_t a64_ldr_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xB9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_str_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xB9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_ldr_x_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xF9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_str_x_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xF9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_sub_x_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0xD1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** cmn wn, #imm12 (ADDS wzr, wn, #imm12); sets Z when wn == -imm12. CMP (immediate) cannot
 * encode a negative immediate, so `cmn wn, #1` is the scratch-free test for -1. */
static inline uint32_t a64_cmn_w_imm(uint8_t rn, uint32_t imm12) {
  return 0x31000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | 0x1Fu;
}

static inline uint32_t a64_cmn_x_imm(uint8_t rn, uint32_t imm12) {
  return 0xB1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | 0x1Fu;
}

/** and wd, wn, #7: a Dynamic's tag. */
static inline uint32_t a64_and_w_imm7(uint8_t rd, uint8_t rn) {
  return 0x12000800u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** ubfx xd, xn, #lsb, #width, zero-extended; UBFM xd, xn, #lsb, #(lsb + width - 1). */
static inline uint32_t a64_ubfx_x(uint8_t rd, uint8_t rn, uint32_t lsb, uint32_t width) {
  return 0xD3400000u | (lsb << 16) | ((lsb + width - 1u) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}

/** and wd, wn, #0x1f: masks a shift count in one instruction, with no scratch register. */
static inline uint32_t a64_and_w_imm31(uint8_t rd, uint8_t rn) {
  return 0x12001000u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** and xd, xn, #0x3f. */
static inline uint32_t a64_and_x_imm63(uint8_t rd, uint8_t rn) {
  return 0x92401400u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_sub_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0xCB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_subs_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0xEB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_mul_x(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x9B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_sdiv_x(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_msub_x(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
  return 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}

static inline uint32_t a64_lslv_x(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x9AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_lsrv_x(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x9AC02400u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_neg_x(uint8_t rd, uint8_t rm) {
  return 0xCB0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

static inline uint32_t a64_ldr_d_uimm(uint8_t dt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xFD400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)dt;
}

static inline uint32_t a64_str_d_uimm(uint8_t dt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xFD000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)dt;
}

static inline uint32_t a64_fadd_d(uint8_t dd, uint8_t dn, uint8_t dm) {
  return 0x1E602800u | ((uint32_t)dm << 16) | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fsub_d(uint8_t dd, uint8_t dn, uint8_t dm) {
  return 0x1E603800u | ((uint32_t)dm << 16) | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fmul_d(uint8_t dd, uint8_t dn, uint8_t dm) {
  return 0x1E600800u | ((uint32_t)dm << 16) | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fdiv_d(uint8_t dd, uint8_t dn, uint8_t dm) {
  return 0x1E601800u | ((uint32_t)dm << 16) | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fsqrt_d(uint8_t dd, uint8_t dn) {
  return 0x1E61C000u | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fneg_d(uint8_t dd, uint8_t dn) {
  return 0x1E614000u | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fmov_d(uint8_t dd, uint8_t dn) {
  return 0x1E604000u | ((uint32_t)dn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fmov_d_x(uint8_t dd, uint8_t xn) {
  return 0x9E670000u | ((uint32_t)xn << 5) | (uint32_t)dd;
}

/** fmov sd, wn. */
static inline uint32_t a64_fmov_s_x(uint8_t sd, uint8_t xn) {
  return 0x1E270000u | ((uint32_t)xn << 5) | (uint32_t)sd;
}

/** fmov wd, sn. */
static inline uint32_t a64_fmov_x_s(uint8_t xn, uint8_t sn) {
  return 0x1E260000u | ((uint32_t)sn << 5) | (uint32_t)xn;
}

static inline uint32_t a64_sxtw_x_w(uint8_t xd, uint8_t ws) {
  return 0x93407C00u | ((uint32_t)ws << 5) | (uint32_t)xd;
}

/** sxth wd, wn (SBFM wd, wn, #0, #15). */
static inline uint32_t a64_sxth_w(uint8_t wd, uint8_t wn) {
  return 0x13003C00u | ((uint32_t)wn << 5) | (uint32_t)wd;
}

static inline uint32_t a64_asrv_w(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x1AC02800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_asrv_x(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x9AC02800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_fmov_x_d(uint8_t xn, uint8_t dn) {
  return 0x9E660000u | ((uint32_t)dn << 5) | (uint32_t)xn;
}

static inline uint32_t a64_fneg_s(uint8_t sd, uint8_t sn) {
  return 0x1E214000u | ((uint32_t)sn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_fcmp_d(uint8_t dn, uint8_t dm) {
  return 0x1E602000u | ((uint32_t)dm << 16) | ((uint32_t)dn << 5);
}

static inline uint32_t a64_ldr_s_uimm(uint8_t st, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xBD400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)st;
}

static inline uint32_t a64_str_s_uimm(uint8_t st, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xBD000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)st;
}

static inline uint32_t a64_fadd_s(uint8_t sd, uint8_t sn, uint8_t sm) {
  return 0x1E202800u | ((uint32_t)sm << 16) | ((uint32_t)sn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_fsub_s(uint8_t sd, uint8_t sn, uint8_t sm) {
  return 0x1E203800u | ((uint32_t)sm << 16) | ((uint32_t)sn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_fmul_s(uint8_t sd, uint8_t sn, uint8_t sm) {
  return 0x1E200800u | ((uint32_t)sm << 16) | ((uint32_t)sn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_fdiv_s(uint8_t sd, uint8_t sn, uint8_t sm) {
  return 0x1E201800u | ((uint32_t)sm << 16) | ((uint32_t)sn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_fcmp_s(uint8_t sn, uint8_t sm) {
  return 0x1E202000u | ((uint32_t)sm << 16) | ((uint32_t)sn << 5);
}

static inline uint32_t a64_fmov_s(uint8_t sd, uint8_t sn) {
  return 0x1E204000u | ((uint32_t)sn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_scvtf_s_w(uint8_t sd, uint8_t wn) {
  return 0x1E220000u | ((uint32_t)wn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_scvtf_d_w(uint8_t dd, uint8_t wn) {
  return 0x1E620000u | ((uint32_t)wn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_scvtf_s_x(uint8_t sd, uint8_t xn) {
  return 0x9E220000u | ((uint32_t)xn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_scvtf_d_x(uint8_t dd, uint8_t xn) {
  return 0x9E620000u | ((uint32_t)xn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fcvtzs_w_s(uint8_t wd, uint8_t sn) {
  return 0x1E380000u | ((uint32_t)sn << 5) | (uint32_t)wd;
}

static inline uint32_t a64_fcvtzs_w_d(uint8_t wd, uint8_t dn) {
  return 0x1E780000u | ((uint32_t)dn << 5) | (uint32_t)wd;
}

static inline uint32_t a64_fcvtzs_x_s(uint8_t xd, uint8_t sn) {
  return 0x9E380000u | ((uint32_t)sn << 5) | (uint32_t)xd;
}

static inline uint32_t a64_fcvtzs_x_d(uint8_t xd, uint8_t dn) {
  return 0x9E780000u | ((uint32_t)dn << 5) | (uint32_t)xd;
}

static inline uint32_t a64_fcvt_d_s(uint8_t dd, uint8_t sn) {
  return 0x1E22C000u | ((uint32_t)sn << 5) | (uint32_t)dd;
}

static inline uint32_t a64_fcvt_s_d(uint8_t sd, uint8_t dn) {
  return 0x1E624000u | ((uint32_t)dn << 5) | (uint32_t)sd;
}

static inline uint32_t a64_cbz_x(uint8_t rt, int32_t off_insns) {
  uint32_t imm19 = ((uint32_t)off_insns & 0x7FFFFu) << 5;
  return 0xB4000000u | imm19 | (uint32_t)rt;
}

static inline uint32_t a64_cbz_w(uint8_t rt, int32_t off_insns) {
  uint32_t imm19 = ((uint32_t)off_insns & 0x7FFFFu) << 5;
  return 0x34000000u | imm19 | (uint32_t)rt;
}

static inline uint32_t a64_cbnz_w(uint8_t rt, int32_t off_insns) {
  uint32_t imm19 = ((uint32_t)off_insns & 0x7FFFFu) << 5;
  return 0x35000000u | imm19 | (uint32_t)rt;
}

static inline uint32_t a64_b(int32_t off_insns) {
  return 0x14000000u | ((uint32_t)off_insns & 0x03FFFFFFu);
}

static inline uint32_t a64_bl(int32_t off_insns) {
  return 0x94000000u | ((uint32_t)off_insns & 0x03FFFFFFu);
}

static inline uint32_t a64_blr_x(uint8_t xn) { return 0xD63F0000u | ((uint32_t)xn << 5); }

static inline uint32_t a64_br_x(uint8_t xn) { return 0xD61F0000u | ((uint32_t)xn << 5); }

/** cmp wn, #imm; `imm` must fit in 12 bits. */
static inline uint32_t a64_cmp_w_imm(uint8_t rn, uint16_t imm) {
  return 0x71000000u | ((uint32_t)imm << 10) | ((uint32_t)rn << 5) | 0x1Fu;
}

static inline uint32_t a64_and_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x0A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_and_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x8A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_ands_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x6A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_ands_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0xEA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_orr_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x2A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_orr_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0xAA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** lsl wd, wn, #sh (UBFM with immr = -sh mod 32, imms = 31 - sh). */
static inline uint32_t a64_lsl_w_imm(uint8_t rd, uint8_t rn, uint8_t sh) {
  uint32_t immr = (uint32_t)((32u - sh) & 31u);
  uint32_t imms = (uint32_t)(31u - sh);
  return 0x53000000u | (immr << 16) | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** add xd, xn, wm, uxtw #sh (ADD, extended register); sh is at most 4. */
static inline uint32_t a64_add_x_uxtw(uint8_t rd, uint8_t rn, uint8_t wm, uint8_t sh) {
  return 0x8B204000u | ((uint32_t)wm << 16) | ((uint32_t)sh << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}

static inline uint32_t a64_uxtw_x_w(uint8_t xd, uint8_t ws) {
  return 0xD3407C00u | ((uint32_t)ws << 5) | (uint32_t)xd;
}

/** lsl xd, xn, #sh (UBFM with N = 1, immr = -sh mod 64, imms = 63 - sh). */
static inline uint32_t a64_lsl_x_imm(uint8_t rd, uint8_t rn, uint8_t sh) {
  uint32_t immr = (uint32_t)((64u - sh) & 63u);
  uint32_t imms = (uint32_t)(63u - sh);
  return 0xD3400000u | (immr << 16) | (imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

/** lsr wd, wm, #sh, encoded as ORR wd, wzr, wm, LSR #sh. */
static inline uint32_t a64_lsr_w_imm(uint8_t rd, uint8_t rm, uint8_t sh) {
  return 0x2A4003E0u | ((uint32_t)rm << 16) | ((uint32_t)sh << 10) | (uint32_t)rd;
}

/** lsr xd, xm, #sh, encoded as ORR xd, xzr, xm, LSR #sh. */
static inline uint32_t a64_lsr_x_imm(uint8_t rd, uint8_t rm, uint8_t sh) {
  return 0xAA4003E0u | ((uint32_t)rm << 16) | ((uint32_t)sh << 10) | (uint32_t)rd;
}

static inline uint32_t a64_mov_w_rr(uint8_t rd, uint8_t rm) {
  return 0x2A0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

static inline uint32_t a64_mov_x_rr(uint8_t rd, uint8_t rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

static inline uint32_t a64_ldrb_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  return 0x39400000u | ((byte_off & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_strb_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  return 0x39000000u | ((byte_off & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

/** ldrh wt, [xn, #byte_off]: `byte_off` is even and at most 8190, as imm12 counts halfwords.
 * The same holds for a64_strh_w_uimm. */
static inline uint32_t a64_ldrh_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 2u;
  return 0x79400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_strh_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 2u;
  return 0x79000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_eor_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x4A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_eor_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0xCA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_cbnz_x(uint8_t rt, int32_t off_insns) {
  uint32_t imm19 = ((uint32_t)off_insns & 0x7FFFFu) << 5;
  return 0xB5000000u | imm19 | (uint32_t)rt;
}

static inline uint32_t a64_ret(void) { return 0xD65F03C0u; }

/** stp xt, xt2, [xn, #imm7 * 8]!; the ldp and d-register forms below scale `imm7` by 8 too. */
static inline uint32_t a64_stp_x_pre(uint8_t rt, uint8_t rt2, uint8_t rn, int32_t imm7) {
  return 0xA9800000u | (((uint32_t)imm7 & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) |
         ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_ldp_x_post(uint8_t rt, uint8_t rt2, uint8_t rn, int32_t imm7) {
  return 0xA8C00000u | (((uint32_t)imm7 & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) |
         ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_stp_d_pre(uint8_t dt, uint8_t dt2, uint8_t rn, int32_t imm7) {
  return 0x6D800000u | (((uint32_t)imm7 & 0x7Fu) << 15) | ((uint32_t)dt2 << 10) |
         ((uint32_t)rn << 5) | (uint32_t)dt;
}

static inline uint32_t a64_ldp_d_post(uint8_t dt, uint8_t dt2, uint8_t rn, int32_t imm7) {
  return 0x6CC00000u | (((uint32_t)imm7 & 0x7Fu) << 15) | ((uint32_t)dt2 << 10) |
         ((uint32_t)rn << 5) | (uint32_t)dt;
}

static inline uint32_t a64_rev16_w(uint8_t rd, uint8_t rn) {
  return 0x5AC00400u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_rev_w(uint8_t rd, uint8_t rn) {
  return 0x5AC00800u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_rev_x(uint8_t rd, uint8_t rn) {
  return 0xDAC00C00u | ((uint32_t)rn << 5) | (uint32_t)rd;
}

#endif
