// SPDX-License-Identifier: BSD-3-Clause
// Native stack switching and process slots, without the VM: ping-pong between the main stack and
// processes on pooled slots, callee-saved state across switches, and slot reuse. Built with -O2,
// so the values held across switches live in callee-saved registers.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <jello/internal/platform.h>
#include <jello/internal/proc_internal.h>

static void* main_sp;
static proc_pool pool;

typedef struct pinger {
  void* sp;
  uint64_t rounds;
  uint64_t seen;
  int done;
} pinger;

/* Adds through eight integers and eight doubles held across every switch, then ends by
 * switching away for good. */
static void ping(void* arg) {
  pinger* p = (pinger*)arg;
  uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
  double fa = 0.5, fb = 1.5, fc = 2.5, fd = 3.5, fe = 4.5, ff = 5.5, fg = 6.5, fh = 7.5;
  for(uint64_t i = 0; i < p->rounds; i++) {
    a += 1; b += 2; c += 3; d += 4; e += 5; f += 6; g += 7; h += 8;
    fa += 1; fb += 2; fc += 3; fd += 4; fe += 5; ff += 6; fg += 7; fh += 8;
    p->seen++;
    proc_stack_switch(&p->sp, main_sp);
  }
  uint64_t n = p->rounds;
  assert(a == 1 + n && b == 2 + 2 * n && c == 3 + 3 * n && d == 4 + 4 * n);
  assert(e == 5 + 5 * n && f == 6 + 6 * n && g == 7 + 7 * n && h == 8 + 8 * n);
  double x = (double)n;
  assert(fa == 0.5 + x && fb == 1.5 + 2 * x && fc == 2.5 + 3 * x && fd == 3.5 + 4 * x);
  assert(fe == 4.5 + 5 * x && ff == 5.5 + 6 * x && fg == 6.5 + 7 * x && fh == 7.5 + 8 * x);
  p->done = 1;
  proc_stack_switch(&p->sp, main_sp);
  assert(0 && "a finished process was resumed");
}

static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* The main stack holds its own callee-saved values across the same switches. */
static void test_ping_pong(void) {
  uint8_t* slot = proc_slot_take(&pool);
  assert(slot);
  pinger p = {0};
  p.rounds = 1000000;
  p.sp = proc_stack_prepare(slot, proc_slot_mid(&pool, slot), ping, &p);
  uint64_t a = 11, b = 12, c = 13, d = 14;
  double fa = 0.25, fb = 0.75;
  uint64_t trips = 0;
  double t0 = seconds();
  while(!p.done) {
    proc_stack_switch(&main_sp, p.sp);
    a ^= trips; b += a; c ^= b; d += c;
    fa += 1; fb += 2;
    trips++;
  }
  double t1 = seconds();
  uint64_t a2 = 11, b2 = 12, c2 = 13, d2 = 14;
  for(uint64_t i = 0; i < trips; i++) { a2 ^= i; b2 += a2; c2 ^= b2; d2 += c2; }
  assert(p.seen == p.rounds && a == a2 && b == b2 && c == c2 && d == d2);
  assert(fa == 0.25 + (double)trips && fb == 0.75 + 2 * (double)trips);
  proc_slot_give(&pool, slot);
  printf("  %.1f ns a switch\n", (t1 - t0) * 1e9 / (double)(2 * trips));
}

/* Interleaves several processes round robin, as a scheduler will. */
static void test_round_robin(void) {
  enum { N = 100 };
  pinger ps[N];
  uint8_t* slots[N];
  memset(ps, 0, sizeof ps);
  for(int i = 0; i < N; i++) {
    slots[i] = proc_slot_take(&pool);
    assert(slots[i]);
    ps[i].rounds = (uint64_t)(i + 1);
    ps[i].sp = proc_stack_prepare(slots[i], proc_slot_mid(&pool, slots[i]), ping, &ps[i]);
  }
  for(int live = N; live;) {
    live = 0;
    for(int i = 0; i < N; i++) {
      if(ps[i].done) continue;
      proc_stack_switch(&main_sp, ps[i].sp);
      if(!ps[i].done) live++;
    }
  }
  for(int i = 0; i < N; i++) {
    assert(ps[i].seen == ps[i].rounds);
    proc_slot_give(&pool, slots[i]);
  }
}

typedef struct diver {
  void* sp;
  int depth;
  uint64_t sum;
} diver;

/* Recurses `depth` frames of more than a page each, so that on Windows each probes its way down
 * through the guard page; returns the sum of the depths, read back from every frame. */
static __attribute__((noinline)) uint64_t dive(int depth) {
  volatile uint8_t pad[8 * 1024];
  for(size_t i = 0; i < sizeof pad; i += 256) pad[i] = (uint8_t)depth;
  uint64_t below = depth > 1 ? dive(depth - 1) : 0;
  return below + pad[0] + pad[sizeof pad - 256];
}

static void dive_entry(void* arg) {
  diver* d = (diver*)arg;
  d->sum = dive(d->depth);
  proc_stack_switch(&d->sp, main_sp);
  assert(0 && "a finished process was resumed");
}

/* A process's native stack grows into the pages below its first: on a new slot, and further on
 * the same slot given back, whose pages stay as the first process left them. */
static void test_deep_native_stack(void) {
  for(int depth = 3; depth <= 6; depth += 3) {
    uint8_t* slot = proc_slot_take(&pool);
    assert(slot);
    diver d = {NULL, depth, 0};
    d.sp = proc_stack_prepare(slot, proc_slot_mid(&pool, slot), dive_entry, &d);
    proc_stack_switch(&main_sp, d.sp);
    assert(d.sum == (uint64_t)(depth * (depth + 1)));
    proc_slot_give(&pool, slot);
  }
}

/* More slots than a slab holds come from distinct, non-overlapping memory, whose ends can be
 * committed and written, and given-back slots are reused, last first, before another is carved. */
static void test_slots(void) {
  enum { N = 200 };
  size_t slot_bytes = (size_t)pool.frame_bytes + pool.native_bytes;
  uint8_t* slots[N];
  for(int i = 0; i < N; i++) {
    slots[i] = proc_slot_take(&pool);
    assert(slots[i]);
    assert(jello_pages_commit(slots[i], 16) && jello_pages_commit(slots[i] + slot_bytes - 16, 16));
    memset(slots[i], i & 0xFF, 16);
    memset(slots[i] + slot_bytes - 16, i & 0xFF, 16);
    for(int j = 0; j < i; j++) {
      assert(slots[i] + slot_bytes <= slots[j] || slots[j] + slot_bytes <= slots[i]);
    }
  }
  for(int i = 0; i < N; i++) proc_slot_give(&pool, slots[i]);
  for(int i = N; i-- > 0;) assert(proc_slot_take(&pool) == slots[i]);
  for(int i = 0; i < N; i++) proc_slot_give(&pool, slots[i]);
}

/* Native stacks of 96 KiB, as proc_vm_tests' small pool: test_deep_native_stack goes about
 * 50 KiB deep, and Windows will not grow a stack into its lowest 24 KiB or so, where it keeps its
 * guard pages and the room to move them. */
int main(void) {
  pool = proc_pool_make(96u * 1024u, 256u * 1024u);
  test_ping_pong();
  test_round_robin();
  test_deep_native_stack();
  test_slots();
  printf("proc_switch_tests: passed\n");
  return 0;
}
