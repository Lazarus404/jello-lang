// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Process slots carved from pooled slabs, and the first frame of a process's native stack. */

#include <jello/internal/platform.h>
#include <jello/internal/proc_internal.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** The slots a slab holds. */
#define PROC_SLAB_SLOTS 64u

/** Where a free slot's link lives: the top of its native stack, a page its process touches
 * first anyway. */
static uint8_t* free_link(const proc_pool* pool, uint8_t* slot) {
  return proc_slot_mid(pool, slot) - sizeof(uint8_t*);
}

/** The page holding `slot`'s mid, where both its stacks start. */
static uint8_t* stacks_page(const proc_pool* pool, uint8_t* slot) {
  return proc_slot_mid(pool, slot) - pool->guard_bytes / 2u;
}

/** A slab's mapping: its guard page, then its slots. */
static size_t slab_bytes(const proc_pool* pool) {
  return pool->guard_bytes + PROC_SLAB_SLOTS * ((size_t)pool->native_bytes + pool->frame_bytes);
}

proc_pool proc_pool_make(uint32_t native_bytes, uint32_t frame_bytes) {
  uint32_t page = (uint32_t)jello_page_size();
  proc_pool p = {
      native_bytes - page / 2u, frame_bytes + page / 2u, page, NULL, NULL, NULL, NULL, 0};
  return p;
}

uint8_t* proc_slot_take(proc_pool* pool) {
  uint8_t* slot = pool->free;
  if(slot) {
    memcpy(&pool->free, free_link(pool, slot), sizeof pool->free);
    return slot;
  }
  if(pool->fresh == pool->fresh_end) {
    size_t bytes = slab_bytes(pool);
    uint8_t** slabs = (uint8_t**)realloc(pool->slabs, sizeof(uint8_t*) * (pool->nslabs + 1u));
    if(!slabs) return NULL;
    pool->slabs = slabs;
    void* mem = jello_pages_reserve(bytes);
    if(!mem) return NULL;
    /* So that the first slot's native stack overflowing faults rather than writing into
     * whatever lies below. */
    if(!jello_pages_guard(mem, pool->guard_bytes)) {
      jello_pages_unmap(mem, bytes);
      return NULL;
    }
    pool->slabs[pool->nslabs++] = (uint8_t*)mem;
    pool->fresh = (uint8_t*)mem + pool->guard_bytes;
    pool->fresh_end = (uint8_t*)mem + bytes;
  }
  slot = pool->fresh;
  /* The page both stacks start in; later processes on the slot find its pages as earlier ones
   * left them. */
  if(!jello_pages_stack(stacks_page(pool, slot), pool->guard_bytes)) return NULL;
  pool->fresh += (size_t)pool->frame_bytes + pool->native_bytes;
  return slot;
}

void proc_slot_give(proc_pool* pool, uint8_t* slot) {
  memcpy(free_link(pool, slot), &pool->free, sizeof pool->free);
  pool->free = slot;
}

void proc_pool_free(proc_pool* pool) {
  for(uint32_t i = 0; i < pool->nslabs; i++) jello_pages_unmap(pool->slabs[i], slab_bytes(pool));
  free(pool->slabs);
  pool->slabs = NULL;
  pool->nslabs = 0;
  pool->free = pool->fresh = pool->fresh_end = NULL;
}

void proc_stack_start(void);

void* proc_stack_prepare(uint8_t* bottom, uint8_t* top, void (*entry)(void*), void* arg) {
  void (*start)(void) = proc_stack_start;
#if defined(_WIN32) && defined(__aarch64__)
  /* proc_stack_switch's frame: x19–x28, x29 and x30, d8–d15, then the thread's stack base,
   * limit and deallocation base, which the system checks a stack against. A zero x29 ends frame
   * walks. */
  uint64_t frame[24] = {0};
  memcpy(&frame[0], &entry, sizeof entry);
  memcpy(&frame[1], &arg, sizeof arg);
  memcpy(&frame[11], &start, sizeof start);
#elif defined(_WIN32) && defined(__x86_64__)
  /* proc_stack_switch's frame: xmm6–xmm15, the thread's stack base, limit and deallocation base,
   * which the system checks a stack against, then r15, r14, r13, r12, rsi, rdi, rbx, rbp and the
   * return address. A zero rbp ends frame walks. The frame ends at `top`, so rsp is 16-byte
   * aligned at proc_stack_start. */
  uint64_t frame[32] = {0};
  memcpy(&frame[25], &arg, sizeof arg);
  memcpy(&frame[26], &entry, sizeof entry);
  memcpy(&frame[31], &start, sizeof start);
#elif defined(__aarch64__)
  /* proc_stack_switch's frame: x19–x28, x29 and x30, then d8–d15. A zero x29 ends frame walks. */
  (void)bottom;
  uint64_t frame[20] = {0};
  memcpy(&frame[0], &entry, sizeof entry);
  memcpy(&frame[1], &arg, sizeof arg);
  memcpy(&frame[11], &start, sizeof start);
#elif defined(__x86_64__)
  /* proc_stack_switch's frame: r15, r14, r13, r12, rbx, rbp and the return address. A zero rbp
   * ends frame walks. The frame ends at `top`, so rsp is 16-byte aligned at proc_stack_start's
   * call. */
  (void)bottom;
  uint64_t frame[7] = {0};
  memcpy(&frame[2], &arg, sizeof arg);
  memcpy(&frame[3], &entry, sizeof entry);
  memcpy(&frame[6], &start, sizeof start);
#endif
#ifdef _WIN32
  /* The stack's limit claims no more than the page `top` lies in, which jello_pages_stack
   * commits; claiming more would let a large frame's probes skip the guard page. */
  uint8_t* limit = (uint8_t*)((uintptr_t)(top - 1) & ~(uintptr_t)(jello_page_size() - 1u));
  memcpy(&frame[20], &top, sizeof top);
  memcpy(&frame[21], &limit, sizeof limit);
  memcpy(&frame[22], &bottom, sizeof bottom);
#endif
  uint8_t* sp = top - sizeof frame;
  memcpy(sp, frame, sizeof frame);
  return sp;
}
