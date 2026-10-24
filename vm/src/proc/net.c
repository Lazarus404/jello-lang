// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Sockets for Jello code: TCP listeners and streams, and UDP sockets. Each is non-blocking and
 * watched by the poller from its making until its closing, so a call that would block parks only
 * its process, until the socket is ready or the call's time runs out.
 *
 * A socket belongs to one process, and only that process's calls may use it. A message or a new
 * process's body moves it: the copy takes it from its owner, whose handle then reads as closed,
 * and its receiver owns it once it takes the message, or the new process from its start. A
 * message never taken closes it, and one that cannot be sent gives it back. Copies of globals
 * and exit values hold one that is not theirs, which reads as closed. A socket closes when its
 * owner closes it or finishes, or else once its last handle is collected.
 *
 * A call that fails returns null, an empty Bytes or false, and records why for the calling VM,
 * where net_error reads it: an errno value, a getaddrinfo error past NET_GAI, a TLS error past
 * NET_TLS_ERRORS, or one of NET_TIMEOUT and NET_CLOSED. Each call clears it first, so an empty
 * read with none recorded is the end of the stream.
 *
 * A stream secured with net_tls_start carries its data through its TLS session from then on:
 * net_read and net_write go through it, and closing it tells the other end first. Sessions are
 * tls.jdll's (jello/internal/tls.h), which the calling module has loaded to make a
 * configuration. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/sockets.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  NET_TIMEOUT = -1, /**< The call's time ran out. */
  NET_CLOSED = -2,  /**< The socket is closed, or not the caller's. */
  NET_GAI = 100000, /**< Added to a getaddrinfo error. */
};

/** The most one read takes. */
#define NET_READ_MAX (1u << 20)

typedef struct proc_socket {
  jdl_refs refs;
  int fd; /**< -1 once closed; its owner's to change. */
  /** The process it belongs to, holding a reference to it, or NULL while a parcel moves it and
   * once it is closed. Read without `net_lock` only to compare with the reader's process, which
   * only that process, or a taker while it is NULL, changes. */
  jello_process* owner;
  struct proc_socket* owned_prev; /**< In its owner's `sockets`. */
  struct proc_socket* owned_next;
  struct sockaddr_storage peer; /**< A stream's other end, or the last datagram's sender. */
  socklen_t peer_len;
  net_tls* tls; /**< A secured stream's TLS session, else NULL; its owner's to change. */
  const net_tls_ops* tls_ops; /**< The table of the TLS library that made `tls`. */
} proc_socket;

/** The sockets a copy moves; parcels joined chain theirs. */
struct proc_socket_list {
  struct proc_socket_list* next;
  uint32_t len;
  uint32_t cap;
  proc_socket* at[];
};

/** Guards every socket's `owner` changes and each process's `sockets`. Taken alone. */
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;

/** A thread's buffer for reads, used by one read attempt from its start to its copying out. */
static _Thread_local uint8_t* read_buf;
static _Thread_local uint32_t read_buf_cap;

/** This thread's read buffer, grown to `max` bytes, or NULL when out of memory. Never inlined, so
 * that the thread is found afresh after a wait, which may move the reading process to another. */
static __attribute__((noinline)) uint8_t* read_buffer(uint32_t max) {
  if(read_buf_cap < max) {
    uint8_t* grown = (uint8_t*)realloc(read_buf, max);
    if(!grown) return NULL;
    read_buf = grown;
    read_buf_cap = max;
  }
  return read_buf;
}

static jello_process* owner_of(const proc_socket* k) {
  return __atomic_load_n(&k->owner, __ATOMIC_ACQUIRE);
}

static void set_owner(proc_socket* k, jello_process* p) {
  __atomic_store_n(&k->owner, p, __ATOMIC_RELEASE);
}

/** Makes `p` the owner of `k`, which has none, under `net_lock`. */
static void adopt(proc_socket* k, jello_process* p) {
  proc_retain(p);
  k->owned_prev = NULL;
  k->owned_next = p->sockets;
  if(p->sockets) p->sockets->owned_prev = k;
  p->sockets = k;
  set_owner(k, p);
}

/** Takes `k` from its owner, under `net_lock`, leaving it none; the caller releases the owner. */
static void disown(proc_socket* k) {
  if(k->owned_prev) k->owned_prev->owned_next = k->owned_next;
  else k->owner->sockets = k->owned_next;
  if(k->owned_next) k->owned_next->owned_prev = k->owned_prev;
  k->owned_prev = k->owned_next = NULL;
  set_owner(k, NULL);
}

/** The TLS library among those `m` loaded, which a program loads to make a configuration; NULL
 * when there is none. */
static const net_tls_ops* tls_library(const jello_bc_module* m) {
  const jello_jdll_handle* const* handles = (const jello_jdll_handle* const*)m->jdll_handles;
  for(uint32_t i = 0; i < m->njdll_handles; i++) {
    const net_tls_ops* ops = (const net_tls_ops*)jello_jdll_symbol(handles[i], "jello_tls_ops");
    if(ops && ops->version == NET_TLS_OPS_VERSION) return ops;
  }
  return NULL;
}

/** Drops the last handle's hold, closing a socket nobody closed. */
static void free_socket(void* payload) {
  if(!jdl_release(payload)) return;
  proc_socket* k = (proc_socket*)payload;
  pthread_mutex_lock(&net_lock);
  jello_process* owner = k->owner;
  if(owner) disown(k);
  pthread_mutex_unlock(&net_lock);
  if(k->tls) k->tls_ops->close(k->tls, 0);
  /* The poller finds the number closed, or open again, the next time it is watched. */
  if(k->fd >= 0) jello_sock_close(k->fd);
  if(owner) proc_release(owner);
  free(k);
}

const jello_jdll_handle_class proc_socket_class = {"socket", free_socket, jdl_retain};

int proc_socket_note(struct proc_socket_list** list, void* payload) {
  struct proc_socket_list* l = *list;
  if(!l || l->len == l->cap) {
    uint32_t cap = l ? l->cap * 2 : 4;
    l = (struct proc_socket_list*)realloc(l, sizeof *l + sizeof(proc_socket*) * cap);
    if(!l) return 0;
    if(!*list) {
      l->next = NULL;
      l->len = 0;
    }
    l->cap = cap;
    *list = l;
  }
  l->at[l->len++] = (proc_socket*)payload;
  return 1;
}

void proc_sockets_pack(const jello_vm* vm, struct proc_socket_list** list) {
  struct proc_socket_list* l = *list;
  jello_process* from = vm ? vm->proc : NULL;
  uint32_t kept = 0;
  pthread_mutex_lock(&net_lock);
  for(uint32_t i = 0; from && i < l->len; i++) {
    proc_socket* k = l->at[i];
    /* A second handle to one taken already finds it ownerless. */
    if(k->owner != from || k->fd < 0) continue;
    disown(k);
    jdl_retain(k);
    l->at[kept++] = k;
  }
  pthread_mutex_unlock(&net_lock);
  for(uint32_t i = 0; i < kept; i++) proc_release(from);
  l->len = kept;
  if(!kept) {
    free(l);
    *list = NULL;
  }
}

void proc_sockets_take(jello_process* p, struct proc_socket_list** list) {
  struct proc_socket_list* chain = __atomic_exchange_n(list, NULL, __ATOMIC_ACQ_REL);
  if(!chain) return;
  if(p) {
    pthread_mutex_lock(&net_lock);
    for(struct proc_socket_list* l = chain; l; l = l->next) {
      for(uint32_t i = 0; i < l->len; i++) {
        if(l->at[i]->fd >= 0) adopt(l->at[i], p);
      }
    }
    pthread_mutex_unlock(&net_lock);
  }
  while(chain) {
    struct proc_socket_list* l = chain;
    chain = l->next;
    for(uint32_t i = 0; i < l->len; i++) {
      proc_socket* k = l->at[i];
      if(!p && k->fd >= 0) {
        jello_sock_close(k->fd);
        k->fd = -1;
      }
      free_socket(k);
    }
    free(l);
  }
}

void proc_sockets_join(struct proc_socket_list** into, struct proc_socket_list** from) {
  struct proc_socket_list* last = *from;
  while(last->next) last = last->next;
  last->next = *into;
  *into = *from;
  *from = NULL;
}

/** Closes `k`, which `p` owns, as its call or its finishing asks, under `net_lock`, which it
 * releases. */
static void close_locked(proc_socket* k, jello_process* p) {
  disown(k);
  int fd = k->fd;
  net_tls* tls = k->tls;
  const net_tls_ops* ops = k->tls_ops;
  k->fd = -1;
  k->tls = NULL;
  pthread_mutex_unlock(&net_lock);
  if(tls) ops->close(tls, 1);
  proc_poll_close(p->vm, fd);
  jello_sock_close(fd);
  proc_release(p);
}

void proc_sockets_end(jello_process* p) {
  for(;;) {
    /* Under the lock, as a collection elsewhere may free one it holds the last handle of. */
    pthread_mutex_lock(&net_lock);
    if(!p->sockets) break;
    close_locked(p->sockets, p);
  }
  pthread_mutex_unlock(&net_lock);
}

/** Records `err` as why the call failed. */
static void failed(jdlo_ctx* c, int32_t err) { c->vm->net_error = err; }

/** Starts a call on the socket in argument 0: its descriptor, with the caller's process in *p
 * and the socket in *k; -1 when it is not the caller's, recording NET_CLOSED, or, failing the
 * call, when the argument is not a socket. */
static int arg_fd(jdlo_ctx* c, jello_process** p, proc_socket** k) {
  failed(c, 0);
  *p = proc_self_or_fail(c);
  *k = *p ? (proc_socket*)proc_arg_handle(c, 0, &proc_socket_class) : NULL;
  if(!*k) return -1;
  int fd = owner_of(*k) == *p ? (*k)->fd : -1;
  if(fd < 0) failed(c, NET_CLOSED);
  return fd;
}

/** Makes `fd`, a new socket, non-blocking, closed on exec, free of SIGPIPE on Apple systems and,
 * for a stream, quick to send small writes, and watches it; whether it could, closing it, with
 * errno set, when not. */
static int watch_fd(jello_vm* vm, int fd, int stream) {
  int watched = 0;
#ifdef SO_NOSIGPIPE
  int one = 1;
  int quiet = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) == 0;
#else
  int quiet = 1;
#endif
  if(quiet && jello_sock_nonblocking(fd) == 0) {
    if(stream) jello_sock_nodelay(fd);
    proc_poll_open(vm, fd, &watched);
  }
  if(!watched) {
    int err = errno ? errno : EMFILE;
    jello_sock_close(fd);
    errno = err;
  }
  return watched;
}

/** A watched socket of `family` and `type`, or -1 with errno set. */
static int open_fd(jello_vm* vm, int family, int type) {
  int fd = jello_sock_open(family, type);
  if(fd >= 0 && type == SOCK_DGRAM) jello_sock_datagram(fd);
  return fd >= 0 && watch_fd(vm, fd, type == SOCK_STREAM) ? fd : -1;
}

static void close_fd(jello_vm* vm, int fd) {
  proc_poll_close(vm, fd);
  jello_sock_close(fd);
}

/** Returns a handle to a new socket of `p`'s on descriptor `fd`, watched already, whose other
 * end is `peer`, when it has one. */
static void return_socket(jdlo_ctx* c, jello_process* p, int fd, const struct sockaddr* peer,
                          socklen_t peer_len) {
  proc_socket* k = (proc_socket*)calloc(1, sizeof(proc_socket));
  if(!k) {
    close_fd(c->vm, fd);
    jdl_fail(c, "out of memory");
    return;
  }
  k->refs = 1;
  k->fd = fd;
  if(peer && (size_t)peer_len <= sizeof k->peer) {
    memcpy(&k->peer, peer, (size_t)peer_len);
    k->peer_len = peer_len;
  }
  pthread_mutex_lock(&net_lock);
  adopt(k, p);
  pthread_mutex_unlock(&net_lock);
  jdl_return_handle(c, &proc_socket_class, k, (uint32_t)sizeof(proc_socket));
}

/** Parks until `fd` may be ready for `events`, or until `deadline` when that is not 0; whether
 * it may be. When not, records why, or fails the call for the trap ending the wait. */
static int await(jdlo_ctx* c, int fd, uint32_t events, uint64_t deadline) {
  uint32_t ready = 0;
  jello_trap_code trap = proc_poll_wait(c->vm, fd, events, deadline, &ready);
  if(trap) proc_trap(c, trap);
  else if(ready & PROC_FD_CLOSED) failed(c, NET_CLOSED);
  else if(!ready) failed(c, NET_TIMEOUT);
  return !trap && ready && !(ready & PROC_FD_CLOSED);
}

/** After a call on `fd` failed with errno, whether to try it again: at once after an interrupt,
 * or once await finds `fd` may be ready for `events`. When not, records why, or fails the call. */
static int retry(jdlo_ctx* c, int fd, uint32_t events, uint64_t deadline) {
  if(errno == EINTR) return 1;
  if(errno == EAGAIN || errno == EWOULDBLOCK) return await(c, fd, events, deadline);
  failed(c, errno);
  return 0;
}

/** After a TLS call on `k`'s socket `fd` gave `r`, whether to try it again, once await finds `fd`
 * ready for what its session waits for. When not, records why, or fails the call. */
static int tls_retry(jdlo_ctx* c, const proc_socket* k, int fd, int r, uint64_t deadline) {
  if(r == NET_TLS_WANT_READ) return await(c, fd, PROC_FD_READ, deadline);
  if(r == NET_TLS_WANT_WRITE) return await(c, fd, PROC_FD_WRITE, deadline);
  failed(c, k->tls_ops->error(k->tls));
  return 0;
}

/** Addresses for a call: getaddrinfo's list, or one numeric address parsed in place. */
typedef struct net_addrs {
  struct addrinfo* list;
  struct addrinfo one;
  struct sockaddr_storage addr;
} net_addrs;

static void addrs_free(net_addrs* a) {
  if(a->list != &a->one) freeaddrinfo(a->list);
}

/** Whether `host` is a numeric address, which it puts in `a` for `port` and sockets of `type`. */
static int numeric(net_addrs* a, const char* host, int32_t port, int type) {
  memset(&a->addr, 0, sizeof a->addr);
  memset(&a->one, 0, sizeof a->one);
  struct sockaddr_in* v4 = (struct sockaddr_in*)&a->addr;
  struct sockaddr_in6* v6 = (struct sockaddr_in6*)&a->addr;
  if(inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons((uint16_t)port);
    a->one.ai_addrlen = sizeof *v4;
  } else if(inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons((uint16_t)port);
    a->one.ai_addrlen = sizeof *v6;
  } else {
    return 0;
  }
  a->one.ai_family = a->addr.ss_family;
  a->one.ai_socktype = type;
  a->one.ai_addr = (struct sockaddr*)&a->addr;
  a->list = &a->one;
  return 1;
}

/** The addresses `host` (argument `hi`, empty for any) and `port` (argument `pi`) name for
 * sockets of `type`, in `a`; whether there were any, recording why when not. Numeric hosts
 * resolve at once; names block the scheduler while they resolve. */
static int resolve(jdlo_ctx* c, int hi, int pi, int type, int passive, net_addrs* a) {
  uint32_t len = jdl_arg_bytes_len(c, hi);
  const uint8_t* data = jdl_arg_bytes_data(c, hi);
  int32_t port = jdl_arg_i32(c, pi);
  char host[256];
  if(len >= sizeof host || (len && memchr(data, '\0', len)) || port < 0 || port > 65535) {
    failed(c, EINVAL);
    return 0;
  }
  if(len) memcpy(host, data, len);
  host[len] = '\0';
  if(len && numeric(a, host, port, type)) return 1;
  if(!jello_sockets_start()) {
    failed(c, EIO);
    return 0;
  }
  char service[16];
  snprintf(service, sizeof service, "%d", (int)port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = type;
  hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
  int err = getaddrinfo(len ? host : NULL, service, &hints, &a->list);
  if(err) {
#ifdef EAI_SYSTEM
    failed(c, err == EAI_SYSTEM ? errno : NET_GAI + err);
#else
    failed(c, NET_GAI + err);
#endif
    return 0;
  }
  return 1;
}

/** A socket of `type` bound to the address arguments 0 and 1 name, listening when it is a
 * stream; null, with the reason recorded, when there can be none. */
static void bound(jdlo_ctx* c, int type) {
  failed(c, 0);
  jello_process* p = proc_self_or_fail(c);
  net_addrs ai;
  if(!p) return;
  if(!resolve(c, 0, 1, type, 1, &ai)) {
    jdl_return_null(c);
    return;
  }
  int fd = -1;
  int err = EADDRNOTAVAIL;
  for(struct addrinfo* a = ai.list; a && fd < 0; a = a->ai_next) {
    fd = open_fd(c->vm, a->ai_family, type);
    if(fd < 0) {
      err = errno;
      continue;
    }
    jello_sock_reuse_port(fd);
    if(jello_bind(fd, a->ai_addr, (socklen_t)a->ai_addrlen) < 0 ||
       (type == SOCK_STREAM && jello_listen(fd, 1024) < 0)) {
      err = errno;
      close_fd(c->vm, fd);
      fd = -1;
    }
  }
  addrs_free(&ai);
  if(fd >= 0) {
    return_socket(c, p, fd, NULL, 0);
    return;
  }
  failed(c, err);
  jdl_return_null(c);
}

/** net_tcp_listen(host, port): a listener on `host`, empty for every address, and `port`, 0 for
 * any free one; null when there can be none. */
void vm_net_tcp_listen(jdlo_ctx* c) { bound(c, SOCK_STREAM); }

/** net_udp_bind(host, port): a UDP socket on `host` and `port`, as net_tcp_listen takes them;
 * null when there can be none. */
void vm_net_udp_bind(jdlo_ctx* c) { bound(c, SOCK_DGRAM); }

/** net_tcp_accept(listener, ms): the next connection, waiting up to `ms` milliseconds for one,
 * or as long as it takes when `ms` is negative; null when there is none. */
void vm_net_tcp_accept(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  uint64_t deadline = proc_deadline(jdl_arg_i32(c, 1));
  while(fd >= 0) {
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof peer;
    int nfd = jello_accept(fd, (struct sockaddr*)&peer, &peer_len);
    if(nfd >= 0) {
      if(watch_fd(c->vm, nfd, 1)) {
        return_socket(c, p, nfd, (struct sockaddr*)&peer, peer_len);
        return;
      }
      failed(c, errno);
      break;
    }
    /* The connection was dropped while it waited; another may follow. */
    if(errno != ECONNABORTED && !retry(c, fd, PROC_FD_READ, deadline)) break;
  }
  if(!c->failed) jdl_return_null(c);
}

/** net_tcp_connect(host, port, ms): a stream connected to `host` and `port`, waiting up to `ms`
 * milliseconds, or as long as it takes when `ms` is negative; null when it cannot connect. */
void vm_net_tcp_connect(jdlo_ctx* c) {
  failed(c, 0);
  jello_process* p = proc_self_or_fail(c);
  net_addrs ai;
  if(!p) return;
  if(!resolve(c, 0, 1, SOCK_STREAM, 0, &ai)) {
    jdl_return_null(c);
    return;
  }
  uint64_t deadline = proc_deadline(jdl_arg_i32(c, 2));
  int err = ECONNREFUSED;
  for(struct addrinfo* a = ai.list; a; a = a->ai_next) {
    int fd = open_fd(c->vm, a->ai_family, SOCK_STREAM);
    if(fd < 0) {
      err = errno;
      continue;
    }
    int r = jello_connect(fd, a->ai_addr, (socklen_t)a->ai_addrlen);
    if(r < 0 && errno == EINPROGRESS) {
      if(await(c, fd, PROC_FD_WRITE, deadline)) {
        err = jello_sock_connect_error(fd);
        r = err ? -1 : 0;
      } else {
        err = c->vm->net_error;
      }
    } else if(r < 0) {
      err = errno;
    }
    if(r == 0) {
      return_socket(c, p, fd, a->ai_addr, (socklen_t)a->ai_addrlen);
      addrs_free(&ai);
      return;
    }
    close_fd(c->vm, fd);
    if(c->failed || err == NET_TIMEOUT) break;
  }
  addrs_free(&ai);
  if(c->failed) return;
  failed(c, err);
  jdl_return_null(c);
}

/** Reads up to argument 1's count into a Bytes, from a stream, or as a datagram when `from`,
 * recording its sender, waiting up to argument 2's milliseconds. */
static void receive(jdlo_ctx* c, int from) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  int32_t max = jdl_arg_i32(c, 1);
  if(fd >= 0 && max <= 0) {
    failed(c, EINVAL);
    fd = -1;
  }
  if((uint32_t)max > NET_READ_MAX) max = (int32_t)NET_READ_MAX;
  uint64_t deadline = proc_deadline(jdl_arg_i32(c, 2));
  while(fd >= 0) {
    uint8_t* buf = read_buffer((uint32_t)max);
    if(!buf) {
      jdl_fail(c, "out of memory");
      return;
    }
    if(k->tls) {
      int r = k->tls_ops->read(k->tls, buf, (uint32_t)max);
      if(r >= 0) {
        jdl_return_bytes_copy(c, buf, (uint32_t)r);
        return;
      }
      if(!tls_retry(c, k, fd, r, deadline)) break;
      continue;
    }
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof peer;
    ssize_t n = from ? jello_recvfrom(fd, buf, (size_t)max, (struct sockaddr*)&peer, &peer_len)
                     : jello_recv(fd, buf, (size_t)max);
    if(n >= 0) {
      if(from) {
        memcpy(&k->peer, &peer, (size_t)peer_len);
        k->peer_len = peer_len;
      }
      jdl_return_bytes_copy(c, buf, (uint32_t)n);
      return;
    }
    if(!retry(c, fd, PROC_FD_READ, deadline)) break;
  }
  if(!c->failed) jdl_return_bytes_copy(c, NULL, 0);
}

/** net_read(stream, max, ms): up to `max` bytes, waiting up to `ms` milliseconds for some, or as
 * long as it takes when `ms` is negative; empty at the end of the stream, recording no error. */
void vm_net_read(jdlo_ctx* c) { receive(c, 0); }

/** net_receive_from(socket, max, ms): the next datagram, as net_read takes it, recording its
 * sender for net_peer_host and net_peer_port. */
void vm_net_receive_from(jdlo_ctx* c) { receive(c, 1); }

/** net_write(stream, data): writes all of `data`, waiting while the other end is slow to take
 * it; whether it could. */
void vm_net_write(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  /* The Bytes stays where it is while the call parks: it is the caller's argument. */
  const uint8_t* data = fd >= 0 ? jdl_arg_bytes_data(c, 1) : NULL;
  uint32_t len = fd >= 0 ? jdl_arg_bytes_len(c, 1) : 0;
  uint32_t done = 0;
  while(fd >= 0 && done < len && k->tls) {
    int r = k->tls_ops->write(k->tls, data + done, len - done);
    if(r >= 0) done += (uint32_t)r;
    else if(!tls_retry(c, k, fd, r, 0)) break;
  }
  while(fd >= 0 && done < len && !k->tls) {
    ssize_t n = jello_send(fd, data + done, len - done);
    if(n > 0) {
      done += (uint32_t)n;
      continue;
    }
    /* Nothing taken is waited out as a full buffer is. */
    if(n == 0) errno = EAGAIN;
    if(!retry(c, fd, PROC_FD_WRITE, 0)) break;
  }
  if(!c->failed) jdl_return_bool(c, fd >= 0 && done == len);
}

/** net_send_to(socket, data, host, port): sends `data` as one datagram to `host` and `port`;
 * whether it went. */
void vm_net_send_to(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  net_addrs ai;
  int sent = 0;
  if(fd >= 0 && resolve(c, 2, 3, SOCK_DGRAM, 0, &ai)) {
    const uint8_t* data = jdl_arg_bytes_data(c, 1);
    uint32_t len = jdl_arg_bytes_len(c, 1);
    while(!(sent = jello_sendto(fd, data, len, ai.list->ai_addr, (socklen_t)ai.list->ai_addrlen) >=
                   0) &&
          retry(c, fd, PROC_FD_WRITE, 0)) {}
    addrs_free(&ai);
  }
  if(!c->failed) jdl_return_bool(c, sent);
}

/** net_close(socket): closes it; whether it was open and the caller's. */
void vm_net_close(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  if(fd >= 0) {
    pthread_mutex_lock(&net_lock);
    close_locked(k, p);
  }
  if(k) jdl_return_bool(c, fd >= 0);
}

/** net_tls_start(stream, config, host): secures `stream` as the side of the TLS configuration
 * `config`, a client naming the server `host`, waiting for the handshake as long as it
 * takes; whether it was made. A stream that cannot be secured is closed, unless it was secured
 * already, or is not a connected stream of the caller's. */
void vm_net_tls_start(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  const net_tls_ops* tls = NULL;
  void* config = NULL;
  if(fd >= 0) {
    tls = tls_library(c->m);
    if(tls) config = proc_arg_handle(c, 1, tls->config_class);
    else jdl_fail(c, "not a TLS configuration");
  }
  if(c->failed) return;
  int type = 0;
  if(fd >= 0 && (k->tls || jello_sock_option(fd, SO_TYPE, &type) != 0 || type != SOCK_STREAM)) {
    failed(c, EINVAL);
    fd = -1;
  }
  int done = 0;
  if(fd >= 0) {
    int32_t err = 0;
    k->tls_ops = tls;
    k->tls = tls->open(config, fd, jdl_arg_bytes_data(c, 2), jdl_arg_bytes_len(c, 2), &err);
    if(!k->tls) failed(c, err);
    int r = NET_TLS_FAILED;
    while(k->tls && (r = tls->handshake(k->tls)) != 0 && tls_retry(c, k, fd, r, 0)) {}
    done = k->tls && r == 0;
    if(!done && owner_of(k) == p) {
      pthread_mutex_lock(&net_lock);
      close_locked(k, p);
    }
  }
  if(!c->failed) jdl_return_bool(c, done);
}

/** The port in address `a`, or -1 for another family. */
static int32_t port_of(const struct sockaddr_storage* a) {
  if(a->ss_family == AF_INET) return ntohs(((const struct sockaddr_in*)a)->sin_port);
  if(a->ss_family == AF_INET6) return ntohs(((const struct sockaddr_in6*)a)->sin6_port);
  return -1;
}

/** net_local_port(socket): the port it is bound to, or -1. */
void vm_net_local_port(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  if(!k) return;
  struct sockaddr_storage a;
  socklen_t len = sizeof a;
  memset(&a, 0, sizeof a);
  if(fd >= 0 && jello_getsockname(fd, (struct sockaddr*)&a, &len) < 0) failed(c, errno);
  jdl_return_i32(c, fd >= 0 ? port_of(&a) : -1);
}

/** net_peer_host(socket): the numeric host of a stream's other end, or of the last datagram's
 * sender; empty when it has none or is not the caller's. */
void vm_net_peer_host(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  if(!k) return;
  char host[NI_MAXHOST];
  if(fd < 0 || !k->peer_len ||
     getnameinfo((const struct sockaddr*)&k->peer, k->peer_len, host, sizeof host, NULL, 0,
                 NI_NUMERICHOST)) {
    host[0] = '\0';
  }
  jdl_return_bytes_copy(c, (const uint8_t*)host, (uint32_t)strlen(host));
}

/** net_peer_port(socket): the port of a stream's other end, or of the last datagram's sender;
 * -1 when it has none or is not the caller's. */
void vm_net_peer_port(jdlo_ctx* c) {
  jello_process* p;
  proc_socket* k;
  int fd = arg_fd(c, &p, &k);
  if(k) jdl_return_i32(c, fd >= 0 && k->peer_len ? port_of(&k->peer) : -1);
}

/** net_error(): why the caller's last socket call failed, 0 when it did not. */
void vm_net_error(jdlo_ctx* c) { jdl_return_i32(c, c->vm->net_error); }

/** net_error_text(code): what net_error's `code` means. */
void vm_net_error_text(jdlo_ctx* c) {
  int32_t code = jdl_arg_i32(c, 0);
  char tls[256];
  /* A TLS code is the TLS library's to read, which a module that never loaded it cannot. */
  const net_tls_ops* ops = code >= NET_TLS_ERRORS ? tls_library(c->m) : NULL;
  if(ops) ops->error_text(code, tls, sizeof tls);
  else if(code >= NET_TLS_ERRORS) snprintf(tls, sizeof tls, "TLS error %d", NET_TLS_ERRORS - code);
  const char* text = code == 0                ? "no error"
                     : code == NET_TIMEOUT    ? "timed out"
                     : code == NET_CLOSED     ? "socket closed, or another process's"
                     : code >= NET_TLS_ERRORS ? tls
                     : code >= NET_GAI        ? gai_strerror(code - NET_GAI)
                                              : jello_error_text(code);
  jdl_return_bytes_copy(c, (const uint8_t*)text, (uint32_t)strlen(text));
}
