// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What tls.jdll gives the VM: sessions on the vendored Mbed TLS, through the table net_tls_ops,
 * its data symbol `jello_tls_ops`. Jello code makes configurations with the library's own
 * exports, so a program using TLS imports it, and fails to load without it, and its bundle
 * carries it; no other program loads it. The VM finds the table among the libraries of the
 * module securing a stream, and keeps it with the session. */
#ifndef JELLO_INTERNAL_TLS_H
#define JELLO_INTERNAL_TLS_H

#include <jello/jdll.h>

#include <stddef.h>
#include <stdint.h>

/** What a TLS session's calls return in place of a count, and where net_error's TLS codes start. */
enum {
  NET_TLS_WANT_READ = -1,  /**< To go on once its socket can be read. */
  NET_TLS_WANT_WRITE = -2, /**< To go on once its socket can be written. */
  NET_TLS_FAILED = -3,     /**< Failed, for the reason `error` gives. */
  NET_TLS_ERRORS = 200000, /**< Added to the negated Mbed TLS error. */
};

/** A stream's TLS session, which tls.jdll documents. */
typedef struct net_tls net_tls;

/** The version of net_tls_ops that this VM and library agree on. */
#define NET_TLS_OPS_VERSION 1u

/** TLS, as tls.jdll exports it. Errors are net_error codes: errno values, or Mbed TLS errors from
 * NET_TLS_ERRORS. */
typedef struct net_tls_ops {
  uint32_t version; /**< NET_TLS_OPS_VERSION. */
  /** The class of a configuration's handle, which tls_config makes, shared by the processes it
   * is sent to. */
  const jello_jdll_handle_class* config_class;
  /** A session on the connected socket `fd` for `config`'s side, a client naming the server
   * `host`, `len` bytes, which it must give; NULL with *err set when it cannot. */
  net_tls* (*open)(void* config, int fd, const uint8_t* host, uint32_t len, int32_t* err);
  /** Takes the handshake as far as the socket allows: 0 once done, else NET_TLS_WANT_READ,
   * NET_TLS_WANT_WRITE or NET_TLS_FAILED. */
  int (*handshake)(net_tls* s);
  /** Reads up to `max` bytes of data into `buf`: the count, 0 at the end of the stream, or as
   * `handshake` does. */
  int (*read)(net_tls* s, uint8_t* buf, uint32_t max);
  /** Writes some of `data`, `len` bytes: the count taken, or as `handshake` does. */
  int (*write)(net_tls* s, const uint8_t* data, uint32_t len);
  /** Why `s` last failed. */
  int32_t (*error)(const net_tls* s);
  /** Ends `s`, first telling the other end, without waiting, when `notify` is set. */
  void (*close)(net_tls* s, int notify);
  /** Writes what the TLS code `code` means into `out`, `size` bytes. */
  void (*error_text)(int32_t code, char* out, size_t size);
} net_tls_ops;

#endif
