// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Processes: switching native stacks, the pooled slots that hold a process's native stack and
 * frame stack, and the schedulers, one OS thread each, that run processes in turn. */

#ifndef JELLO_PROC_INTERNAL_H
#define JELLO_PROC_INTERNAL_H

#include <jello.h>
#include <jello/internal/tls.h>
#include <jello/internal/vm_internal.h>
#include <jello/jdll.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/** Defined when ThreadSanitizer instruments the build, which must be told of every switch. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PROC_TSAN 1
#endif
#endif

/** A process's default native stack and frame stack, each give or take half a page. */
#define PROC_NATIVE_STACK_BYTES (256u * 1024u)
#define PROC_FRAME_STACK_BYTES (256u * 1024u)
/** The most threads a program's blocking pool may have. */
#define PROC_MAX_NATIVE_THREADS 256u

/** A pool of same-sized process slots, carved from slabs it maps, one mapping a slab, each below
 * an inaccessible page. A slot is its native stack, growing down to the slot's start, then its
 * frame stack, growing up from there; the two meet mid-page, so a process that has not gone deep
 * touches a single page for both. Pages are committed as they are first touched, so an idle
 * process costs only those. A pool is not locked; its scheduler's pool_lock guards it. */
typedef struct proc_pool {
  uint32_t native_bytes; /**< The native stack's size; a multiple of 16. */
  uint32_t frame_bytes;  /**< The frame stack's size; a multiple of 16. */
  uint32_t guard_bytes;  /**< The inaccessible page below each slab. */
  uint8_t* free;         /**< Given-back slots, linked through a word at their native top. */
  uint8_t* fresh;        /**< The current slab's first slot never handed out. */
  uint8_t* fresh_end;    /**< The current slab's end. */
  uint8_t** slabs;       /**< Every slab it has mapped, for proc_pool_free. */
  uint32_t nslabs;
} proc_pool;

/** An empty pool of slots with a native stack of `native_bytes`, less half a page, and a frame
 * stack of `frame_bytes`, plus half a page; both are multiples of the page size. */
proc_pool proc_pool_make(uint32_t native_bytes, uint32_t frame_bytes);

/** A slot from `pool`, mapping a new slab when none is left; NULL when that mapping fails. */
uint8_t* proc_slot_take(proc_pool* pool);
/** Returns `slot` to `pool`. Its native stack must not be the one running. */
void proc_slot_give(proc_pool* pool, uint8_t* slot);
/** Unmaps every slab of `pool`, whose slots must all have been given back, and empties it. */
void proc_pool_free(proc_pool* pool);

/** Where `slot`'s two stacks meet: the top of its native stack, 16-byte aligned, where its first
 * frame goes, and the start of its frame stack. */
static inline uint8_t* proc_slot_mid(const proc_pool* pool, uint8_t* slot) {
  return slot + pool->native_bytes;
}

/** Saves the running native stack's callee-saved registers and stack pointer in *save, and
 * resumes the stack that `load`, a pointer a previous switch saved or proc_stack_prepare made,
 * describes. Returns when another switch loads *save. */
void proc_stack_switch(void** save, void* load);

/** Lays out the native stack from `bottom` to `top` so that switching to the result calls
 * `entry(arg)` there. `top` must be 16-byte aligned, and `entry` must never return. */
void* proc_stack_prepare(uint8_t* bottom, uint8_t* top, void (*entry)(void*), void* arg);

typedef struct proc_sched proc_sched;

/** How a process finished, as std.concurrent's Exit numbers it; PROC_RUNNING until it has. */
typedef enum proc_exit {
  PROC_RUNNING = -1,
  PROC_NORMAL = 0,        /**< Its body returned. */
  PROC_STOPPED = 1,       /**< It was stopped. */
  PROC_CRASHED = 2,       /**< It trapped; exit_value holds what an uncaught throw threw. */
  PROC_PARENT_EXITED = 3, /**< It was stopped because the process owning it finished. */
} proc_exit;

/** What a parked process waits for; PROC_PARK_NONE while it is running or queued. */
enum {
  PROC_PARK_NONE,
  PROC_PARK_RECEIVE,
  PROC_PARK_JOIN,
  PROC_PARK_ALL,
  PROC_PARK_SLEEP,
  /** A blocking export runs for it on the blocking pool, writing to its stack meanwhile, so
   * only that call's end wakes it. */
  PROC_PARK_NATIVE,
  PROC_PARK_POLL
};

/** Not in a scheduler's timer heap. */
#define PROC_TIMER_IDLE UINT32_MAX

/** A deadline in a scheduler's timer heap: the one a parked process waits until, embedded in
 * it, or one that sends a message to the process that armed it, which cancels it on finishing. */
typedef struct proc_timer {
  uint64_t due;                 /**< When it fires, in proc_now's nanoseconds. */
  uint32_t at;                  /**< Its index in the heap, or PROC_TIMER_IDLE. */
  _Atomic uint32_t refs;        /**< A sending timer's: the heap's while armed, and its handles'. */
  struct jello_process* target; /**< Who it wakes, or whom it sends to. */
  vm_parcel msg;                /**< What it sends; empty for a parked process's deadline. */
  struct proc_timer* prev;      /**< In its target's list of sends, while armed. */
  struct proc_timer* next;
} proc_timer;

/** What a scheduler does with the process it switched away from, once that one's stack is saved:
 * see proc_worker's `prev`. */
enum { PROC_LEFT_NONE, PROC_LEFT_PARKED, PROC_LEFT_YIELDED, PROC_LEFT_FINISHED };

/** A blocking export's call, on the stack of the process waiting for it, queued for the blocking
 * pool. */
typedef struct proc_native_call {
  jdll_export_fn fn;
  jdlo_ctx* ctx;
  struct jello_process* p;
  struct proc_native_call* next;
} proc_native_call;

/** What a wait on a file descriptor is for, and what it finds: ready to read or write, or the
 * descriptor no longer watched. */
enum { PROC_FD_READ = 1u, PROC_FD_WRITE = 2u, PROC_FD_CLOSED = 4u };

/** The most events the poller takes from the kernel at once. */
#define PROC_POLL_BATCH 64u

/** An I/O poller: a kqueue; an epoll instance and an eventfd to wake it; or on Windows a wepoll
 * port and a UDP socket, connected to itself, to wake it. */
typedef struct proc_poller {
#if defined(_WIN32)
  void* port;
  int wake;
#else
  int fd;
#if defined(__linux__)
  int wake;
#endif
#endif
} proc_poller;

/** An event the poller found: the token its file descriptor is watched with, or NULL for a wake,
 * and what the descriptor has become ready for, PROC_FD_* bits. */
typedef struct proc_poll_event {
  void* token;
  uint32_t ready;
} proc_poll_event;

/** Opens `pl`, its descriptors closed on exec. Returns 0, or -1 setting errno. */
int proc_poller_open(proc_poller* pl);
/** Watches `fd` until unwatched, for reading and writing, reporting `token`, which is not NULL,
 * each time it becomes ready for either, edge-triggered: once, until its reader or writer has
 * found it not ready again. Returns 0; 1 for a descriptor that is always ready, such as a regular
 * file, which is not watched; or -1 setting errno, watching nothing. On Windows `fd` is a socket,
 * and each report disarms the watch, which proc_poller_rearm arms again. */
int proc_poller_watch(proc_poller* pl, int fd, void* token);
/** Arms watched `fd` again for `events`, PROC_FD_* bits, reporting `token`, where a report
 * disarms a watch (Windows); elsewhere a watch stays armed, and this does nothing. */
#if defined(_WIN32)
void proc_poller_rearm(proc_poller* pl, int fd, void* token, uint32_t events);
#else
static inline void proc_poller_rearm(proc_poller* pl, int fd, void* token, uint32_t events) {}
#endif
/** Stops watching `fd`. */
void proc_poller_unwatch(proc_poller* pl, int fd);
/** Waits for at least one event, or a wake, and puts up to PROC_POLL_BATCH in `out`. Returns how
 * many; 0 when interrupted. A descriptor ready for both may be reported twice, once each. */
uint32_t proc_poller_wait(proc_poller* pl, proc_poll_event* out);
/** Ends a wait in proc_poller_wait, or the next one, with an event of token NULL. */
void proc_poller_wake(proc_poller* pl);
/** Closes `pl`. */
void proc_poller_close(proc_poller* pl);

/** File descriptors a chunk of the poller's table covers. */
#define PROC_POLL_CHUNK 1024u
/** The chunks the poller's table may have. */
#define PROC_POLL_CHUNKS 1024u
/** The descriptors the poller can watch. */
#define PROC_POLL_FDS (PROC_POLL_CHUNK * PROC_POLL_CHUNKS)

/** `fd`'s index in the poller's table, or -1 when it has none: the descriptor, or on Windows the
 * socket's handle over 4, as handles are multiples of 4. */
static inline int32_t proc_poll_index(int fd) {
#if defined(_WIN32)
  uint32_t at = (uint32_t)fd >> 2;
#else
  uint32_t at = (uint32_t)fd;
#endif
  return fd < 0 || at >= PROC_POLL_FDS ? -1 : (int32_t)at;
}

/** A file descriptor's record with the poller, at the descriptor's index in its table, so stable
 * that late events for one closed, or opened again, find it, and at worst report a readiness that
 * is a hint anyway. */
typedef struct proc_poll_desc {
  pthread_mutex_t mu; /**< Guards the rest; taken before any process's `mu`. */
  uint8_t open;       /**< Set from proc_poll_open until proc_poll_close. */
  uint8_t always;     /**< Set for a descriptor the poller cannot watch, as it is always ready. */
  uint8_t ready;      /**< What it became ready for, with no one waiting, since a wait last found
                       * it so. */
  int fd;             /**< The descriptor it is the record of, while `open`. */
  struct jello_process* waiters[2]; /**< The process waiting to read, and to write. */
} proc_poll_desc;

/** `fd`'s record with `s`'s poller, or NULL when its table has none for it. */
proc_poll_desc* proc_polls_find(proc_sched* s, int fd);
/** `fd`'s record with `s`'s poller, opening the poller, starting its thread and adding the
 * record's chunk as needed; NULL when it cannot. */
proc_poll_desc* proc_polls_make(proc_sched* s, int fd);
/** Ends `s`'s poller, as every process has finished, and frees its records. */
void proc_polls_end(proc_sched* s);
/** Hands what event `e` found to its record, from the poller's thread. */
void proc_poll_ready(proc_sched* s, proc_poll_event e);

/** The most schedulers a main process runs. */
#define PROC_MAX_SCHEDULERS 256u

/** Words of a bit for each scheduler. */
#define PROC_IDLE_WORDS (PROC_MAX_SCHEDULERS / 64u)

/** The size of a cache line, or of the pair of them hardware prefetches together. */
#define PROC_CACHE_LINE 128

/** A scheduler: an OS thread running processes in turn. Scheduler 0 is the main process's own
 * thread; the others are started with the first spawn. What other schedulers read, its run queue,
 * and what only it touches are on separate cache lines, and each scheduler on lines of its own. */
typedef struct proc_worker {
  _Alignas(PROC_CACHE_LINE) proc_sched* sched;
  _Atomic uint32_t mu;        /**< A spin lock guarding its run queue: a mutex would sleep in the
                               * kernel under the contention of schedulers taking from it. */
  struct jello_process* head; /**< Its run queue, first in first out, through `queued`; only
                               * scheduler 0's holds the pinned processes, which only it runs. */
  struct jello_process* tail;
  _Atomic uint32_t len; /**< How many are in its run queue, pinned ones aside. */
  /** A process its running process spawned or woke, to run before its run queue as soon as that
   * process parks; no other scheduler takes it, and only this one touches it. */
  _Alignas(PROC_CACHE_LINE) struct jello_process* next;
  uint32_t streak;    /**< How many it has run in a row from `next` while its run queue waited. */
  uint64_t run_until; /**< Only scheduler 0's: when the proc_run_for driving it is to return, or 0
                       * while none is. */
  void* idle_sp;      /**< Its thread's own stack while it runs a process; unused by scheduler 0. */
  struct jello_process* prev; /**< The process it has just switched away from, whose stack is
                               * saved only once the switch is over: whatever runs next on it
                               * then deals with that one as `left` says. */
  uint8_t left;               /**< A PROC_LEFT_* value, for `prev`. */
  uint64_t switches;          /**< Its switches from one stack to another, for tests. */
  pthread_mutex_t park_mu;    /**< Guards `woken`, for waiting on `park_cv`. */
  pthread_cond_t park_cv;     /**< Where it waits for work, unless it is scheduler 0. */
  uint8_t woken;              /**< Set by whatever took its bit in `idle_mask`, to end its wait. */
  pthread_t thread;
#ifdef PROC_TSAN
  void* fiber; /**< ThreadSanitizer's name for its thread's own stack. */
#endif
} proc_worker;

/** A Jello process: a VM on a slot of its own, or the main process, the VM that started the
 * schedulers, on its thread's own stacks, which only scheduler 0 runs. The record is reference
 * counted: its scheduler holds one reference until it has finished, and each handle to it one
 * more, so how it finished can be read for as long as a handle can. Its VM and slot are freed
 * soon after it finishes. Its links to other processes, joins and timers are guarded by its
 * scheduler's `lock`; its stack and scheduler are changed only by the scheduler switching to it,
 * and its run queue link under that queue's scheduler's `mu`. */
typedef struct jello_process {
  pthread_mutex_t mu;  /**< Guards its inbox, `parked`, `off`, `done` and writes to `stopping`. */
  struct jello_vm* vm; /**< NULL once its VM is freed; nothing runs before it finishes. */
  proc_sched* sched;   /**< Read only while it has yet to finish, as it may be freed after. */
  proc_worker* worker; /**< The scheduler running it, or the last to, or scheduler 0. */
  proc_pool* pool;
  uint8_t* slot; /**< NULL for the main process. */
  void* sp;      /**< Its native stack pointer while it is not running. */
#ifdef PROC_TSAN
  void* fiber; /**< ThreadSanitizer's name for its stack. */
#endif
  const struct jello_function* body; /**< What it runs, a root of its VM until it starts. */
  struct jello_process* queued;      /**< The next in its run queue. */
  struct jello_process* live_prev;   /**< In its scheduler's list of unfinished processes. */
  struct jello_process* live_next;
  struct jello_process* joiners; /**< Processes parked joining it, through join_next. */
  struct jello_process* joining; /**< The process it is parked joining. */
  struct jello_process* join_prev;
  struct jello_process* join_next;
  struct jello_process* owner;    /**< The process whose finishing stops it, unless detached. */
  struct jello_process* children; /**< The unfinished processes it owns, through sibling_next. */
  struct jello_process* sibling_prev;
  struct jello_process* sibling_next;
  vm_parcel* inbox; /**< Messages not yet received, a ring of inbox_cap, a power of two. */
  uint32_t inbox_head;
  uint32_t inbox_len;
  uint32_t inbox_cap;
  _Atomic uint32_t refs;
  uint8_t parked; /**< A PROC_PARK_* value; still set when woken because nothing could run. */
  uint8_t off;    /**< Set while it parks, until the switch away from it is over; woken
                   * meanwhile, it is queued once that switch is. */
  _Atomic uint8_t stopping; /**< Set under `mu` by proc_stop, to the first reason it was given. */
  _Atomic uint8_t done;     /**< Set, once it has finished, after how it did is recorded. */
  uint8_t closed;           /**< Set under `mu` as it finishes, once its inbox is freed; nothing is
                             * delivered to it after. */
  uint8_t cleaning; /**< Its own: set while its cleanups run, when a stop waits, a park traps. */
  uint8_t polled;   /**< What the file descriptor it waits on is ready for, PROC_FD_* bits. */
  uint8_t pinned;   /**< Run only by scheduler 0: the main process, and those spawned on main. */
  struct proc_socket* sockets; /**< The sockets it owns, closed as it finishes; see net.c. */
  proc_timer deadline;         /**< The deadline of its park, while it has one. */
  proc_timer* sends;           /**< Its armed sending timers. */
  jello_value* cleanups; /**< Its own: what onExit registered, run last first as it finishes. */
  uint32_t ncleanups;
  uint32_t cleanup_windows;    /**< Reduction windows the cleanup running has used. */
  jello_trap_code trap;        /**< Why it trapped, once it has finished. */
  jello_value exit_value;      /**< A crash's thrown value, or null, which its VM roots while it
                                * finishes; the main process's stays so, for its host. */
  vm_parcel exit;              /**< Another's exit_value, packed once it has finished; the main
                                * process's stays empty, so its handle reads null. */
  jello_numeric_result result; /**< Its body's result, once it has finished normally. */
} jello_process;

/** The processes of one main process and the schedulers running them, each process until its window
 * of reductions ends with another waiting, until it parks, or until it finishes. Every unfinished
 * process is running, queued or parked. Each scheduler has a run queue of its own, where the
 * processes it queues go, and a scheduler with none to run takes half of another's. Locks are taken
 * in this order: `lock`, the heap's proc_lock, for the rarer changes, or a poll record's `mu`; each
 * process's `mu`; `native_lock`, or a scheduler's `mu`, for its run queue, one at a time; and
 * `idle_lock` or a scheduler's `park_mu`. `poll_lock` is taken alone. None is ever held across a
 * switch, while allocating or while waiting for a collection. Atomic fields are also read without
 * their lock, as hints. A scheduler about to wait announces it, in `idle_mask` and `idle` or in
 * `main_waiting`, then looks at the run queues; one queueing a process does the reverse, so one of
 * them sees the other, and wakes a scheduler by taking its bit. One looking for work before it
 * waits, counted in `spinning`, spares queuers the wake, and announces its wait once it stops
 * looking. */
struct proc_sched {
  pthread_mutex_t* lock;     /**< Guards the process tree, joins, replies, timers and `ending`. */
  pthread_mutex_t idle_lock; /**< Guards scheduler 0's waiting for work, `start_failed` and
                              * changes to `main_waiting` and `started`. */
  pthread_cond_t main_cv;    /**< Where scheduler 0 waits while the main process is parked. */
  /** How many of the processes in scheduler 0's run queue only it runs, the main process among
   * them; its `len` leaves them out, as no other scheduler may take them. */
  _Atomic uint32_t pinned_ready;
  uint8_t main_idle; /**< Set, only on scheduler 0, when it switches back to the main process,
                      * parked, because it has nothing else to run. */
  _Atomic uint8_t main_waiting; /**< Whether scheduler 0 waits for work, the main process parked. */
  _Atomic uint8_t shutdown;     /**< Set once every process has finished, to end the threads. */
  uint8_t start_failed;         /**< Set once a scheduler's thread has failed to start. */
  _Atomic uint32_t idle;        /**< Schedulers other than 0 waiting for work. */
  /** A bit for each scheduler other than 0 that waits for work, set before it counts in `idle`;
   * whatever takes one wakes that scheduler. */
  _Atomic uint64_t idle_mask[PROC_IDLE_WORDS];
  _Atomic uint32_t spinning; /**< Schedulers other than 0 looking for work before they wait, while
                              * which no queuer wakes another. */
  _Atomic uint32_t started;  /**< Schedulers whose threads have started, 0 included. */
  uint32_t nworkers;
  proc_worker* workers; /**< Scheduler 0, then the others. */
  jello_process* main;  /**< The main process; never finishes while the schedulers run. */
  uint8_t ending;       /**< In proc_end_all, which stops processes spawned meanwhile too. */
  jello_process* live;  /**< The unfinished processes, the main process aside. */
  proc_timer** timers;  /**< A binary min-heap by due time. */
  uint32_t ntimers;
  uint32_t timers_cap;
  _Atomic uint64_t next_due; /**< The first timer's due time, or 0 when there is none. */
  uint64_t epoch;            /**< When it started, by proc_now. */

  /** Blocking exports' calls not yet done, queued or running, and waits on file descriptors not
   * yet given up: what something outside the program may end, so while any is, nothing has
   * deadlocked. On a cache line of its own, as every such wait changes it. */
  _Alignas(64) _Atomic uint32_t outside;
  /** Guards the blocking pool: its queue, threads and counts. */
  _Alignas(64) pthread_mutex_t native_lock;
  pthread_cond_t native_cv;      /**< Where its idle threads wait for a call. */
  proc_native_call* native_head; /**< Calls queued for it, first in first out. */
  proc_native_call* native_tail;
  uint32_t native_queued; /**< How many calls are queued. */
  uint32_t native_idle;   /**< Its threads waiting for a call. */
  uint32_t native_nthreads;
  pthread_t native_threads[PROC_MAX_NATIVE_THREADS];
  uint8_t native_shutdown; /**< Set once every process has finished, to end its threads. */

  pthread_mutex_t poll_lock; /**< Guards `polling` and adding chunks to the poller's table. */
  proc_poller poller;        /**< Opened, and its thread started, by the first proc_poll_open. */
  pthread_t poll_thread;
  uint8_t polling; /**< Set once the poller is open and its thread started. */
  /** The poller's records, a chunk of PROC_POLL_CHUNK descriptors each, added as needed. */
  proc_poll_desc* _Atomic poll_chunks[PROC_POLL_CHUNKS];

  proc_pool pool;            /**< Slots of the default sizes. */
  pthread_mutex_t pool_lock; /**< Guards every pool its processes' slots come from. */
};

/** How many times `s`'s schedulers have switched from one stack to another; for tests, once they
 * have stopped. */
static inline uint64_t proc_switches(const proc_sched* s) {
  uint64_t n = 0;
  for(uint32_t i = 0; i < s->nworkers; i++) n += s->workers[i].switches;
  return n;
}

/** Whether `p` has finished. */
static inline int proc_finished(const jello_process* p) {
  return atomic_load_explicit(&p->done, memory_order_acquire);
}

/** How `p` finished, from its trap once it has. */
static inline proc_exit proc_exit_of(const jello_process* p) {
  if(!proc_finished(p)) return PROC_RUNNING;
  if(p->trap == JELLO_TRAP_NONE) return PROC_NORMAL;
  if(p->trap != JELLO_TRAP_STOPPED) return PROC_CRASHED;
  return p->stopping == PROC_PARENT_EXITED ? PROC_PARENT_EXITED : PROC_STOPPED;
}

/** The `i`th message in `p`'s inbox, counting from the next to be received. */
static inline vm_parcel* proc_inbox_at(const jello_process* p, uint32_t i) {
  return &p->inbox[(p->inbox_head + i) & (p->inbox_cap - 1u)];
}

/** proc_spawn's flags: the new process is owned by nothing, or runs only on scheduler 0, the main
 * process's thread. */
#define PROC_SPAWN_DETACHED 1u
#define PROC_SPAWN_ON_MAIN 2u

/** Spawns a process running `body` with no arguments, on a slot from `pool`, or from the
 * scheduler's default pool when that is NULL. It shares `vm`'s heap, running module, layouts and
 * JIT code, and inherits its fuel. `vm` is the main process, which starts the schedulers' threads
 * on its first spawn, or a process. The new process waits at the back of its run queue, owned by
 * `vm`'s process unless `flags` has PROC_SPAWN_DETACHED, so stopped with PROC_PARENT_EXITED when
 * that finishes, and run only by scheduler 0 when it has PROC_SPAWN_ON_MAIN. Returns
 * it with a reference for the caller beside its scheduler's, or NULL when out of memory, when
 * `pool`'s native stacks are too small to run JIT code, or, setting *trap to
 * JELLO_TRAP_UNSENDABLE, when `body` captures what cannot leave `vm`'s process; *trap is
 * otherwise JELLO_TRAP_NONE. Every process must have finished, by proc_end_all, before the JIT
 * code it shares is freed. */
jello_process* proc_spawn(struct jello_vm* vm, const struct jello_function* body, proc_pool* pool,
                          unsigned flags, jello_trap_code* trap);
/** jello_run_for, for `vm`, a main process between runs, on its own thread. */
int proc_run_for(struct jello_vm* vm, uint32_t ms);
/** Gives scheduler 0's thread back to the host of `vm`, a main process whose run returns leaving
 * its processes running: the process waiting in its `next` joins its run queue instead, for
 * another scheduler to take while the host is busy. */
void proc_leave_to_host(struct jello_vm* vm);
/** Fails native call `c` with trap `code`. */
void proc_trap(struct jdlo_ctx* c, jello_trap_code code);
/** The payload of argument `index` of `c`, a handle of class `cls`; fails the call and returns NULL
 * for null, trapping NULL_DEREF, or for another value. */
void* proc_arg_handle(struct jdlo_ctx* c, int index, const struct jello_jdll_handle_class* cls);
/** The process making native call `c`; fails the call and returns NULL when out of memory. */
jello_process* proc_self_or_fail(struct jdlo_ctx* c);
/** The class of a socket's handle, which moves, rather than being shared, when a message or a new
 * process's body takes it; a new process's globals and an exit value hold one not theirs. */
extern const struct jello_jdll_handle_class proc_socket_class;
/** Moves the socket `payload` out of `vm`'s process, as a message or a new process's body takes
 * it, when that process owns it; the next other process to use it owns it then. */
void proc_socket_moved(void* payload, const struct jello_vm* vm);
/** Closes the sockets `p` owns, as it finishes. */
void proc_sockets_end(jello_process* p);
/** The `vm` library's socket exports, net_*, which net.c documents. */
void vm_net_tcp_listen(struct jdlo_ctx* c);
void vm_net_udp_bind(struct jdlo_ctx* c);
void vm_net_tcp_accept(struct jdlo_ctx* c);
void vm_net_tcp_connect(struct jdlo_ctx* c);
void vm_net_read(struct jdlo_ctx* c);
void vm_net_receive_from(struct jdlo_ctx* c);
void vm_net_write(struct jdlo_ctx* c);
void vm_net_send_to(struct jdlo_ctx* c);
void vm_net_close(struct jdlo_ctx* c);
void vm_net_local_port(struct jdlo_ctx* c);
void vm_net_peer_host(struct jdlo_ctx* c);
void vm_net_peer_port(struct jdlo_ctx* c);
void vm_net_error(struct jdlo_ctx* c);
void vm_net_error_text(struct jdlo_ctx* c);
/** The `vm` library's TLS export, net_tls_start, which net.c documents; the rest of TLS is in
 * tls.jdll (jello/internal/tls.h). */
void vm_net_tls_start(struct jdlo_ctx* c);

/** The `vm` library's image_run, which image.c documents. */
void vm_image_run(struct jdlo_ctx* c);
/** Whether `vm`'s process is running on scheduler 0, the main process's thread, as a VM that has
 * never been a process is. */
int proc_on_main(const struct jello_vm* vm);
/** The process `vm` runs as, making schedulers for it when `vm` is a main process without them,
 * as many as JELLO_SCHEDULERS says, else one a CPU; NULL when out of memory. */
jello_process* proc_self(struct jello_vm* vm);
void proc_retain(jello_process* p);
/** Drops a reference to `p`, freeing it with the last. */
void proc_release(jello_process* p);
/** Asks `p`, which is not the caller, to stop, for reason `why`, PROC_STOPPED or
 * PROC_PARENT_EXITED, unless it has been asked already: it traps JELLO_TRAP_STOPPED, which no
 * catch handler sees, at its next safepoint, waking if it is parked, or finishes so without
 * running when it has not started. Nothing happens once it has finished. Returns whether it had
 * yet to finish. */
int proc_stop(jello_process* p, proc_exit why);
/** Fires due timers and, unless another scheduler is idle to take them, lets the processes waiting
 * on its scheduler run, `vm`'s, which is running, at the back of its run queue. Returns 0, or
 * JELLO_TRAP_STOPPED once its process has been asked to stop. */
jello_trap_code proc_yield(struct jello_vm* vm);
/** Adds `msg` to `p`'s inbox, taking it, waking `p` if it is parked in proc_receive; nothing
 * happens once it has finished, but `msg` is freed, the sockets it moves given back to `from`,
 * its sender, or closed when that is NULL. Returns whether it was added. */
int proc_send(jello_process* from, jello_process* p, vm_parcel* msg);
/** How many messages wait in the inbox of `vm`'s process. */
uint32_t proc_pending(struct jello_vm* vm);
/** The next message in the inbox of `vm`'s process in *out, parking until one arrives. Returns 0,
 * or the trap ending the wait: JELLO_TRAP_STOPPED, or JELLO_TRAP_DEADLOCK when no process could
 * ever send one. */
jello_trap_code proc_receive(struct jello_vm* vm, jello_value* out);
/** The monotonic clock, in nanoseconds. */
uint64_t proc_now(void);
/** The deadline `ms` milliseconds from now; 0, for none, when `ms` is negative. */
uint64_t proc_deadline(int32_t ms);
/** Parks `vm`'s process until `p` has finished or until `deadline`, when it is not 0. Returns 0,
 * or the trap ending the wait, as proc_receive does. */
jello_trap_code proc_await(struct jello_vm* vm, jello_process* p, uint64_t deadline);
/** Parks `vm`'s process until a message is in its inbox or until `deadline`, when it is not 0.
 * Returns 0, or the trap ending the wait, as proc_receive does. */
jello_trap_code proc_wait_message(struct jello_vm* vm, uint64_t deadline);
/** Parks `vm`'s process until `deadline`. Returns 0, or JELLO_TRAP_STOPPED. */
jello_trap_code proc_sleep(struct jello_vm* vm, uint64_t deadline);
/** A timer sending `msg` to `vm`'s process at `deadline`, taking it, unless that has finished
 * first; it holds one reference for its handle. NULL, leaving `msg`, when out of memory. */
proc_timer* proc_send_after(struct jello_vm* vm, uint64_t deadline, vm_parcel* msg);
/** Cancels `t`, for `vm`'s process, unless it has fired, dropping its message; whether it had
 * yet to fire. */
int proc_timer_cancel(struct jello_vm* vm, proc_timer* t);
/** Drops a reference to sending timer `t`, freeing it with the last. */
void proc_timer_release(proc_timer* t);
/** Watches file descriptor `fd`, a non-blocking one, with the poller of `vm`'s process until
 * proc_poll_close, setting *watched, or clearing it when the system refuses. A descriptor watched
 * already, closed since without proc_poll_close, is watched afresh, its waiters woken closed.
 * Returns 0, or JELLO_TRAP_TYPE_MISMATCH for one out of range. */
jello_trap_code proc_poll_open(struct jello_vm* vm, int fd, int* watched);
/** Parks `vm`'s process until watched descriptor `fd` becomes ready for one of `events`, read and
 * write PROC_FD_* bits, unless it has since a wait last found it so, or until `deadline`, when it
 * is not 0, and puts what it found in *ready: 0 when the deadline passed first, PROC_FD_CLOSED
 * when it was closed meanwhile. Readiness is edge-triggered and a hint: the caller makes the call
 * that would block until it does, then waits, then tries again. Each direction has one waiter at a
 * time. Returns 0, JELLO_TRAP_STOPPED, or JELLO_TRAP_TYPE_MISMATCH for a descriptor not watched,
 * or a direction another process waits on, which takes nothing found for that one. */
jello_trap_code proc_poll_wait(struct jello_vm* vm, int fd, uint32_t events, uint64_t deadline,
                               uint32_t* ready);
/** Stops watching `fd`, before it is closed, waking whatever waits on it with PROC_FD_CLOSED.
 * Returns 0, or JELLO_TRAP_TYPE_MISMATCH when it is not watched. */
jello_trap_code proc_poll_close(struct jello_vm* vm, int fd);
/** Calls export `fn` with `ctx` for `vm`, one that may block: on a thread of the program's
 * blocking pool while its process parks, so that other processes run meanwhile, or on the calling
 * thread when `vm` runs no process, or is the main process and no other is unfinished. Returns
 * once the call has, with 0, or JELLO_TRAP_STOPPED when the process has been asked to stop and is
 * not running its cleanups. */
jello_trap_code proc_call_blocking(struct jello_vm* vm, jdll_export_fn fn, jdlo_ctx* ctx);

/** Calls export `fn` with `ctx` for `vm` on the calling thread, quiescent, so that a collection
 * need not wait for it. */
static inline void proc_call_quiescent(struct jello_vm* vm, jdll_export_fn fn, jdlo_ctx* ctx) {
  vm_go_quiescent(vm);
  fn(ctx);
  vm_leave_quiescent(vm);
}
/** Parks `vm`'s process until `p` has finished; proc_await without a deadline. */
jello_trap_code proc_join(struct jello_vm* vm, jello_process* p);
/** Registers `fn`, a function of no arguments, to run when `vm`'s process finishes, before the
 * cleanups registered earlier; one a cleanup registers never runs. */
void proc_on_exit(struct jello_vm* vm, jello_value fn);
/** Runs the other processes of main process `vm`'s scheduler until all of them have finished, or
 * until none of them can run, the main process waiting meanwhile. */
void proc_wait_all(struct jello_vm* vm);
/** Runs the cleanups of main process `vm`, then stops every other process of its scheduler, runs
 * them until they have finished, and frees the scheduler; records with handles live on. The
 * module they run must still be loaded. */
void proc_end_all(struct jello_vm* vm);

/** A one-shot reply: the value a process sends back to a caller waiting in proc_reply_wait.
 * Guarded by its scheduler's `lock`. */
typedef struct proc_reply {
  jdl_refs refs;   /**< The handles holding it, in any process. */
  vm_parcel value; /**< What was sent, once `sent`, for each holder to read a copy of. */
  uint8_t sent;
  jello_process* waiter; /**< The process waiting for it, while one does. */
} proc_reply;

/** Sends `value` to `r` for `vm`'s process, taking it, waking whoever waits for it; whether it
 * went, which it does only the first time, freeing `value` otherwise, its sockets given back. */
int proc_reply_send(struct jello_vm* vm, proc_reply* r, vm_parcel* value);
/** Parks `vm`'s process until something is sent to `r`, until `p`, which was asked for it, has
 * finished, or until `deadline`, when it is not 0. Sets *outcome to 1 when something was sent, 0
 * when `p` finished first, or -1 on time out, and returns 0, or the trap ending the wait. */
jello_trap_code proc_reply_wait(struct jello_vm* vm, proc_reply* r, jello_process* p,
                                uint64_t deadline, int* outcome);

/** The handle class for a process, whose payload is its record. */
extern const struct jello_jdll_handle_class proc_handle_class;
/** The handle class for a reply, whose payload is a proc_reply. */
extern const struct jello_jdll_handle_class proc_reply_class;

#endif /* JELLO_PROC_INTERNAL_H */
