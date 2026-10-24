// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Random bytes and numbers from the system's entropy, which are fit for keys, nonces and tokens,
 * for std.crypto.Random. */

#include "std.h"

#include <jello/internal/entropy.h>

#include <stdlib.h>

/* Argument 0, at least 0, random bytes. */
void std_random_bytes(jdlo_ctx* c) {
  uint32_t len = (uint32_t)jdl_arg_i32(c, 0);
  uint8_t* out = (uint8_t*)malloc((size_t)len + 1);
  if(!out) jdl_fail(c, "out of memory");
  else if(!jello_entropy(out, len)) jdl_fail(c, "the system has no entropy");
  else jdl_return_bytes_copy(c, out, len);
  free(out);
}

/* A random I32 from argument 0 to argument 1, both included, each as likely; the first is not
 * above the second. */
void std_random_int(jdlo_ctx* c) {
  int32_t low = jdl_arg_i32(c, 0);
  uint64_t range = (uint64_t)((int64_t)jdl_arg_i32(c, 1) - low) + 1;
  /* Draws below 2^64 mod range would make the lowest values likelier, so are drawn again. */
  uint64_t least = (0 - range) % range, draw;
  do {
    if(!jello_entropy(&draw, sizeof draw)) {
      jdl_fail(c, "the system has no entropy");
      return;
    }
  } while(draw < least);
  jdl_return_i32(c, (int32_t)((int64_t)low + (int64_t)(draw % range)));
}
