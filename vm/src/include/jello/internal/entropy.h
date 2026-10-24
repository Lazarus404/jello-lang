// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The system's entropy, for std.jdll's random values and tls.jdll's keys. */
#ifndef JELLO_INTERNAL_ENTROPY_H
#define JELLO_INTERNAL_ENTROPY_H

#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <jello/internal/platform.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif
#endif

/** Fills `out`'s `len` bytes from the system's entropy, which never runs out once the system has
 * started; whether it could. */
static inline int jello_entropy(void* out, size_t len) {
  uint8_t* p = (uint8_t*)out;
  while(len > 0) {
#ifdef _WIN32
    ULONG n = len < 65536 ? (ULONG)len : 65536;
    if(BCryptGenRandom(NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return 0;
#else
    size_t n = len < 256 ? len : 256; /* getentropy's most */
    if(getentropy(p, n) != 0) return 0;
#endif
    p += n;
    len -= n;
  }
  return 1;
}

#endif /* JELLO_INTERNAL_ENTROPY_H */
