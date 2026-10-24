// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The I/O poller: kqueue on macOS and the BSDs, epoll on Linux, wepoll on Windows, watching each
 * descriptor for reading and writing at once, edge-triggered, from when it is opened until it is
 * closed; its thread, and its table of records, one a descriptor. */

#include <jello/internal/proc_internal.h>
#include <jello/internal/sockets.h>

#include <errno.h>
#include <stdlib.h>

#if defined(_WIN32)
#include "../../third_party/wepoll-1.5.8/wepoll.h"
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#endif

#if defined(_WIN32)

/* wepoll's watches are level-triggered or one-shot. Each is one-shot here, armed at first for
 * both directions and then, by proc_poller_rearm, for those a process waits on, so that a socket
 * is reported once for each wait it may end, as an edge-triggered watch would. */

int proc_poller_open(proc_poller* pl) {
  pl->port = jello_sockets_start() ? epoll_create1(0) : NULL;
  if(!pl->port) {
    errno = EIO;
    return -1;
  }
  int wake = jello_sock_open(AF_INET, SOCK_DGRAM);
  struct sockaddr_in at = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t len = (socklen_t)sizeof at;
  struct epoll_event ev = {.events = EPOLLIN, .data = {.ptr = NULL}};
  if(wake < 0 || jello_bind(wake, (struct sockaddr*)&at, len) != 0 ||
     jello_getsockname(wake, (struct sockaddr*)&at, &len) != 0 ||
     jello_connect(wake, (struct sockaddr*)&at, len) != 0 || jello_sock_nonblocking(wake) != 0 ||
     epoll_ctl(pl->port, EPOLL_CTL_ADD, JELLO_SOCKET(wake), &ev) != 0) {
    int err = errno ? errno : EIO;
    if(wake >= 0) jello_sock_close(wake);
    epoll_close(pl->port);
    errno = err;
    return -1;
  }
  pl->wake = wake;
  return 0;
}

int proc_poller_watch(proc_poller* pl, int fd, void* token) {
  struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT,
                           .data = {.ptr = token}};
  if(epoll_ctl(pl->port, EPOLL_CTL_ADD, JELLO_SOCKET(fd), &ev) == 0) return 0;
  return -1;
}

void proc_poller_rearm(proc_poller* pl, int fd, void* token, uint32_t events) {
  struct epoll_event ev = {.events = EPOLLONESHOT, .data = {.ptr = token}};
  if(events & PROC_FD_READ) ev.events |= EPOLLIN | EPOLLRDHUP;
  if(events & PROC_FD_WRITE) ev.events |= EPOLLOUT;
  if(events) epoll_ctl(pl->port, EPOLL_CTL_MOD, JELLO_SOCKET(fd), &ev);
}

void proc_poller_unwatch(proc_poller* pl, int fd) {
  epoll_ctl(pl->port, EPOLL_CTL_DEL, JELLO_SOCKET(fd), NULL);
}

uint32_t proc_poller_wait(proc_poller* pl, proc_poll_event* out) {
  struct epoll_event evs[PROC_POLL_BATCH];
  int n = epoll_wait(pl->port, evs, (int)PROC_POLL_BATCH, -1);
  if(n < 0) return 0;
  for(int i = 0; i < n; i++) {
    uint32_t e = evs[i].events, ready = 0;
    if(e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) ready |= PROC_FD_READ;
    if(e & (EPOLLOUT | EPOLLHUP | EPOLLERR)) ready |= PROC_FD_WRITE;
    out[i] = (proc_poll_event){evs[i].data.ptr, ready};
  }
  return (uint32_t)n;
}

void proc_poller_wake(proc_poller* pl) {
  char one = 1;
  jello_send(pl->wake, &one, 1);
}

void proc_poller_close(proc_poller* pl) {
  jello_sock_close(pl->wake);
  epoll_close(pl->port);
}

#elif defined(__linux__)

int proc_poller_open(proc_poller* pl) {
  pl->fd = epoll_create1(EPOLL_CLOEXEC);
  if(pl->fd < 0) return -1;
  pl->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  struct epoll_event ev = {.events = EPOLLIN, .data = {.ptr = NULL}};
  if(pl->wake < 0 || epoll_ctl(pl->fd, EPOLL_CTL_ADD, pl->wake, &ev) != 0) {
    int err = errno;
    if(pl->wake >= 0) close(pl->wake);
    close(pl->fd);
    errno = err;
    return -1;
  }
  return 0;
}

int proc_poller_watch(proc_poller* pl, int fd, void* token) {
  struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                           .data = {.ptr = token}};
  if(epoll_ctl(pl->fd, EPOLL_CTL_ADD, fd, &ev) == 0) return 0;
  return errno == EPERM ? 1 : -1;
}

void proc_poller_unwatch(proc_poller* pl, int fd) { epoll_ctl(pl->fd, EPOLL_CTL_DEL, fd, NULL); }

uint32_t proc_poller_wait(proc_poller* pl, proc_poll_event* out) {
  struct epoll_event evs[PROC_POLL_BATCH];
  int n = epoll_wait(pl->fd, evs, (int)PROC_POLL_BATCH, -1);
  if(n < 0) return 0;
  for(int i = 0; i < n; i++) {
    uint32_t e = evs[i].events, ready = 0;
    if(e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) ready |= PROC_FD_READ;
    if(e & (EPOLLOUT | EPOLLHUP | EPOLLERR)) ready |= PROC_FD_WRITE;
    out[i] = (proc_poll_event){evs[i].data.ptr, ready};
  }
  return (uint32_t)n;
}

void proc_poller_wake(proc_poller* pl) {
  uint64_t one = 1;
  while(write(pl->wake, &one, sizeof one) < 0 && errno == EINTR) {}
}

void proc_poller_close(proc_poller* pl) {
  close(pl->wake);
  close(pl->fd);
}

#else

/** Applies change `flags` to `fd`'s filters for reading and writing, with `token`. Returns 0, or
 * -1 setting errno. */
static int change(proc_poller* pl, int fd, uint16_t flags, void* token) {
  static const int16_t filters[2] = {EVFILT_READ, EVFILT_WRITE};
  struct kevent evs[2];
  for(int i = 0; i < 2; i++) EV_SET(&evs[i], (uintptr_t)fd, filters[i], flags, 0, 0, token);
  return kevent(pl->fd, evs, 2, NULL, 0, NULL);
}

int proc_poller_open(proc_poller* pl) {
  pl->fd = kqueue();
  if(pl->fd < 0) return -1;
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if(fcntl(pl->fd, F_SETFD, FD_CLOEXEC) != 0 || kevent(pl->fd, &ev, 1, NULL, 0, NULL) != 0) {
    int err = errno;
    close(pl->fd);
    errno = err;
    return -1;
  }
  return 0;
}

/* A regular file's filters report it ready only short of its end, though a read there does not
 * block. A change failing part way is undone, so that nothing reports `token` once it has. */
int proc_poller_watch(proc_poller* pl, int fd, void* token) {
  struct stat st;
  if(fstat(fd, &st) != 0) return -1;
  if(S_ISREG(st.st_mode)) return 1;
  if(change(pl, fd, EV_ADD | EV_CLEAR, token) == 0) return 0;
  int err = errno;
  change(pl, fd, EV_DELETE, NULL);
  errno = err;
  return -1;
}

void proc_poller_unwatch(proc_poller* pl, int fd) { change(pl, fd, EV_DELETE, NULL); }

uint32_t proc_poller_wait(proc_poller* pl, proc_poll_event* out) {
  struct kevent evs[PROC_POLL_BATCH];
  int n = kevent(pl->fd, NULL, 0, evs, (int)PROC_POLL_BATCH, NULL);
  if(n < 0) return 0;
  for(int i = 0; i < n; i++) {
    uint32_t ready = evs[i].filter == EVFILT_READ ? PROC_FD_READ : PROC_FD_WRITE;
    out[i] = (proc_poll_event){evs[i].filter == EVFILT_USER ? NULL : evs[i].udata, ready};
  }
  return (uint32_t)n;
}

void proc_poller_wake(proc_poller* pl) {
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent(pl->fd, &ev, 1, NULL, 0, NULL);
}

void proc_poller_close(proc_poller* pl) { close(pl->fd); }

#endif

/** The poller's thread: waits for events and hands them to their records, until woken to end. */
static void* poll_main(void* arg) {
  proc_sched* s = (proc_sched*)arg;
  proc_poll_event evs[PROC_POLL_BATCH];
  for(;;) {
    uint32_t n = proc_poller_wait(&s->poller, evs);
    for(uint32_t i = 0; i < n; i++) {
      if(!evs[i].token) return NULL;
      proc_poll_ready(s, evs[i]);
    }
  }
}

/** Opens `s`'s poller and starts its thread, with poll_lock held, unless it has. Returns whether
 * it has now. */
static int poll_start(proc_sched* s) {
  if(s->polling) return 1;
  if(proc_poller_open(&s->poller) != 0) return 0;
  if(pthread_create(&s->poll_thread, NULL, poll_main, s) != 0) {
    proc_poller_close(&s->poller);
    return 0;
  }
  s->polling = 1;
  return 1;
}

proc_poll_desc* proc_polls_find(proc_sched* s, int fd) {
  int32_t at = proc_poll_index(fd);
  if(at < 0) return NULL;
  proc_poll_desc* chunk =
      atomic_load_explicit(&s->poll_chunks[(uint32_t)at / PROC_POLL_CHUNK], memory_order_acquire);
  return chunk ? &chunk[(uint32_t)at % PROC_POLL_CHUNK] : NULL;
}

proc_poll_desc* proc_polls_make(proc_sched* s, int fd) {
  proc_poll_desc* d = proc_polls_find(s, fd);
  int32_t at = proc_poll_index(fd);
  if(d || at < 0) return d;
  pthread_mutex_lock(&s->poll_lock);
  uint32_t c = (uint32_t)at / PROC_POLL_CHUNK;
  int started = poll_start(s);
  proc_poll_desc* chunk = started ? atomic_load(&s->poll_chunks[c]) : NULL;
  if(started && !chunk && (chunk = (proc_poll_desc*)calloc(PROC_POLL_CHUNK, sizeof *chunk))) {
    for(uint32_t i = 0; i < PROC_POLL_CHUNK; i++)
      if(pthread_mutex_init(&chunk[i].mu, NULL) != 0) abort();
    atomic_store_explicit(&s->poll_chunks[c], chunk, memory_order_release);
  }
  pthread_mutex_unlock(&s->poll_lock);
  return chunk ? &chunk[(uint32_t)at % PROC_POLL_CHUNK] : NULL;
}

void proc_polls_end(proc_sched* s) {
  if(s->polling) {
    proc_poller_wake(&s->poller);
    pthread_join(s->poll_thread, NULL);
    proc_poller_close(&s->poller);
  }
  for(uint32_t c = 0; c < PROC_POLL_CHUNKS; c++) {
    proc_poll_desc* chunk = atomic_load(&s->poll_chunks[c]);
    if(!chunk) continue;
    for(uint32_t i = 0; i < PROC_POLL_CHUNK; i++) pthread_mutex_destroy(&chunk[i].mu);
    free(chunk);
  }
}
