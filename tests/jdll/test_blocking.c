// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* A test .jdll whose exports block: a process calling one parks while the
 * blocking pool makes the call. One returns a handle, so that a call whose result is dropped can
 * be seen to finalise it. */

#include <jello/jdll.h>

#include <stdatomic.h>
#include <unistd.h>

static _Atomic int32_t finalized;

static void count_finalize(void* payload) {
  (void)payload;
  atomic_fetch_add(&finalized, 1);
}

static const jello_jdll_handle_class NAPPED = {"napped", count_finalize, NULL};

static void nap(int32_t ms) {
  if(ms > 0) usleep((unsigned)ms * 1000u);
}

/* Sleeps argument 0 milliseconds; true. */
static void tb_nap_ms(jdlo_ctx* c) {
  nap(jdl_arg_i32(c, 0));
  jdl_return_bool(c, 1);
}

/* Sleeps argument 0 milliseconds; a handle that counts its finalising. */
static void tb_nap_handle_ms(jdlo_ctx* c) {
  nap(jdl_arg_i32(c, 0));
  jdl_return_handle(c, &NAPPED, (void*)&finalized, 1);
}

/* How many handles nap_handle_ms made have been finalised. */
static void tb_finalized(jdlo_ctx* c) {
  jdl_return_i32(c, atomic_load(&finalized));
}

#define I (uint8_t)JELLO_T_I32
static const jello_jdll_export TB_EXPORTS[] = {
  {"nap_ms", tb_nap_ms, (uint8_t)JELLO_T_BOOL, 1, {I}, JELLO_JDLL_BLOCKING},
  {"nap_handle_ms", tb_nap_handle_ms, (uint8_t)JELLO_T_DYNAMIC, 1, {I}, JELLO_JDLL_BLOCKING},
  {"finalized", tb_finalized, I, 0, {0}, 0},
};

JELLO_JDLL_LIBRARY(test_blocking, TB_EXPORTS);
