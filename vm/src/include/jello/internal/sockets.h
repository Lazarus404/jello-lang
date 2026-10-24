// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Sockets as the VM and tls.jdll use them, the same on every system: a socket is an `int`
 * descriptor, and a call that fails returns -1 with errno set to the POSIX error. On Windows the
 * descriptor is the socket's handle, which, as every handle, fits in 32 bits, and Winsock's errors
 * are mapped to errno's. */
#ifndef JELLO_INTERNAL_SOCKETS_H
#define JELLO_INTERNAL_SOCKETS_H

#include <jello/internal/platform.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#ifdef _WIN32
#include <mstcpip.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32

/** The handle of descriptor `fd`. */
#define JELLO_SOCKET(fd) ((SOCKET)(uintptr_t)(uint32_t)(fd))

/** Sets errno to the POSIX error for Winsock's last, and returns -1. */
static inline int jello_sock_failed(void) {
  switch(WSAGetLastError()) {
    case WSAEWOULDBLOCK: errno = EWOULDBLOCK; break;
    case WSAEINPROGRESS: errno = EINPROGRESS; break;
    case WSAEALREADY: errno = EALREADY; break;
    case WSAEINTR: errno = EINTR; break;
    case WSAEBADF:
    case WSAENOTSOCK: errno = EBADF; break;
    case WSAEACCES: errno = EACCES; break;
    case WSAEFAULT:
    case WSAEINVAL: errno = EINVAL; break;
    case WSAEMFILE: errno = EMFILE; break;
    case WSAEMSGSIZE: errno = EMSGSIZE; break;
    case WSAEPROTOTYPE: errno = EPROTOTYPE; break;
    case WSAENOPROTOOPT: errno = ENOPROTOOPT; break;
    case WSAEPROTONOSUPPORT: errno = EPROTONOSUPPORT; break;
    case WSAEOPNOTSUPP: errno = EOPNOTSUPP; break;
    case WSAEAFNOSUPPORT: errno = EAFNOSUPPORT; break;
    case WSAEADDRINUSE: errno = EADDRINUSE; break;
    case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
    case WSAENETDOWN: errno = ENETDOWN; break;
    case WSAENETUNREACH: errno = ENETUNREACH; break;
    case WSAENETRESET: errno = ENETRESET; break;
    case WSAECONNABORTED: errno = ECONNABORTED; break;
    case WSAECONNRESET: errno = ECONNRESET; break;
    case WSAENOBUFS: errno = ENOBUFS; break;
    case WSAEISCONN: errno = EISCONN; break;
    case WSAENOTCONN: errno = ENOTCONN; break;
    case WSAESHUTDOWN: errno = EPIPE; break;
    case WSAETIMEDOUT: errno = ETIMEDOUT; break;
    case WSAECONNREFUSED: errno = ECONNREFUSED; break;
    case WSAEHOSTDOWN:
    case WSAEHOSTUNREACH: errno = EHOSTUNREACH; break;
    case WSA_NOT_ENOUGH_MEMORY: errno = ENOMEM; break;
    default: errno = EIO; break;
  }
  return -1;
}

/** Starts Winsock for this process, once; whether it is. */
static inline int jello_sockets_start(void) {
  static volatile LONG started;
  if(started) return 1;
  WSADATA wsa;
  if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
  InterlockedExchange(&started, 1);
  return 1;
}

#else

#define JELLO_SOCKET(fd) (fd)

/** Returns -1, errno being set already. */
static inline int jello_sock_failed(void) { return -1; }

/** Whether sockets can be used, which they always can. */
static inline int jello_sockets_start(void) { return 1; }

#endif

/** Checks `r`, a socket call's result: itself, or -1 with errno set when the call failed. */
static inline ssize_t jello_sock_check(ssize_t r) { return r < 0 ? jello_sock_failed() : r; }

/** A new socket of `family` and `type`, not inherited by programs this one starts; -1 with errno
 * set when there can be none. */
static inline int jello_sock_open(int family, int type) {
#ifdef _WIN32
  if(!jello_sockets_start()) return jello_sock_failed();
  SOCKET s = WSASocketW(family, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  return s == INVALID_SOCKET ? jello_sock_failed() : (int)(uint32_t)s;
#else
  return socket(family, type, 0);
#endif
}

/** Makes socket `fd` non-blocking and, on POSIX, closed when this program runs another; 0, or -1
 * with errno set. */
static inline int jello_sock_nonblocking(int fd) {
#ifdef _WIN32
  u_long on = 1;
  return ioctlsocket(JELLO_SOCKET(fd), (long)FIONBIO, &on) == 0 ? 0 : jello_sock_failed();
#else
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
                 fcntl(fd, F_SETFD, FD_CLOEXEC) == 0
             ? 0
             : -1;
#endif
}

/** Lets socket `fd`, before it is bound, take a port another socket has just closed; on Windows,
 * where that is the default and the option would let a second socket share a bound port, makes
 * the port its alone instead. */
static inline void jello_sock_reuse_port(int fd) {
  int one = 1;
#ifdef _WIN32
  setsockopt(JELLO_SOCKET(fd), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
}

/** Keeps UDP socket `fd` from failing its next receive after a datagram it sent was refused, as
 * Windows sockets otherwise do; elsewhere they never do. */
static inline void jello_sock_datagram(int fd) {
#ifdef _WIN32
  BOOL off = FALSE;
  DWORD bytes = 0;
  WSAIoctl(JELLO_SOCKET(fd), SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
#else
  (void)fd;
#endif
}

#ifdef _WIN32
/** `len`, as much of it as one Winsock call, which counts in `int`, can take. */
static inline int jello_sock_len(size_t len) { return len > INT32_MAX ? INT32_MAX : (int)len; }
#endif

/** Closes socket `fd`. */
static inline void jello_sock_close(int fd) {
#ifdef _WIN32
  closesocket(JELLO_SOCKET(fd));
#else
  close(fd);
#endif
}

/** Sends `len` bytes of `buf` on `fd`, as send does, never raising SIGPIPE. */
static inline ssize_t jello_send(int fd, const void* buf, size_t len) {
#ifdef _WIN32
  return jello_sock_check(send(JELLO_SOCKET(fd), (const char*)buf, jello_sock_len(len), 0));
#else
  return send(fd, buf, len, MSG_NOSIGNAL);
#endif
}

/** Sends `len` bytes of `buf` on `fd` to `to`, as sendto does, never raising SIGPIPE. */
static inline ssize_t jello_sendto(int fd, const void* buf, size_t len, const struct sockaddr* to,
                                   socklen_t to_len) {
#ifdef _WIN32
  return jello_sock_check(
      sendto(JELLO_SOCKET(fd), (const char*)buf, jello_sock_len(len), 0, to, to_len));
#else
  return sendto(fd, buf, len, MSG_NOSIGNAL, to, to_len);
#endif
}

/** Receives up to `len` bytes into `buf` from `fd`, as recv does. */
static inline ssize_t jello_recv(int fd, void* buf, size_t len) {
#ifdef _WIN32
  return jello_sock_check(recv(JELLO_SOCKET(fd), (char*)buf, jello_sock_len(len), 0));
#else
  return recv(fd, buf, len, 0);
#endif
}

/** Receives up to `len` bytes into `buf` from `fd`, and their sender, as recvfrom does. */
static inline ssize_t jello_recvfrom(int fd, void* buf, size_t len, struct sockaddr* from,
                                     socklen_t* from_len) {
#ifdef _WIN32
  return jello_sock_check(
      recvfrom(JELLO_SOCKET(fd), (char*)buf, jello_sock_len(len), 0, from, from_len));
#else
  return recvfrom(fd, buf, len, 0, from, from_len);
#endif
}

/** The next connection on listener `fd`, as accept does. */
static inline int jello_accept(int fd, struct sockaddr* peer, socklen_t* peer_len) {
#ifdef _WIN32
  SOCKET s = accept(JELLO_SOCKET(fd), peer, peer_len);
  return s == INVALID_SOCKET ? jello_sock_failed() : (int)(uint32_t)s;
#else
  return accept(fd, peer, peer_len);
#endif
}

/** Connects `fd` to `to`, as connect does; a non-blocking socket's connection still being made
 * fails with EINPROGRESS. */
static inline int jello_connect(int fd, const struct sockaddr* to, socklen_t to_len) {
#ifdef _WIN32
  if(connect(JELLO_SOCKET(fd), to, to_len) == 0) return 0;
  jello_sock_failed();
  if(errno == EWOULDBLOCK) errno = EINPROGRESS;
  return -1;
#else
  return connect(fd, to, to_len);
#endif
}

/** Binds `fd` to `at`, as bind does. */
static inline int jello_bind(int fd, const struct sockaddr* at, socklen_t at_len) {
  return (int)jello_sock_check(bind(JELLO_SOCKET(fd), at, at_len));
}

/** Listens on `fd`, as listen does. */
static inline int jello_listen(int fd, int backlog) {
  return (int)jello_sock_check(listen(JELLO_SOCKET(fd), backlog));
}

/** The address `fd` is bound to, as getsockname gives it. */
static inline int jello_getsockname(int fd, struct sockaddr* at, socklen_t* at_len) {
  return (int)jello_sock_check(getsockname(JELLO_SOCKET(fd), at, at_len));
}

/** Socket-level option `name` of `fd` as an int, in *value; 0, or -1 with errno set. */
static inline int jello_sock_option(int fd, int name, int* value) {
  socklen_t len = sizeof *value;
  return (int)jello_sock_check(getsockopt(JELLO_SOCKET(fd), SOL_SOCKET, name, (char*)value, &len));
}

/** Why connecting `fd` failed, as an errno value, 0 when it did not. */
static inline int jello_sock_connect_error(int fd) {
  int err = 0;
  if(jello_sock_option(fd, SO_ERROR, &err) < 0) return errno;
#ifdef _WIN32
  if(err) {
    WSASetLastError(err);
    jello_sock_failed();
    err = errno;
  }
#endif
  return err;
}

/** What errno value `code` means, as strerror says; on Windows, whose C library names none of the
 * socket errors, in the words of glibc's. */
static inline const char* jello_error_text(int code) {
#ifdef _WIN32
  switch(code) {
    case EWOULDBLOCK: return "Resource temporarily unavailable";
    case EINPROGRESS: return "Operation now in progress";
    case EALREADY: return "Operation already in progress";
    case EPROTOTYPE: return "Protocol wrong type for socket";
    case ENOPROTOOPT: return "Protocol not available";
    case EPROTONOSUPPORT: return "Protocol not supported";
    case EOPNOTSUPP: return "Operation not supported";
    case EAFNOSUPPORT: return "Address family not supported by protocol";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Cannot assign requested address";
    case ENETDOWN: return "Network is down";
    case ENETUNREACH: return "Network is unreachable";
    case ENETRESET: return "Network dropped connection on reset";
    case ECONNABORTED: return "Software caused connection abort";
    case ECONNRESET: return "Connection reset by peer";
    case ENOBUFS: return "No buffer space available";
    case EISCONN: return "Transport endpoint is already connected";
    case ENOTCONN: return "Transport endpoint is not connected";
    case ETIMEDOUT: return "Connection timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "No route to host";
    case EMSGSIZE: return "Message too long";
    default: break;
  }
#endif
  return strerror(code);
}

/** Sends small writes on stream `fd` at once, rather than gathering them. */
static inline void jello_sock_nodelay(int fd) {
  int one = 1;
  setsockopt(JELLO_SOCKET(fd), IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

#endif /* JELLO_INTERNAL_SOCKETS_H */
