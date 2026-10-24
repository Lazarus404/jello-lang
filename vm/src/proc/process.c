// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Jello processes and their schedulers. A process is a VM on a pooled slot, sharing its parent's
 * heap, module, frame layouts and JIT code, running on its own native stack. JIT code calls only
 * above JIT_NATIVE_RESERVE from the stack's bottom, and the interpreter enters JIT code only
 * above PROC_JIT_ENTRY_MARGIN more, as on a thread's stack.
 *
 * A main process's processes run on N schedulers, each an OS thread: scheduler 0 is the main
 * process's own, and the only one to run it. A process runs until its window of reductions ends
 * with another waiting on its scheduler and none idle, then goes to the back of that one's run
 * queue, or until it parks to wait for a message, for another process to finish, or for a time.
 * A scheduler with nothing to run takes half of another's run queue.
 *
 * No lock is held across a switch. A process switching away is resumed by nobody until its stack
 * is saved: whatever runs next on its scheduler queues it, if it yielded or was woken while it
 * parked, or frees it, if it finished, since nothing can free the stack it is running on. A
 * process switched out leaves its VM quiescent, and its frames are complete at every safepoint
 * and native call. A collection reads no inbox, timer or reply, so it takes no scheduler's
 * lock. */

#include <jello/internal.h>
#include <jello/internal/platform.h>
#include <jello/internal/proc_internal.h>

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** Reductions each cleanup may use, after which it is stopped. */
#define PROC_CLEANUP_REDUCTIONS 100000u

/** Room above native_stack_limit an interpreted call needs to enter JIT code. */
#define PROC_JIT_ENTRY_MARGIN (16u * 1024u)

/** How many processes a scheduler runs in a row from its `next` before one from its run queue. */
#define PROC_NEXT_STREAK 16u

/** How long, in nanoseconds, a scheduler with nothing to run looks for work before it waits. */
#define PROC_SPIN_NS 20000u

#ifdef PROC_TSAN
void* __tsan_get_current_fiber(void);
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
#endif

uint64_t proc_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t proc_deadline(int32_t ms) { return ms < 0 ? 0 : proc_now() + (uint64_t)ms * 1000000u; }

static void lock(proc_sched* s) { pthread_mutex_lock(s->lock); }

static void unlock(proc_sched* s) { pthread_mutex_unlock(s->lock); }

static void idle_lock(proc_sched* s) { pthread_mutex_lock(&s->idle_lock); }

static void idle_unlock(proc_sched* s) { pthread_mutex_unlock(&s->idle_lock); }

/** Lets the CPU rest a moment in a busy wait. */
static void pause_briefly(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("isb" ::: "memory");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

/** Takes `w`'s run queue lock, spinning while another scheduler holds it, and giving up the CPU
 * now and then in case the holder's thread is not running. */
static void queue_lock(proc_worker* w) {
  uint32_t spins = 0;
  while(atomic_exchange_explicit(&w->mu, 1, memory_order_acquire)) {
    while(atomic_load_explicit(&w->mu, memory_order_relaxed)) {
      if(++spins % 64u == 0) sched_yield();
      else pause_briefly();
    }
  }
}

static void queue_unlock(proc_worker* w) { atomic_store_explicit(&w->mu, 0, memory_order_release); }

/** Waits on `cv`, with `mu` held, until signalled or until `due`, when it is not 0. */
static void wait_until(pthread_cond_t* cv, pthread_mutex_t* mu, uint64_t due) {
  if(!due) {
    pthread_cond_wait(cv, mu);
    return;
  }
  uint64_t now = proc_now();
  if(due <= now) return;
#if defined(__APPLE__) || defined(_WIN32)
  uint64_t ns = due - now;
  struct timespec rel = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
  pthread_cond_timedwait_relative_np(cv, mu, &rel);
#else
  struct timespec at = {(time_t)(due / 1000000000u), (long)(due % 1000000000u)};
  pthread_cond_timedwait(cv, mu, &at);
#endif
}

/** The scheduler the calling thread is, when it is one, and the schedulers it is one of. */
static _Thread_local proc_worker* this_worker;
static _Thread_local const proc_sched* this_sched;

/** Records the calling thread as scheduler `w`. */
static void be_worker(proc_worker* w) {
  this_worker = w;
  this_sched = w->sched;
}

/** The scheduler of `s` whose run queue gets `p` from the calling thread: scheduler 0, the only
 * one to run it, for a pinned process; else the calling scheduler, or, from a thread that is none
 * of `s`'s, the one that last ran `p`, or scheduler 0 when none has. */
static proc_worker* home(proc_sched* s, const jello_process* p) {
  if(p->pinned) return s->workers;
  return this_sched == s ? this_worker : p->worker;
}

/** Whether any scheduler's run queue holds a process that is not pinned. Read after
 * announcing a wait, it sees every process queued before the queuer could see the announcement. A
 * `next` is left out: only its scheduler runs it, and only that one, busy, has one. */
static int any_ready(const proc_sched* s) {
  for(uint32_t i = 0; i < s->nworkers; i++) {
    if(atomic_load(&s->workers[i].len)) return 1;
  }
  return 0;
}

/** Signals `cv` under idle_lock, so that a scheduler that has just decided to wait on it is
 * already waiting. */
static void wake_one(proc_sched* s, pthread_cond_t* cv) {
  idle_lock(s);
  pthread_cond_signal(cv);
  idle_unlock(s);
}

/** Ends `w`'s wait for work, or its next, if it has yet to wait. */
static void unpark_worker(proc_worker* w) {
  pthread_mutex_lock(&w->park_mu);
  w->woken = 1;
  pthread_cond_signal(&w->park_cv);
  pthread_mutex_unlock(&w->park_mu);
}

/** Wakes a scheduler waiting for work, taking its bit in `idle_mask`, if any waits. */
static void wake_idle(proc_sched* s) {
  for(uint32_t i = 0; i < PROC_IDLE_WORDS; i++) {
    uint64_t bits = atomic_load_explicit(&s->idle_mask[i], memory_order_relaxed);
    while(bits) {
      uint64_t bit = bits & (~bits + 1u);
      uint64_t was = atomic_fetch_and(&s->idle_mask[i], ~bit);
      if(was & bit) {
        unpark_worker(&s->workers[i * 64u + (uint32_t)__builtin_ctzll(bit)]);
        return;
      }
      bits = was & ~bit;
    }
  }
}

/** Appends `first` to `last`, a chain of `n` processes through `queued`, to `w`'s run queue,
 * with its `mu` held; `n` counts pinned processes out. */
static void queue_append(proc_worker* w, jello_process* first, jello_process* last, uint32_t n) {
  last->queued = NULL;
  if(w->tail) w->tail->queued = first;
  else w->head = first;
  w->tail = last;
  if(n) atomic_fetch_add(&w->len, n);
}

/** Puts `p`, which is neither running nor queued, its stack saved, at the back of its home's run
 * queue, then wakes a scheduler waiting for work, if one is and none is looking. */
static void make_ready(proc_sched* s, jello_process* p) {
  proc_worker* w = home(s, p);
  queue_lock(w);
  queue_append(w, p, p, !p->pinned);
  if(p->pinned) atomic_fetch_add(&s->pinned_ready, 1);
  queue_unlock(w);
  if(!p->pinned && atomic_load(&s->idle) && !atomic_load(&s->spinning)) wake_idle(s);
  else if(atomic_exchange(&s->main_waiting, 0)) wake_one(s, &s->main_cv);
}

/** Makes `p`, which the calling thread's process has just spawned or woken, ready to run as soon
 * as that process parks: in the calling scheduler's `next`, queuing only the one it displaces, so
 * that a message passed on runs on the same thread without waking another. Queues `p` instead
 * from a thread that is no scheduler, and with one scheduler, to keep its first-in, first-out
 * order, and for a pinned process, which may have to cross to scheduler 0. */
static void make_ready_next(proc_sched* s, jello_process* p) {
  if(this_sched != s || s->nworkers == 1 || p->pinned) {
    make_ready(s, p);
    return;
  }
  jello_process* last = this_worker->next;
  this_worker->next = p;
  if(last) make_ready(s, last);
}

/** Whether `w`'s run queue holds a process, a pinned one included. */
static int queue_waits(const proc_sched* s, proc_worker* w) {
  return atomic_load_explicit(&w->len, memory_order_relaxed) ||
         (w == s->workers && atomic_load_explicit(&s->pinned_ready, memory_order_relaxed));
}

/** Takes `w`'s `next`, unless it has run PROC_NEXT_STREAK in a row from there while its run queue
 * waited, else the first process off its run queue, a pinned one included; NULL when both are
 * empty. Runs only on `w`. */
static jello_process* take_own(proc_sched* s, proc_worker* w) {
  jello_process* p = w->next;
  int waits = queue_waits(s, w);
  if(p && (!waits || w->streak < PROC_NEXT_STREAK)) {
    w->next = NULL;
    w->streak = waits ? w->streak + 1u : 0;
    return p;
  }
  w->streak = 0;
  if(!waits) return NULL;
  queue_lock(w);
  p = w->head;
  if(p) {
    w->head = p->queued;
    if(!w->head) w->tail = NULL;
    if(p->pinned) atomic_fetch_sub_explicit(&s->pinned_ready, 1, memory_order_relaxed);
    else atomic_fetch_sub_explicit(&w->len, 1, memory_order_relaxed);
  }
  queue_unlock(w);
  if(p) return p;
  /* Its queue was taken meanwhile. */
  p = w->next;
  w->next = NULL;
  return p;
}

/** Takes the first half of the processes in another scheduler's run queue, pinned ones
 * aside, for `w`, trying each in turn from the next; returns the first and queues the rest on `w`,
 * or NULL when there are none. */
static jello_process* steal(proc_sched* s, proc_worker* w) {
  uint32_t n = s->nworkers;
  uint32_t self = (uint32_t)(w - s->workers);
  for(uint32_t i = 1; i < n; i++) {
    proc_worker* v = &s->workers[(self + i) % n];
    if(!atomic_load_explicit(&v->len, memory_order_relaxed)) continue;
    queue_lock(v);
    uint32_t want = (atomic_load_explicit(&v->len, memory_order_relaxed) + 1u) / 2u;
    jello_process* first = NULL;
    jello_process* last = NULL;
    jello_process* kept = NULL; /* The last pinned process passed over. */
    uint32_t got = 0;
    for(jello_process* p = v->head; p && got < want; p = kept ? kept->queued : v->head) {
      if(p->pinned) {
        kept = p;
        continue;
      }
      if(kept) kept->queued = p->queued;
      else v->head = p->queued;
      if(v->tail == p) v->tail = kept;
      if(last) last->queued = p;
      else first = p;
      last = p;
      got++;
    }
    if(got) atomic_fetch_sub_explicit(&v->len, got, memory_order_relaxed);
    queue_unlock(v);
    if(!got) continue;
    if(got > 1) {
      queue_lock(w);
      queue_append(w, first->queued, last, got - 1u);
      queue_unlock(w);
    }
    first->queued = NULL;
    return first;
  }
  return NULL;
}

/** The process scheduler `w` should run next: the first in its own run queue, else the first
 * it can steal; NULL when there is none. */
static jello_process* take(proc_sched* s, proc_worker* w) {
  jello_process* p = take_own(s, w);
  return p ? p : steal(s, w);
}

/** Puts heap entry `t` at index `i`. */
static void timer_place(proc_sched* s, proc_timer* t, uint32_t i) {
  s->timers[i] = t;
  t->at = i;
}

/** Restores the heap order around index `i`, whose entry may be out of place either way. */
static void timer_settle(proc_sched* s, uint32_t i) {
  proc_timer* t = s->timers[i];
  while(i > 0 && s->timers[(i - 1u) / 2u]->due > t->due) {
    timer_place(s, s->timers[(i - 1u) / 2u], i);
    i = (i - 1u) / 2u;
  }
  for(;;) {
    uint32_t c = 2u * i + 1u;
    if(c >= s->ntimers) break;
    if(c + 1u < s->ntimers && s->timers[c + 1u]->due < s->timers[c]->due) c++;
    if(s->timers[c]->due >= t->due) break;
    timer_place(s, s->timers[c], i);
    i = c;
  }
  timer_place(s, t, i);
}

/** Publishes the first timer's due time, for the schedulers to check without the lock. */
static void publish_next_due(proc_sched* s) {
  atomic_store_explicit(&s->next_due, s->ntimers ? s->timers[0]->due : 0, memory_order_relaxed);
}

/** Publishes the first timer's due time and wakes a waiting scheduler to wait for it. Each reads
 * the due time after announcing its wait, so one this misses sees the new one. */
static void timers_changed(proc_sched* s) {
  publish_next_due(s);
  atomic_thread_fence(memory_order_seq_cst);
  if(atomic_load(&s->idle)) wake_idle(s);
  idle_lock(s);
  if(atomic_load(&s->main_waiting)) pthread_cond_signal(&s->main_cv);
  idle_unlock(s);
}

static void timer_arm(proc_sched* s, proc_timer* t, uint64_t due) {
  if(s->ntimers == s->timers_cap) {
    s->timers_cap = s->timers_cap ? s->timers_cap * 2u : 16u;
    s->timers = (proc_timer**)realloc(s->timers, sizeof(proc_timer*) * s->timers_cap);
    if(!s->timers) abort();
  }
  t->due = due;
  timer_place(s, t, s->ntimers++);
  timer_settle(s, t->at);
  if(t->at == 0) timers_changed(s);
}

/** Takes `t`, which is armed, out of the heap. */
static void timer_disarm(proc_sched* s, proc_timer* t) {
  uint32_t i = t->at;
  t->at = PROC_TIMER_IDLE;
  proc_timer* last = s->timers[--s->ntimers];
  if(last != t) {
    timer_place(s, last, i);
    timer_settle(s, i);
  }
  if(i == 0) publish_next_due(s);
}

/** Ends `p`'s park, whatever its reason, with its `mu` held. Returns whether the caller is to
 * queue it, as unpark does. */
static int resume(jello_process* p) {
  p->parked = PROC_PARK_NONE;
  return !p->off;
}

/** Ends `p`'s park, if it is parked, with its `mu` held. Returns whether the caller is to queue it
 * (make_ready) once `mu` is released; while the switch away from it is not over, that switch
 * queues it instead. */
static int unpark(jello_process* p) {
  if(p->parked == PROC_PARK_NONE || p->parked == PROC_PARK_NATIVE) return 0;
  return resume(p);
}

/** Ends `p`'s park, if it is parked, taking its `mu`, and queues it. */
static void wake(proc_sched* s, jello_process* p) {
  pthread_mutex_lock(&p->mu);
  int queue = unpark(p);
  pthread_mutex_unlock(&p->mu);
  if(queue) make_ready_next(s, p);
}

static void live_unlink(proc_sched* s, jello_process* p) {
  if(p->live_prev) p->live_prev->live_next = p->live_next;
  else s->live = p->live_next;
  if(p->live_next) p->live_next->live_prev = p->live_prev;
}

static void join_unlink(jello_process* p) {
  jello_process* target = p->joining;
  if(p->join_prev) p->join_prev->join_next = p->join_next;
  else target->joiners = p->join_next;
  if(p->join_next) p->join_next->join_prev = p->join_prev;
  p->joining = NULL;
}

void proc_retain(jello_process* p) { atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed); }

/** Drops every message in `p`'s inbox into `dropped`, for the caller to free once it holds no
 * lock, as freeing runs handles' finalisers. */
static void inbox_free(jello_process* p, vm_parcel* dropped) {
  for(uint32_t i = 0; i < p->inbox_len; i++) vm_parcel_join(dropped, proc_inbox_at(p, i));
  free(p->inbox);
  p->inbox = NULL;
  p->inbox_len = 0;
  p->inbox_cap = 0;
}

void proc_release(jello_process* p) {
  if(atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) != 1) return;
  pthread_mutex_destroy(&p->mu);
  vm_parcel dropped = vm_parcel_empty();
  inbox_free(p, &dropped);
  vm_parcel_free(&dropped);
  vm_parcel_free(&p->exit);
  free(p);
}

/** Drops sending timer `t`, disarmed, from its target's list, with its message, into `dropped`,
 * as inbox_free does. */
static void timer_drop(proc_timer* t, vm_parcel* dropped) {
  if(t->prev) t->prev->next = t->next;
  else t->target->sends = t->next;
  if(t->next) t->next->prev = t->prev;
  vm_parcel_join(dropped, &t->msg);
  proc_timer_release(t);
}

void proc_timer_release(proc_timer* t) {
  if(atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) free(t);
}

/** Cancels `t` unless it has fired, its message into `dropped`, as inbox_free does; whether it had
 * yet to. */
static int timer_cancel(proc_sched* s, proc_timer* t, vm_parcel* dropped) {
  if(t->at == PROC_TIMER_IDLE) return 0;
  timer_disarm(s, t);
  timer_drop(t, dropped);
  return 1;
}

/** Adds `msg` to unfinished `p`'s inbox, taking it, with its `mu` held, ending its park if it
 * waits for one; whether the caller is to queue it, as unpark says. */
static int deliver(jello_process* p, vm_parcel* msg) {
  if(p->inbox_len == p->inbox_cap) {
    uint32_t cap = p->inbox_cap ? p->inbox_cap * 2u : 4u;
    vm_parcel* ring = (vm_parcel*)malloc(sizeof(vm_parcel) * cap);
    if(!ring) abort();
    for(uint32_t i = 0; i < p->inbox_len; i++) ring[i] = *proc_inbox_at(p, i);
    free(p->inbox);
    p->inbox = ring;
    p->inbox_cap = cap;
    p->inbox_head = 0;
  }
  vm_parcel_move(proc_inbox_at(p, p->inbox_len++), msg);
  return p->parked == PROC_PARK_RECEIVE && unpark(p);
}

/** Fires every timer due by `now`, with the lock held: wakes a parked process, or sends a
 * message. A process's deadline fires only while it waits until it, as it disarms it under the
 * lock once woken. */
static void fire_due(proc_sched* s, uint64_t now) {
  while(s->ntimers && s->timers[0]->due <= now) {
    proc_timer* t = s->timers[0];
    timer_disarm(s, t);
    jello_process* p = t->target;
    int deadline = t == &p->deadline;
    pthread_mutex_lock(&p->mu);
    int queue = deadline ? unpark(p) : deliver(p, &t->msg);
    pthread_mutex_unlock(&p->mu);
    if(queue) make_ready(s, p);
    vm_parcel delivered = vm_parcel_empty(); /* What timer_drop finds once deliver took it. */
    if(!deadline) timer_drop(t, &delivered);
  }
}

/** The time now when a timer is due by the time published without the lock, else 0. */
static uint64_t timers_due(const proc_sched* s) {
  uint64_t due = atomic_load_explicit(&s->next_due, memory_order_relaxed);
  if(!due) return 0;
  uint64_t now = proc_now();
  return due <= now ? now : 0;
}

/** fire_due, taking the lock. */
static void fire_at(proc_sched* s, uint64_t now) {
  lock(s);
  fire_due(s, now);
  unlock(s);
}

/** Fires the timers due, if any are. */
static void fire_if_due(proc_sched* s) {
  uint64_t now = timers_due(s);
  if(now) fire_at(s, now);
}

/** Frees the VM and slot of `p`, which has finished and no longer runs, dropping its scheduler's
 * reference. */
static void reap(proc_sched* s, jello_process* p) {
#ifdef PROC_TSAN
  __tsan_destroy_fiber(p->fiber);
#endif
  vm_destroy_attached(p->vm);
  p->vm = NULL;
  pthread_mutex_lock(&s->pool_lock);
  proc_slot_give(p->pool, p->slot);
  pthread_mutex_unlock(&s->pool_lock);
  proc_release(p);
}

/** Deals with the process scheduler `w` has just switched away from, now that its stack is
 * saved: queues it if it yielded, or if it was woken while it parked, and frees it if it has
 * finished. */
static void switched(proc_sched* s, proc_worker* w) {
  jello_process* p = w->prev;
  if(!p) return;
  w->prev = NULL;
  switch(w->left) {
    case PROC_LEFT_PARKED: {
      pthread_mutex_lock(&p->mu);
      p->off = 0;
      int woken = p->parked == PROC_PARK_NONE;
      pthread_mutex_unlock(&p->mu);
      if(woken) make_ready(s, p);
      break;
    }
    case PROC_LEFT_YIELDED: make_ready(s, p); break;
    default: reap(s, p); break;
  }
}

/** Whether scheduler `w` is 0, driven by a host's proc_run_for whose time is up. */
static int host_due(const proc_worker* w) { return w->run_until && proc_now() >= w->run_until; }

/** Runs `next`, or scheduler `w`'s own stack when it is NULL, in place of `me`, or of `w`'s own
 * stack when `me` is NULL. `me` leaves as `left` says, for whatever runs next to deal with, unless
 * it is PROC_LEFT_NONE. Resumes on whichever scheduler switches back. */
static void switch_to(proc_sched* s, proc_worker* w, jello_process* me, jello_process* next,
                      uint8_t left) {
  w->switches++;
  w->prev = left == PROC_LEFT_NONE ? NULL : me;
  w->left = left;
  if(next) next->worker = w;
#ifdef PROC_TSAN
  __tsan_switch_to_fiber(next ? next->fiber : w->fiber, 0);
#endif
  proc_stack_switch(me ? &me->sp : &w->idle_sp, next ? next->sp : w->idle_sp);
  switched(s, me ? me->worker : w);
}

/** Runs the process `p`'s scheduler should run next in place of `p`, which it leaves as `left`
 * says, or, with none, the main process's stack on scheduler 0, which resumes still waiting in
 * main_wait, or another scheduler's own stack. `p` is not the main process, and resumes once
 * something switches back to it. */
static void run_next(proc_sched* s, jello_process* p, uint8_t left) {
  proc_worker* w = p->worker;
  jello_process* next = host_due(w) ? NULL : take(s, w);
  if(!next && w == s->workers) {
    next = s->main;
    s->main_idle = 1;
  }
  switch_to(s, w, p, next, left);
}

/** Whether nothing could ever wake the main process, which waits for it: no process is queued,
 * every other scheduler waits for work, no timer is armed, no blocking call is under way and no
 * process waits on a file descriptor. A scheduler busy with a switch, a timer or
 * a process is not waiting. A call's end queues its process before it stops counting, so the
 * queues are looked at again after; a wait stops counting only once its process runs again. */
static int stalled(const proc_sched* s) {
  return atomic_load(&s->idle) + 1u == s->started &&
         !atomic_load_explicit(&s->next_due, memory_order_relaxed) && !atomic_load(&s->outside) &&
         !any_ready(s) && !atomic_load(&s->pinned_ready);
}

/** Scheduler 0's own loop while the main process is parked, or while its host drives it with
 * proc_run_for: fires timers, runs other processes and waits for them. Returns 0 once the main
 * process, woken, comes off the run queue, or, for a host, once its time is up or nothing is
 * ready; JELLO_TRAP_DEADLOCK when nothing could wake the main process. */
static jello_trap_code main_wait(proc_sched* s) {
  jello_process* m = s->main;
  proc_worker* w = s->workers;
  for(;;) {
    jello_process* next = host_due(w) ? NULL : take(s, w);
    if(next == m) return JELLO_TRAP_NONE;
    if(next) {
      switch_to(s, w, m, next, PROC_LEFT_NONE);
      /* Back with nothing to run, or because a scheduler took the main process to run it. */
      if(!s->main_idle) return JELLO_TRAP_NONE;
      s->main_idle = 0;
      continue;
    }
    uint64_t now = timers_due(s);
    if(now) {
      fire_at(s, now);
      continue;
    }
    if(w->run_until) return JELLO_TRAP_NONE;
    idle_lock(s);
    atomic_store(&s->main_waiting, 1);
    jello_trap_code trap = JELLO_TRAP_NONE;
    if(!atomic_load(&s->pinned_ready) && !any_ready(s)) {
      if(stalled(s)) trap = JELLO_TRAP_DEADLOCK;
      else
        wait_until(&s->main_cv, &s->idle_lock,
                   atomic_load_explicit(&s->next_due, memory_order_relaxed));
    }
    atomic_store(&s->main_waiting, 0);
    idle_unlock(s);
    if(trap != JELLO_TRAP_NONE) return trap;
  }
}

/** Parks the running process, `vm`'s, with its `mu` held, and the lock too when `locked`, for
 * reason `why`, until woken, releasing both. Returns 0, or the trap ending the wait:
 * JELLO_TRAP_STOPPED, or, for the main process, JELLO_TRAP_DEADLOCK when nothing else could run
 * and no timer could wake anything. */
static jello_trap_code park(proc_sched* s, jello_vm* vm, uint8_t why, int locked) {
  jello_process* p = vm->proc;
  int stopped = why != PROC_PARK_NATIVE && (p->stopping || p->cleaning);
  if(!stopped) {
    p->parked = why;
    p->off = p != s->main;
  }
  pthread_mutex_unlock(&p->mu);
  if(locked) unlock(s);
  if(stopped) return JELLO_TRAP_STOPPED;
  vm_go_quiescent(vm);
  jello_trap_code trap = JELLO_TRAP_NONE;
  if(p != s->main) {
    run_next(s, p, PROC_LEFT_PARKED);
  } else if((trap = main_wait(s)) != JELLO_TRAP_NONE) {
    pthread_mutex_lock(&p->mu);
    p->parked = PROC_PARK_NONE;
    pthread_mutex_unlock(&p->mu);
  }
  vm_leave_quiescent(vm);
  return p->stopping ? JELLO_TRAP_STOPPED : trap;
}

/** Takes `p` out of its owner's children, leaving it unowned. */
static void sibling_unlink(jello_process* p) {
  if(p->sibling_prev) p->sibling_prev->sibling_next = p->sibling_next;
  else p->owner->children = p->sibling_next;
  if(p->sibling_next) p->sibling_next->sibling_prev = p->sibling_prev;
  p->owner = NULL;
}

/** Asks `p` to stop for reason `why`, unless it has been asked already or has finished; whether
 * it had yet to finish. */
static int stop(proc_sched* s, jello_process* p, proc_exit why) {
  pthread_mutex_lock(&p->mu);
  int running = !p->done;
  int queue = 0;
  if(running) {
    if(!p->stopping) p->stopping = (uint8_t)why;
    queue = unpark(p);
  }
  pthread_mutex_unlock(&p->mu);
  if(queue) make_ready(s, p);
  return running;
}

/** Runs `p`'s cleanups on its VM, last registered first, each within its budget; a cleanup that
 * traps ends only itself. Frames left by a trap are unwound first, so that a cleanup's throw
 * cannot reach their handlers. Each stays registered, and so rooted, until all have run. */
static void run_cleanups(jello_process* p) {
  jello_vm* vm = p->vm;
  p->cleaning = 1;
  for(uint32_t i = p->ncleanups; i-- > 0;) {
    vm_frame_unwind(vm);
    p->cleanup_windows = 0;
    jello_numeric_result ignored;
    vm_exec_function(vm, (const jello_function*)jello_as_ptr(p->cleanups[i]), NULL, 0, &ignored);
  }
  vm_frame_unwind(vm);
  p->cleaning = 0;
  p->ncleanups = 0;
  free(p->cleanups);
  p->cleanups = NULL;
}

/** Records how `p` finished, from its VM, and runs its cleanups; then, quiescent, closes its
 * inbox and frees what it never received, closing the sockets that held; and only then marks it
 * finished, wakes whoever waits for it and stops the processes it owns. */
static void finish(proc_sched* s, jello_process* p, jello_exec_status st) {
  jello_vm* vm = p->vm;
  p->trap = st == JELLO_EXEC_OK ? JELLO_TRAP_NONE : vm->trap_code;
  p->exit_value = p->trap == JELLO_TRAP_UNCAUGHT_THROW ? vm->thrown : jello_make_null();
  if(p->ncleanups) run_cleanups(p);
  if(p->sockets) proc_sockets_end(p);
  /* Others read it once its globals are gone, so its prototypes are left for them to resolve. */
  vm_parcel_pack(vm, p->exit_value, 1, &p->exit);
  p->exit_value = jello_make_null();
  vm_go_quiescent(vm);
  vm_parcel dropped = vm_parcel_empty();
  lock(s);
  pthread_mutex_lock(&p->mu);
  inbox_free(p, &dropped);
  p->closed = 1;
  pthread_mutex_unlock(&p->mu);
  while(p->sends) timer_cancel(s, p->sends, &dropped);
  unlock(s);
  /* Freed without a lock, before a joiner can see it finished and expect its sockets closed. */
  vm_parcel_free(&dropped);
  lock(s);
  pthread_mutex_lock(&p->mu);
  atomic_store_explicit(&p->done, 1, memory_order_release);
  pthread_mutex_unlock(&p->mu);
  while(p->joiners) {
    jello_process* j = p->joiners;
    join_unlink(j);
    wake(s, j);
  }
  live_unlink(s, p);
  if(p->owner) sibling_unlink(p);
  for(jello_process* c = p->children; c; c = c->sibling_next) {
    c->owner = NULL;
    stop(s, c, PROC_PARENT_EXITED);
  }
  p->children = NULL;
  if(!s->live) {
    pthread_mutex_lock(&s->main->mu);
    int queue = s->main->parked == PROC_PARK_ALL && unpark(s->main);
    pthread_mutex_unlock(&s->main->mu);
    if(queue) make_ready(s, s->main);
  }
  unlock(s);
}

/** A process's first frame: runs its body, unless stopped first, then lets the next process run,
 * never to resume. */
static void proc_main(void* arg) {
  jello_process* p = (jello_process*)arg;
  proc_sched* s = p->sched;
  switched(s, p->worker);
  vm_leave_quiescent(p->vm);
  jello_exec_status st = JELLO_EXEC_TRAP;
  if(p->stopping) p->vm->trap_code = JELLO_TRAP_STOPPED;
  else st = vm_exec_function(p->vm, p->body, NULL, 0, &p->result);
  finish(s, p, st);
  run_next(s, p, PROC_LEFT_FINISHED);
  abort(); /* A finished process is never resumed. */
}

/** Looks for up to PROC_SPIN_NS for a process queued or a timer due, unless half the schedulers
 * not waiting already look; whether it saw one. The last to stop looking because it saw one wakes
 * a waiting scheduler to look in its place, as more may follow. */
static int spin(proc_sched* s) {
  if(2u * atomic_load(&s->spinning) >= s->nworkers - atomic_load(&s->idle)) return 0;
  atomic_fetch_add(&s->spinning, 1);
  int seen = 0;
  uint64_t until = proc_now() + PROC_SPIN_NS;
  for(uint32_t i = 1; !(seen = any_ready(s)); i++) {
    if(i % 32u == 0 && (timers_due(s) || proc_now() >= until)) break;
    pause_briefly();
  }
  if(atomic_fetch_sub(&s->spinning, 1) == 1 && seen && atomic_load(&s->idle)) wake_idle(s);
  return seen;
}

/** Waits, as scheduler `w`, other than 0, with nothing to run, until a process is queued, until
 * the first timer is due or until the schedulers shut down, unless one of those has happened
 * already. Announces the wait first, in `idle_mask` and then `idle`, and looks again after; tells
 * the main process, waiting, when nothing could ever wake it. */
static void park_worker(proc_sched* s, proc_worker* w) {
  uint32_t i = (uint32_t)(w - s->workers);
  uint64_t bit = 1ull << (i % 64u);
  _Atomic uint64_t* word = &s->idle_mask[i / 64u];
  atomic_fetch_or(word, bit);
  atomic_fetch_add(&s->idle, 1);
  uint64_t due = atomic_load(&s->next_due);
  if(!any_ready(s) && !atomic_load(&s->shutdown) && !(due && due <= proc_now())) {
    if(atomic_load(&s->main_waiting) && stalled(s)) wake_one(s, &s->main_cv);
    pthread_mutex_lock(&w->park_mu);
    if(!w->woken) wait_until(&w->park_cv, &w->park_mu, due);
    w->woken = 0;
    pthread_mutex_unlock(&w->park_mu);
  }
  atomic_fetch_and(word, ~bit);
  atomic_fetch_sub(&s->idle, 1);
}

/** Scheduler `w`'s own loop on a thread of its own: runs processes, fires timers and waits for
 * either, until the schedulers shut down. */
static void worker_loop(proc_worker* w) {
  proc_sched* s = w->sched;
  be_worker(w);
  for(;;) {
    jello_process* p = take(s, w);
    if(p) {
      switch_to(s, w, NULL, p, PROC_LEFT_NONE);
      continue;
    }
    uint64_t now = timers_due(s);
    if(now) {
      fire_at(s, now);
      continue;
    }
    if(spin(s)) continue;
    if(atomic_load(&s->shutdown)) return;
    park_worker(s, w);
  }
}

static void* worker_main(void* arg) {
  proc_worker* w = (proc_worker*)arg;
#ifdef PROC_TSAN
  w->fiber = __tsan_get_current_fiber();
#endif
  worker_loop(w);
  return NULL;
}

/** How many schedulers to run: JELLO_SCHEDULERS when set to a count, else one for each processor
 * the process may use. */
static uint32_t scheduler_count(void) {
  const char* env = getenv("JELLO_SCHEDULERS");
  char* end = NULL;
  long n = env && *env ? strtol(env, &end, 10) : 0;
  if(n < 1 || *end) n = jello_cpu_count();
  return n > (long)PROC_MAX_SCHEDULERS ? PROC_MAX_SCHEDULERS : (uint32_t)n;
}

/** Starts the threads of the schedulers other than 0, with idle_lock held; as many as can be. */
static void start_workers(proc_sched* s) {
  while(!s->start_failed && s->started < s->nworkers) {
    proc_worker* w = &s->workers[s->started];
    if(pthread_create(&w->thread, NULL, worker_main, w) != 0) {
      s->start_failed = 1;
      return;
    }
    atomic_fetch_add(&s->started, 1);
  }
}

/** A process record holding one reference; NULL when out of memory. */
static jello_process* record_new(void) {
  jello_process* p = (jello_process*)calloc(1, sizeof(jello_process));
  if(!p) return NULL;
  if(pthread_mutex_init(&p->mu, NULL) != 0) {
    free(p);
    return NULL;
  }
  atomic_init(&p->refs, 1);
  p->exit_value = jello_make_null();
  p->exit = vm_parcel_empty();
  p->deadline.at = PROC_TIMER_IDLE;
  p->deadline.target = p;
  p->deadline.msg = vm_parcel_empty();
  return p;
}

/** The scheduler `vm` started as a main process; NULL when it has none, or is not one. */
static proc_sched* main_sched(const jello_vm* vm) {
  proc_sched* s = vm->proc ? vm->proc->sched : NULL;
  return s && vm->proc == s->main ? s : NULL;
}

/** A scheduler for main process `m`, on `vm`; NULL when out of memory. */
static proc_sched* sched_new(jello_vm* vm, jello_process* m) {
  proc_sched* s = (proc_sched*)calloc(1, sizeof(proc_sched));
  uint32_t n = scheduler_count();
  proc_worker* workers =
      s ? (proc_worker*)jello_aligned_alloc(PROC_CACHE_LINE, n * sizeof(proc_worker)) : NULL;
  if(!workers) {
    free(s);
    return NULL;
  }
  s->lock = &vm->heap->proc_lock;
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
#if !defined(__APPLE__) && !defined(_WIN32)
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
  if(pthread_cond_init(&s->main_cv, &attr) != 0 || pthread_cond_init(&s->native_cv, NULL) != 0 ||
     pthread_mutex_init(&s->idle_lock, NULL) != 0 || pthread_mutex_init(&s->pool_lock, NULL) != 0 ||
     pthread_mutex_init(&s->native_lock, NULL) != 0 || pthread_mutex_init(&s->poll_lock, NULL) != 0)
    abort();
  memset(workers, 0, n * sizeof(proc_worker));
  s->workers = workers;
  s->nworkers = n;
  s->started = 1;
  for(uint32_t i = 0; i < n; i++) {
    workers[i].sched = s;
    if(pthread_mutex_init(&workers[i].park_mu, NULL) != 0 ||
       pthread_cond_init(&workers[i].park_cv, &attr) != 0)
      abort();
  }
  pthread_condattr_destroy(&attr);
  be_worker(workers);
  m->pinned = 1;
  s->pool = proc_pool_make(PROC_NATIVE_STACK_BYTES, PROC_FRAME_STACK_BYTES);
  s->main = m;
  s->epoch = proc_now();
  return s;
}

jello_process* proc_self(jello_vm* vm) {
  if(vm->proc) return vm->proc;
  jello_process* m = record_new();
  proc_sched* s = m ? sched_new(vm, m) : NULL;
  if(!s) {
    if(m) proc_release(m);
    return NULL;
  }
  m->vm = vm;
  m->sched = s;
  m->worker = s->workers;
#ifdef PROC_TSAN
  m->fiber = __tsan_get_current_fiber();
#endif
  vm->proc = m;
  return m;
}

/** Gives `pvm`, a new process's VM holding its body in its transient root, the module of `vm`,
 * which spawns it, and a copy of its globals, then copies the body and those globals into its
 * space as one graph, so that what they share stays shared, setting *sockets to those the body
 * takes. Returns the body's copy, or NULL when out of memory or, setting *trap, when the body
 * captures what cannot leave `vm`'s process. */
static const jello_function* copy_start(jello_vm* pvm, jello_vm* vm, jello_trap_code* trap,
                                        struct proc_socket_list** sockets) {
  const jello_bc_module* m = vm->running_module;
  pvm->running_module = m;
  if(m && m->nglobals) {
    pvm->globals = (uint64_t*)malloc(sizeof(uint64_t) * m->nglobals);
    if(!pvm->globals) return NULL;
    memcpy(pvm->globals, vm->globals, sizeof(uint64_t) * m->nglobals);
    pvm->owns_globals = 1;
  }
  *trap = vm_copy_start(vm, pvm->space, &pvm->gc_transient_root, pvm->globals, sockets);
  return *trap ? NULL : (const jello_function*)pvm->gc_transient_root;
}

jello_process* proc_spawn(jello_vm* vm, const jello_function* body, proc_pool* pool, unsigned flags,
                          jello_trap_code* trap) {
  *trap = JELLO_TRAP_NONE;
  jello_process* self = proc_self(vm);
  if(!self) return NULL;
  proc_sched* s = self->sched;
  if(!pool) pool = &s->pool;
  if(pool->native_bytes < JIT_NATIVE_RESERVE + PROC_JIT_ENTRY_MARGIN) return NULL;
  jello_process* p = record_new();
  uint8_t* slot = NULL;
  uint8_t* mid = NULL;
  if(p) {
    pthread_mutex_lock(&s->pool_lock);
    slot = proc_slot_take(pool);
    pthread_mutex_unlock(&s->pool_lock);
    if(slot) mid = proc_slot_mid(pool, slot);
  }
  /* Rooted here until copied, as a collection of this space may run while the new VM attaches,
   * and its root, in a space of its own, is no root of this one. */
  jello_gc_push_root(vm, (void*)body);
  jello_vm* pvm =
      slot ? vm_create_attached(vm, mid, pool->frame_bytes, VM_DEFAULT_CALL_FRAMES, 1) : NULL;
  if(pvm) pvm->gc_transient_root = (void*)body;
  struct proc_socket_list* sockets = NULL;
  if(pvm && !(body = copy_start(pvm, vm, trap, &sockets))) {
    vm_destroy_attached(pvm);
    pvm = NULL;
  }
  jello_gc_pop_roots(vm, 1);
  if(!pvm) {
    if(slot) {
      pthread_mutex_lock(&s->pool_lock);
      proc_slot_give(pool, slot);
      pthread_mutex_unlock(&s->pool_lock);
    }
    if(p) proc_release(p);
    return NULL;
  }
  pvm->active_jit = vm->active_jit;
  pvm->max_bytes_len = vm->max_bytes_len;
  vm_inherit_fuel(pvm, vm);
  pvm->native_stack_limit = (uintptr_t)slot + JIT_NATIVE_RESERVE;
  pvm->jit_stack_floor = pvm->native_stack_limit + PROC_JIT_ENTRY_MARGIN;
  vm_share_layouts(pvm, vm);
  pvm->proc = p;
  if(sockets) proc_sockets_take(p, &sockets);
  /* Counted as running from the start, so that only quiescence keeps a stop-the-world from
   * waiting for it: it may leave quiescence on any thread just as one begins. */
  vm_go_quiescent(pvm);
  atomic_store(&pvm->in_exec, 1);
  p->vm = pvm;
  p->sched = s;
  p->worker = s->workers;
  p->pool = pool;
  p->slot = slot;
  p->body = body;
  p->pinned = (flags & PROC_SPAWN_ON_MAIN) != 0;
  p->sp = proc_stack_prepare(slot, mid, proc_main, p);
#ifdef PROC_TSAN
  p->fiber = __tsan_create_fiber(0);
#endif
  lock(s);
  p->stopping = s->ending ? PROC_STOPPED : 0;
  if(!(flags & PROC_SPAWN_DETACHED)) {
    p->owner = self;
    p->sibling_next = self->children;
    if(self->children) self->children->sibling_prev = p;
    self->children = p;
  }
  p->live_next = s->live;
  if(s->live) s->live->live_prev = p;
  s->live = p;
  unlock(s);
  if(atomic_load_explicit(&s->started, memory_order_relaxed) < s->nworkers) {
    idle_lock(s);
    start_workers(s);
    idle_unlock(s);
  }
  proc_retain(p); /* The caller's, taken before it can run, finish and drop the scheduler's. */
  make_ready_next(s, p);
  return p;
}

int proc_stop(jello_process* p, proc_exit why) {
  return !proc_finished(p) && stop(p->sched, p, why);
}

/** Lets the first process in the run queue of `vm`'s scheduler run in place of `vm`'s, which goes
 * to the back of it; nothing happens when there is none. */
static void yield(proc_sched* s, jello_vm* vm) {
  jello_process* p = vm->proc;
  proc_worker* w = p->worker;
  jello_process* next = NULL;
  if(host_due(w)) {
    /* Gives scheduler 0's thread back to its host, to run on when it next calls. */
    next = s->main;
    s->main_idle = 1;
  } else if(!(next = take_own(s, w))) {
    return;
  }
  vm_go_quiescent(vm);
  switch_to(s, w, p, next, PROC_LEFT_YIELDED);
  vm_leave_quiescent(vm);
}

/** Whether a process waits that `p`'s scheduler should run instead: a pinned one, on
 * scheduler 0, its `next`, which no other scheduler takes, or one in its run queue while no other
 * scheduler is idle to take it. */
static int others_wait(const proc_sched* s, const jello_process* p) {
  proc_worker* w = p->worker;
  if(w == s->workers && atomic_load_explicit(&s->pinned_ready, memory_order_relaxed)) return 1;
  if(w->next) return 1;
  return atomic_load_explicit(&w->len, memory_order_relaxed) &&
         !atomic_load_explicit(&s->idle, memory_order_relaxed);
}

/** Fires due timers and lets waiting processes run, as the end of a window does; returns whether
 * `vm`'s process should stop. */
static int window_turn(jello_vm* vm, int may_yield) {
  jello_process* p = vm->proc;
  proc_sched* s = p->sched;
  fire_if_due(s);
  if(may_yield && (host_due(p->worker) || others_wait(s, p))) yield(s, vm);
  return p->stopping;
}

int proc_window_end(jello_vm* vm, int trapped) {
  jello_process* p = vm->proc;
  int stops = p->stopping;
  if(p->cleaning) stops = ++p->cleanup_windows >= PROC_CLEANUP_REDUCTIONS / VM_REDUCTIONS;
  if(window_turn(vm, !trapped && !stops) && !p->cleaning) stops = 1;
  if(!stops) return trapped;
  vm->trap_code = JELLO_TRAP_STOPPED;
  return -1;
}

int proc_run_for(jello_vm* vm, uint32_t ms) {
  proc_sched* s = main_sched(vm);
  if(!s || this_worker != s->workers) return 0;
  /* Called during a run, from a native, its processes run as the main process parks. */
  if(atomic_load(&vm->in_exec)) return 1;
  atomic_store(&vm->in_exec, 1);
  vm_safepoint_check(vm);
  vm_go_quiescent(vm);
  proc_worker* w = s->workers;
  /* Never 0, which would leave main_wait waiting for the main process. */
  w->run_until = proc_now() + (uint64_t)ms * 1000000u + 1u;
  main_wait(s);
  w->run_until = 0;
  proc_leave_to_host(vm);
  vm_leave_quiescent(vm);
  atomic_store(&vm->in_exec, 0);
  lock(s);
  int live = s->live != NULL;
  unlock(s);
  return live;
}

void proc_leave_to_host(jello_vm* vm) {
  proc_sched* s = main_sched(vm);
  if(!s || this_worker != s->workers) return;
  jello_process* p = s->workers->next;
  s->workers->next = NULL;
  if(p) make_ready(s, p);
}

int proc_on_main(const jello_vm* vm) {
  const jello_process* p = vm->proc;
  return !p || p->worker == p->sched->workers;
}

jello_trap_code proc_yield(jello_vm* vm) {
  jello_process* p = vm->proc;
  if(!p) return JELLO_TRAP_NONE;
  return window_turn(vm, 1) && !p->cleaning ? JELLO_TRAP_STOPPED : JELLO_TRAP_NONE;
}

/** Whether `deadline`, when it is not 0, has passed. */
static int expired(uint64_t deadline) { return deadline && proc_now() >= deadline; }

/** The process `vm` runs as, which must exist. */
static jello_process* running(jello_vm* vm) {
  jello_process* p = proc_self(vm);
  if(!p) abort();
  return p;
}

int proc_send(jello_process* from, jello_process* p, vm_parcel* msg) {
  pthread_mutex_lock(&p->mu);
  int sent = !p->closed && !p->done;
  int queue = sent && deliver(p, msg);
  pthread_mutex_unlock(&p->mu);
  if(!sent && msg->sockets) proc_sockets_take(from, &msg->sockets);
  if(!sent) vm_parcel_free(msg);
  if(queue) make_ready_next(p->sched, p);
  return sent;
}

uint32_t proc_pending(jello_vm* vm) {
  jello_process* p = vm->proc;
  if(!p) return 0;
  pthread_mutex_lock(&p->mu);
  uint32_t n = p->inbox_len;
  pthread_mutex_unlock(&p->mu);
  return n;
}

/** Arms `p`'s deadline for `deadline`, when it is not 0, taking the lock. */
static void deadline_arm(proc_sched* s, jello_process* p, uint64_t deadline) {
  if(!deadline) return;
  lock(s);
  timer_arm(s, &p->deadline, deadline);
  unlock(s);
}

/** Disarms `p`'s deadline, with the lock held, if it has yet to fire. */
static void deadline_disarm(proc_sched* s, jello_process* p) {
  if(p->deadline.at != PROC_TIMER_IDLE) timer_disarm(s, &p->deadline);
}

/** Parks `vm`'s process for reason `why` until `deadline`, or, for a receive, until its inbox has
 * a message, or, for a wait on a file descriptor, until the poller finds it ready. A deadline is
 * checked under `mu`, so that its timer, which fires once it has passed, wakes the process only
 * once it has parked. */
static jello_trap_code park_until(proc_sched* s, jello_vm* vm, uint8_t why, uint64_t deadline) {
  jello_process* p = vm->proc;
  deadline_arm(s, p, deadline);
  pthread_mutex_lock(&p->mu);
  jello_trap_code trap = JELLO_TRAP_NONE;
  while(!trap && !(why == PROC_PARK_RECEIVE && p->inbox_len) &&
        !(why == PROC_PARK_POLL && p->polled) && !expired(deadline)) {
    trap = park(s, vm, why, 0);
    pthread_mutex_lock(&p->mu);
  }
  pthread_mutex_unlock(&p->mu);
  if(deadline) {
    lock(s);
    deadline_disarm(s, p);
    unlock(s);
  }
  return trap;
}

jello_trap_code proc_wait_message(jello_vm* vm, uint64_t deadline) {
  return park_until(running(vm)->sched, vm, PROC_PARK_RECEIVE, deadline);
}

jello_trap_code proc_sleep(jello_vm* vm, uint64_t deadline) {
  return park_until(running(vm)->sched, vm, PROC_PARK_SLEEP, deadline);
}

/** Parks `vm`'s process, with the lock held, until `p` has finished, until *done, when `done` is
 * not NULL, is set under the lock, or until `deadline`, when it is not 0. */
static jello_trap_code await(proc_sched* s, jello_vm* vm, jello_process* p, const uint8_t* done,
                             uint64_t deadline) {
  jello_process* self = vm->proc;
  if(deadline) timer_arm(s, &self->deadline, deadline);
  jello_trap_code trap = JELLO_TRAP_NONE;
  while(!proc_finished(p) && !(done && *done) && !expired(deadline)) {
    if(p == self) {
      trap = JELLO_TRAP_DEADLOCK;
      break;
    }
    self->joining = p;
    self->join_prev = NULL;
    self->join_next = p->joiners;
    if(p->joiners) p->joiners->join_prev = self;
    p->joiners = self;
    pthread_mutex_lock(&self->mu);
    trap = park(s, vm, PROC_PARK_JOIN, 1);
    lock(s);
    if(self->joining) join_unlink(self);
    if(trap) break;
  }
  deadline_disarm(s, self);
  return trap;
}

jello_trap_code proc_await(jello_vm* vm, jello_process* p, uint64_t deadline) {
  proc_sched* s = running(vm)->sched;
  lock(s);
  jello_trap_code trap = await(s, vm, p, NULL, deadline);
  unlock(s);
  return trap;
}

jello_trap_code proc_receive(jello_vm* vm, jello_value* out) {
  jello_process* p = running(vm);
  pthread_mutex_lock(&p->mu);
  while(!p->inbox_len) {
    jello_trap_code trap = park(p->sched, vm, PROC_PARK_RECEIVE, 0);
    if(trap) return trap;
    pthread_mutex_lock(&p->mu);
  }
  vm_parcel msg = *proc_inbox_at(p, 0);
  p->inbox_head = (p->inbox_head + 1u) & (p->inbox_cap - 1u);
  p->inbox_len--;
  pthread_mutex_unlock(&p->mu);
  *out = vm_parcel_take(vm, &msg);
  return JELLO_TRAP_NONE;
}

jello_trap_code proc_join(jello_vm* vm, jello_process* p) { return proc_await(vm, p, 0); }

proc_timer* proc_send_after(jello_vm* vm, uint64_t deadline, vm_parcel* msg) {
  jello_process* p = running(vm);
  proc_timer* t = (proc_timer*)malloc(sizeof(proc_timer));
  if(!t) return NULL;
  atomic_init(&t->refs, 2);
  t->target = p;
  vm_parcel_move(&t->msg, msg);
  t->prev = NULL;
  lock(p->sched);
  t->next = p->sends;
  if(p->sends) p->sends->prev = t;
  p->sends = t;
  timer_arm(p->sched, t, deadline);
  unlock(p->sched);
  return t;
}

int proc_timer_cancel(jello_vm* vm, proc_timer* t) {
  proc_sched* s = running(vm)->sched;
  vm_parcel dropped = vm_parcel_empty();
  lock(s);
  int cancelled = timer_cancel(s, t, &dropped);
  unlock(s);
  vm_parcel_free(&dropped);
  return cancelled;
}

void proc_on_exit(jello_vm* vm, jello_value fn) {
  jello_process* p = running(vm);
  p->cleanups = (jello_value*)realloc(p->cleanups, sizeof(jello_value) * (p->ncleanups + 1u));
  if(!p->cleanups) abort();
  p->cleanups[p->ncleanups++] = fn;
}

int proc_reply_send(jello_vm* vm, proc_reply* r, vm_parcel* value) {
  proc_sched* s = running(vm)->sched;
  lock(s);
  int first = !r->sent;
  if(first) {
    vm_parcel_move(&r->value, value);
    r->sent = 1;
    if(r->waiter) wake(s, r->waiter);
  }
  unlock(s);
  if(!first && value->sockets) proc_sockets_take(vm->proc, &value->sockets);
  if(!first) vm_parcel_free(value);
  return first;
}

jello_trap_code proc_reply_wait(jello_vm* vm, proc_reply* r, jello_process* p, uint64_t deadline,
                                int* outcome) {
  proc_sched* s = running(vm)->sched;
  lock(s);
  r->waiter = vm->proc;
  jello_trap_code trap = await(s, vm, p, &r->sent, deadline);
  r->waiter = NULL;
  *outcome = r->sent ? 1 : proc_finished(p) ? 0 : -1;
  unlock(s);
  return trap;
}

/** Ends `p`'s park for a blocking call, which has returned, and queues it, unless the switch away
 * from it is not over, which queues it then. */
static void native_done(proc_sched* s, jello_process* p) {
  pthread_mutex_lock(&p->mu);
  int queue = resume(p);
  pthread_mutex_unlock(&p->mu);
  if(queue) make_ready(s, p);
}

/** A thread of `s`'s blocking pool: makes the calls queued, in turn, until it shuts down. Nothing
 * of a call is read once its process is queued, as that may resume and return. */
static void* native_main(void* arg) {
  proc_sched* s = (proc_sched*)arg;
  pthread_mutex_lock(&s->native_lock);
  for(;;) {
    proc_native_call* c = s->native_head;
    if(c) {
      s->native_head = c->next;
      if(!s->native_head) s->native_tail = NULL;
      s->native_queued--;
      pthread_mutex_unlock(&s->native_lock);
      jello_process* p = c->p;
      c->fn(c->ctx);
      native_done(s, p);
      atomic_fetch_sub(&s->outside, 1);
      pthread_mutex_lock(&s->native_lock);
      continue;
    }
    if(s->native_shutdown) break;
    s->native_idle++;
    pthread_cond_wait(&s->native_cv, &s->native_lock);
    s->native_idle--;
  }
  pthread_mutex_unlock(&s->native_lock);
  return NULL;
}

/** Queues `c` for `s`'s blocking pool, starting a thread when none is idle to take it, unless the
 * pool has as many as it may. Returns 0, queueing nothing, when it has none and can start none. */
static int native_queue(proc_sched* s, proc_native_call* c) {
  pthread_mutex_lock(&s->native_lock);
  if(s->native_idle <= s->native_queued && s->native_nthreads < PROC_MAX_NATIVE_THREADS) {
    if(pthread_create(&s->native_threads[s->native_nthreads], NULL, native_main, s) == 0)
      s->native_nthreads++;
  }
  if(!s->native_nthreads) {
    pthread_mutex_unlock(&s->native_lock);
    return 0;
  }
  c->next = NULL;
  if(s->native_tail) s->native_tail->next = c;
  else s->native_head = c;
  s->native_tail = c;
  s->native_queued++;
  pthread_cond_signal(&s->native_cv);
  pthread_mutex_unlock(&s->native_lock);
  return 1;
}

/** Ends `s`'s blocking pool, whose calls have all returned, as every process has finished. */
static void natives_end(proc_sched* s) {
  pthread_mutex_lock(&s->native_lock);
  s->native_shutdown = 1;
  pthread_cond_broadcast(&s->native_cv);
  pthread_mutex_unlock(&s->native_lock);
  for(uint32_t i = 0; i < s->native_nthreads; i++) pthread_join(s->native_threads[i], NULL);
}

/** What a blocking call returns for process `p` once made: JELLO_TRAP_STOPPED when it has been
 * asked to stop, unless it is running its cleanups, which a stop does not end. */
static jello_trap_code native_end(const jello_process* p) {
  return p->stopping && !p->cleaning ? JELLO_TRAP_STOPPED : JELLO_TRAP_NONE;
}

/* The call is counted before its process parks, so that no scheduler finds a deadlock meanwhile,
 * and queued with the process's `mu` held, so that its end, whenever it comes, sees the park.
 * Unlike other parks, it is made for a process stopping or running its cleanups too, as it always
 * ends. */
jello_trap_code proc_call_blocking(jello_vm* vm, jdll_export_fn fn, jdlo_ctx* ctx) {
  jello_process* p = vm->proc;
  proc_sched* s = p ? p->sched : NULL;
  int here = !s;
  if(!here && p == s->main) {
    lock(s);
    here = !s->live;
    unlock(s);
  }
  if(here) {
    proc_call_quiescent(vm, fn, ctx);
    return s ? native_end(p) : JELLO_TRAP_NONE;
  }
  proc_native_call c = {fn, ctx, p, NULL};
  atomic_fetch_add(&s->outside, 1);
  pthread_mutex_lock(&p->mu);
  if(!native_queue(s, &c)) {
    pthread_mutex_unlock(&p->mu);
    atomic_fetch_sub(&s->outside, 1);
    proc_call_quiescent(vm, fn, ctx);
    return native_end(p);
  }
  if(park(s, vm, PROC_PARK_NATIVE, 0) == JELLO_TRAP_DEADLOCK) abort(); /* Not while it counts. */
  return native_end(p);
}

/** Hands `found`, PROC_FD_* bits, to `d`'s waiters, with its `mu` held: each waiter of a direction
 * found, or every waiter on PROC_FD_CLOSED, gives up its waits and is queued with what it found.
 * Readiness no one waits for is kept in the record. A waiter lives on meanwhile, as giving up its
 * wait itself takes `mu`. */
static void poll_signal(proc_sched* s, proc_poll_desc* d, uint32_t found) {
  for(uint32_t dir = 0; dir < 2; dir++) {
    jello_process* p = d->waiters[dir];
    uint32_t bit = 1u << dir;
    if(!(found & (bit | PROC_FD_CLOSED))) continue;
    if(!p) {
      d->ready |= (uint8_t)(found & bit);
      continue;
    }
    for(uint32_t i = 0; i < 2; i++)
      if(d->waiters[i] == p) d->waiters[i] = NULL;
    pthread_mutex_lock(&p->mu);
    p->polled |= (uint8_t)(found & (bit | PROC_FD_CLOSED));
    int queue = unpark(p);
    pthread_mutex_unlock(&p->mu);
    if(queue) make_ready(s, p);
  }
}

/** Arms `d`'s descriptor again, with `d`'s `mu` held, for the directions its waiters wait on,
 * where a report disarms a watch. */
static void poll_rearm(proc_sched* s, proc_poll_desc* d) {
  uint32_t events = (d->waiters[0] ? PROC_FD_READ : 0u) | (d->waiters[1] ? PROC_FD_WRITE : 0u);
  if(!d->always) proc_poller_rearm(&s->poller, d->fd, d, events);
}

/* A record closed meanwhile ignores its event; one opened again since takes it, a readiness that
 * is only a hint. */
void proc_poll_ready(proc_sched* s, proc_poll_event e) {
  proc_poll_desc* d = (proc_poll_desc*)e.token;
  pthread_mutex_lock(&d->mu);
  if(d->open) {
    poll_signal(s, d, e.ready);
    poll_rearm(s, d);
  }
  pthread_mutex_unlock(&d->mu);
}

/** Stops watching `fd`, `d`'s descriptor, with `d`'s `mu` held, waking its waiters closed. */
static void poll_unwatch(proc_sched* s, proc_poll_desc* d, int fd) {
  if(!d->always) proc_poller_unwatch(&s->poller, fd);
  poll_signal(s, d, PROC_FD_CLOSED);
  d->open = 0;
}

jello_trap_code proc_poll_open(jello_vm* vm, int fd, int* watched) {
  proc_sched* s = running(vm)->sched;
  *watched = 0;
  if(proc_poll_index(fd) < 0) return JELLO_TRAP_TYPE_MISMATCH;
  proc_poll_desc* d = proc_polls_make(s, fd);
  if(!d) return JELLO_TRAP_NONE;
  pthread_mutex_lock(&d->mu);
  if(d->open) poll_unwatch(s, d, fd);
  int w = proc_poller_watch(&s->poller, fd, d);
  if(w >= 0) {
    d->open = 1;
    d->always = w == 1;
    d->ready = 0;
    d->fd = fd;
    *watched = 1;
  }
  pthread_mutex_unlock(&d->mu);
  return JELLO_TRAP_NONE;
}

/* A wait takes no lock but its record's and its process's, and makes no system call but, on
 * Windows, the one that arms its watch as it parks: the descriptor's readiness, which the poller's
 * thread records as it changes, either is there to take, or is handed to the process parked. The
 * wait is counted before it parks, so that no scheduler finds a deadlock meanwhile. */
jello_trap_code proc_poll_wait(jello_vm* vm, int fd, uint32_t events, uint64_t deadline,
                               uint32_t* ready) {
  jello_process* p = running(vm);
  proc_sched* s = p->sched;
  proc_poll_desc* d = proc_polls_find(s, fd);
  events &= PROC_FD_READ | PROC_FD_WRITE;
  *ready = 0;
  if(!d) return JELLO_TRAP_TYPE_MISMATCH;
  pthread_mutex_lock(&d->mu);
  int busy = !d->open || ((events & PROC_FD_READ) && d->waiters[0]) ||
             ((events & PROC_FD_WRITE) && d->waiters[1]);
  uint8_t got = busy ? 0 : d->always ? (uint8_t)events : d->ready & (uint8_t)events;
  if(busy || got || !events) {
    d->ready &= (uint8_t)~got;
    pthread_mutex_unlock(&d->mu);
    *ready = got;
    return busy ? JELLO_TRAP_TYPE_MISMATCH : JELLO_TRAP_NONE;
  }
  p->polled = 0;
  for(uint32_t dir = 0; dir < 2; dir++)
    if(events & (1u << dir)) d->waiters[dir] = p;
  poll_rearm(s, d);
  pthread_mutex_unlock(&d->mu);
  atomic_fetch_add(&s->outside, 1);
  jello_trap_code trap = park_until(s, vm, PROC_PARK_POLL, deadline);
  pthread_mutex_lock(&p->mu);
  got = p->polled;
  pthread_mutex_unlock(&p->mu);
  if(!got) { /* Not handed anything, so still a waiter, unless handed something since. */
    pthread_mutex_lock(&d->mu);
    for(uint32_t dir = 0; dir < 2; dir++)
      if(d->waiters[dir] == p) d->waiters[dir] = NULL;
    got = p->polled;
    pthread_mutex_unlock(&d->mu);
  }
  atomic_fetch_sub(&s->outside, 1);
  *ready = got;
  return trap;
}

jello_trap_code proc_poll_close(jello_vm* vm, int fd) {
  proc_sched* s = running(vm)->sched;
  proc_poll_desc* d = proc_polls_find(s, fd);
  if(!d) return JELLO_TRAP_TYPE_MISMATCH;
  pthread_mutex_lock(&d->mu);
  int open = d->open;
  if(open) poll_unwatch(s, d, fd);
  pthread_mutex_unlock(&d->mu);
  return open ? JELLO_TRAP_NONE : JELLO_TRAP_TYPE_MISMATCH;
}

void proc_wait_all(jello_vm* vm) {
  proc_sched* s = main_sched(vm);
  if(!s) return;
  lock(s);
  if(s->live) {
    pthread_mutex_lock(&s->main->mu);
    park(s, vm, PROC_PARK_ALL, 1);
  } else {
    unlock(s);
  }
}

/** Runs the main process's cleanups, leaving how its run ended as it was. */
static void main_cleanups(jello_vm* vm, jello_process* m) {
  jello_trap_code trap = vm->trap_code;
  uint32_t func_index = vm->trap_func_index;
  uint32_t pc = vm->trap_pc;
  jello_trap_trace trace = vm->trap_trace;
  run_cleanups(m);
  vm->trap_code = trap;
  vm->trap_func_index = func_index;
  vm->trap_pc = pc;
  vm->trap_trace = trace;
  vm->thrown = m->exit_value;
}

void proc_end_all(jello_vm* vm) {
  proc_sched* s = main_sched(vm);
  if(!s) return;
  jello_process* m = s->main;
  /* Rooted by the main process's thread while the others finish, for the host to report. */
  m->exit_value = vm->trap_code == JELLO_TRAP_UNCAUGHT_THROW ? vm->thrown : jello_make_null();
  if(m->ncleanups) main_cleanups(vm, m);
  if(m->sockets) proc_sockets_end(m);
  lock(s);
  s->ending = 1;
  vm_parcel dropped = vm_parcel_empty();
  while(m->sends) timer_cancel(s, m->sends, &dropped);
  for(jello_process* p = s->live; p; p = p->live_next) stop(s, p, PROC_STOPPED);
  unlock(s);
  proc_wait_all(vm);
  atomic_store(&s->shutdown, 1);
  for(uint32_t i = 1; i < s->started; i++) unpark_worker(&s->workers[i]);
  for(uint32_t i = 1; i < s->started; i++) pthread_join(s->workers[i].thread, NULL);
  natives_end(s);
  proc_polls_end(s);
  if(this_sched == s) this_sched = NULL;
  inbox_free(m, &dropped);
  vm_parcel_free(&dropped);
  m->vm = NULL;
  atomic_store_explicit(&m->done, 1, memory_order_release);
  proc_release(m);
  proc_pool_free(&s->pool);
  for(uint32_t i = 0; i < s->nworkers; i++) {
    pthread_mutex_destroy(&s->workers[i].park_mu);
    pthread_cond_destroy(&s->workers[i].park_cv);
  }
  pthread_cond_destroy(&s->main_cv);
  pthread_mutex_destroy(&s->idle_lock);
  pthread_mutex_destroy(&s->pool_lock);
  pthread_cond_destroy(&s->native_cv);
  pthread_mutex_destroy(&s->native_lock);
  pthread_mutex_destroy(&s->poll_lock);
  free(s->timers);
  jello_aligned_free(s->workers);
  vm->proc = NULL;
  free(s);
}
