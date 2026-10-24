// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The register allocator's backend-independent bookkeeping; caches registers' home slots in
 * pool registers, with eviction, pinning and the survive register. */

#include <jello/internal/jit_impl.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

_Atomic uint32_t jit_reg_reload_count;
_Atomic uint32_t jit_reg_discard_count;
_Atomic uint32_t jit_call_survive_armed_count;
_Atomic uint32_t jit_call_survive_decline_count;

static int preg_is_xmm(int preg) { return preg >= (int)JIT_PREG_XMM0; }

static int preg_xmm_index(int preg) { return preg - (int)JIT_PREG_XMM0; }

static int preg_to_gpr_pool_index(int preg) {
  for(unsigned i = 0; i < jit_gpr_pool_len; i++) {
    if((int)jit_gpr_pool[i] == preg) return (int)i;
  }
  return -1;
}

void jit_reg_state_init(jit_reg_state* st, uint32_t nregs, jit_reg_emit_ctx emit) {
  st->nregs = nregs;
  st->emit = emit;
  st->current = (int8_t*)malloc(nregs ? nregs : 1u);
  if(st->current) {
    for(uint32_t i = 0; i < nregs; i++) st->current[i] = (int8_t)JIT_PREG_NONE;
  }
  for(unsigned i = 0; i < jit_gpr_pool_len; i++) st->gpr_vreg[i] = JIT_PREG_NONE;
  for(unsigned i = 0; i < jit_xmm_pool_len; i++) st->xmm_vreg[i] = JIT_PREG_NONE;
  memset(st->gpr_pinned, 0, sizeof st->gpr_pinned);
  memset(st->xmm_pinned, 0, sizeof st->xmm_pinned);
  st->next_gpr_victim = 0;
  st->next_xmm_victim = 0;
  st->survive_held = 0;
  st->survive_vreg = 0;
}

void jit_reg_state_free(jit_reg_state* st) {
  free(st->current);
  st->current = NULL;
}

void jit_reg_state_clobber_call(jit_reg_state* st) {
  for(uint32_t v = 0; v < st->nregs; v++) {
    if(st->current[v] != (int8_t)JIT_PREG_NONE) {
      int preg = (int)st->current[v];
      if(preg_is_xmm(preg)) {
        int xi = preg_xmm_index(preg);
        if(xi >= 0 && xi < (int)jit_xmm_pool_len) st->xmm_vreg[xi] = JIT_PREG_NONE;
      } else {
        int gi = preg_to_gpr_pool_index(preg);
        if(gi >= 0) st->gpr_vreg[gi] = JIT_PREG_NONE;
      }
      st->current[v] = (int8_t)JIT_PREG_NONE;
    }
  }
}

void jit_reg_state_discard_currents(jit_reg_state* st) {
  jit_reg_discard_count++;
  jit_reg_state_clobber_call(st);
}

void jit_reg_survive_hold(jit_reg_state* st, uint8_t vreg) {
  assert(!st->survive_held &&
         "jit_reg_survive_hold: already held; only one slot may be held at a time");
  jit_spill_load_gpr64(st->emit.buf, (unsigned)JIT_REG_SURVIVE, vreg);
  st->survive_held = 1;
  st->survive_vreg = vreg;
}

void jit_reg_survive_release(jit_reg_state* st) {
  assert(st->survive_held && "jit_reg_survive_release: not held");
  st->survive_held = 0;
}

int jit_reg_survive_is_held(const jit_reg_state* st) { return st->survive_held; }

uint8_t jit_reg_survive_held_vreg(const jit_reg_state* st) { return st->survive_vreg; }

void jit_reg_unbind_preg(jit_reg_state* st, int preg) {
  if(preg_is_xmm(preg)) {
    int xi = preg_xmm_index(preg);
    if(xi >= 0 && xi < (int)jit_xmm_pool_len) {
      int16_t v = st->xmm_vreg[xi];
      st->xmm_vreg[xi] = JIT_PREG_NONE;
      if(v >= 0 && (uint32_t)v < st->nregs) st->current[v] = (int8_t)JIT_PREG_NONE;
    }
  } else {
    int gi = preg_to_gpr_pool_index(preg);
    if(gi >= 0) {
      int16_t v = st->gpr_vreg[gi];
      st->gpr_vreg[gi] = JIT_PREG_NONE;
      if(v >= 0 && (uint32_t)v < st->nregs) st->current[v] = (int8_t)JIT_PREG_NONE;
    }
  }
}

void jit_reg_unbind_vreg(jit_reg_state* st, uint8_t vreg) {
  if((uint32_t)vreg >= st->nregs) return;
  int preg = (int)st->current[vreg];
  if(preg == JIT_PREG_NONE) return;
  jit_reg_unbind_preg(st, preg);
}

void jit_reg_unpin_all(jit_reg_state* st) {
  memset(st->gpr_pinned, 0, sizeof st->gpr_pinned);
  memset(st->xmm_pinned, 0, sizeof st->xmm_pinned);
}

static void pin_gpr_index(jit_reg_state* st, unsigned i) {
  if(i < (unsigned)(sizeof st->gpr_pinned)) st->gpr_pinned[i] = 1;
}

static void pin_xmm_index(jit_reg_state* st, unsigned i) {
  if(i < (unsigned)(sizeof st->xmm_pinned)) st->xmm_pinned[i] = 1;
}

static void pin_preg(jit_reg_state* st, int preg) {
  if(preg_is_xmm(preg)) {
    int xi = preg_xmm_index(preg);
    if(xi >= 0) pin_xmm_index(st, (unsigned)xi);
  } else {
    int gi = preg_to_gpr_pool_index(preg);
    if(gi >= 0) pin_gpr_index(st, (unsigned)gi);
  }
}

static unsigned pick_unpinned_victim(const uint8_t* pinned, unsigned len, unsigned start) {
  unsigned i = start % len;
  for(unsigned n = 0; n < len; n++) {
    if(!pinned[i]) return i;
    i = (i + 1u) % len;
  }
  return start % len;
}

void jit_reg_bind(jit_reg_state* st, uint8_t vreg, int preg) {
  if((uint32_t)vreg >= st->nregs) return;
  jit_reg_unbind_vreg(st, vreg);
  jit_reg_unbind_preg(st, preg);
  st->current[vreg] = (int8_t)preg;
  if(preg_is_xmm(preg)) {
    int xi = preg_xmm_index(preg);
    if(xi >= 0 && xi < (int)jit_xmm_pool_len) st->xmm_vreg[xi] = vreg;
  } else {
    int gi = preg_to_gpr_pool_index(preg);
    if(gi >= 0) st->gpr_vreg[gi] = vreg;
  }
}

/** Binds `vreg` to a free pool slot, or else to one evicted round-robin. A free slot may still
 * be pinned; in `c = a - c`, allocating the destination unbinds c, but c's register is still
 * the subtrahend, and reusing it would let `mov dst, lhs` destroy it before `sub dst, rhs`
 * reads it. */
static int alloc_from_gpr_pool(jit_reg_state* st, uint8_t vreg, int and_load) {
  for(unsigned i = 0; i < jit_gpr_pool_len; i++) {
    if(st->gpr_vreg[i] == JIT_PREG_NONE && !st->gpr_pinned[i]) {
      int preg = (int)jit_gpr_pool[i];
      jit_reg_bind(st, vreg, preg);
      pin_gpr_index(st, i);
      if(and_load) {
        jit_spill_load_gpr64(st->emit.buf, (unsigned)preg, vreg);
        jit_reg_reload_count++;
      }
      return preg;
    }
  }
  {
    unsigned i = pick_unpinned_victim(st->gpr_pinned, jit_gpr_pool_len, st->next_gpr_victim);
    int16_t victim = st->gpr_vreg[i];
    if(victim != JIT_PREG_NONE) st->current[victim] = (int8_t)JIT_PREG_NONE;
    st->gpr_vreg[i] = JIT_PREG_NONE;
    int preg = (int)jit_gpr_pool[i];
    jit_reg_bind(st, vreg, preg);
    pin_gpr_index(st, i);
    if(and_load) {
      jit_spill_load_gpr64(st->emit.buf, (unsigned)preg, vreg);
      jit_reg_reload_count++;
    }
    st->next_gpr_victim = (i + 1u) % jit_gpr_pool_len;
    return preg;
  }
}

static int alloc_from_xmm_pool(jit_reg_state* st, uint8_t vreg, int and_load, int wide) {
  for(unsigned i = 0; i < jit_xmm_pool_len; i++) {
    if(st->xmm_vreg[i] == JIT_PREG_NONE && !st->xmm_pinned[i]) {
      int preg = (int)JIT_PREG_XMM0 + (int)i;
      jit_reg_bind(st, vreg, preg);
      pin_xmm_index(st, i);
      if(and_load) {
        if(wide) jit_spill_load_xmm64(st->emit.buf, (unsigned)i, vreg);
        else jit_spill_load_xmm32(st->emit.buf, (unsigned)i, vreg);
        jit_reg_reload_count++;
      }
      return preg;
    }
  }
  {
    unsigned i = pick_unpinned_victim(st->xmm_pinned, jit_xmm_pool_len, st->next_xmm_victim);
    int16_t victim = st->xmm_vreg[i];
    if(victim != JIT_PREG_NONE) st->current[victim] = (int8_t)JIT_PREG_NONE;
    st->xmm_vreg[i] = JIT_PREG_NONE;
    int preg = (int)JIT_PREG_XMM0 + (int)i;
    jit_reg_bind(st, vreg, preg);
    pin_xmm_index(st, i);
    if(and_load) {
      if(wide) jit_spill_load_xmm64(st->emit.buf, (unsigned)i, vreg);
      else jit_spill_load_xmm32(st->emit.buf, (unsigned)i, vreg);
      jit_reg_reload_count++;
    }
    st->next_xmm_victim = (i + 1u) % jit_xmm_pool_len;
    return preg;
  }
}

int jit_reg_alloc_gpr(jit_reg_state* st, uint8_t vreg, int and_load) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  jit_reg_unbind_vreg(st, vreg);
  return alloc_from_gpr_pool(st, vreg, and_load);
}

int jit_reg_alloc_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  jit_reg_unbind_vreg(st, vreg);
  return alloc_from_xmm_pool(st, vreg, and_load, wide);
}

int jit_reg_get_gpr(jit_reg_state* st, uint8_t vreg, int and_load) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  // A held survive value is already in JIT_REG_SURVIVE; nothing is emitted.
  if(st->survive_held && st->survive_vreg == vreg) return (int)JIT_REG_SURVIVE;
  int preg = (int)st->current[vreg];
  if(preg != JIT_PREG_NONE && !preg_is_xmm(preg)) {
    pin_preg(st, preg);
    return preg;
  }
  return jit_reg_alloc_gpr(st, vreg, and_load);
}

int jit_reg_get_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  int preg = (int)st->current[vreg];
  if(preg != JIT_PREG_NONE && preg_is_xmm(preg)) {
    pin_preg(st, preg);
    return preg;
  }
  return jit_reg_alloc_xmm(st, vreg, and_load, wide);
}

void jit_reg_store_home_gpr32(jit_reg_state* st, uint8_t vreg, int preg) {
  if(preg_is_xmm(preg)) return;
  jit_spill_store_gpr32(st->emit.buf, vreg, (unsigned)preg);
  jit_reg_bind(st, vreg, preg);
}

void jit_reg_store_home_gpr64(jit_reg_state* st, uint8_t vreg, int preg) {
  if(preg_is_xmm(preg)) return;
  jit_spill_store_gpr64(st->emit.buf, vreg, (unsigned)preg);
  jit_reg_bind(st, vreg, preg);
}

void jit_reg_store_home_xmm32(jit_reg_state* st, uint8_t vreg, int preg) {
  if(!preg_is_xmm(preg)) return;
  jit_spill_store_xmm32(st->emit.buf, vreg, (unsigned)preg_xmm_index(preg));
  jit_reg_bind(st, vreg, preg);
}

void jit_reg_store_home_xmm64(jit_reg_state* st, uint8_t vreg, int preg) {
  if(!preg_is_xmm(preg)) return;
  jit_spill_store_xmm64(st->emit.buf, vreg, (unsigned)preg_xmm_index(preg));
  jit_reg_bind(st, vreg, preg);
}
