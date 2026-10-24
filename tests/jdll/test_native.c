// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* A small, hand-written test .jdll, not the std JDLL: the VM's fixtures
 * round-trip typed and Dynamic values through it, and its slow native call
 * proves that a native call runs quiescent, so the world can stop while it
 * sleeps.
 */

#include <jello/jdll.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void tn_echo_i32(jdlo_ctx* c) {
  jdl_return_i32(c, jdl_arg_i32(c, 0));
}

static void tn_sum2_i32(jdlo_ctx* c) {
  jdl_return_i32(c, jdl_arg_i32(c, 0) + jdl_arg_i32(c, 1));
}

static void tn_echo_bytes(jdlo_ctx* c) {
  jdl_return_bytes_copy(c, jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0));
}

static void tn_echo_dyn(jdlo_ctx* c) {
  jdl_return_value(c, jdl_arg_value(c, 0));
}

/* Deliberately does *not* call jello_vm_blocking itself -- op_call_native
 * (call_native.c) already wraps every native invocation in quiescent state
 * unconditionally, and this export exists specifically to prove that
 * wrapping alone is sufficient. */
static void tn_sleep_ms(jdlo_ctx* c) {
  int32_t ms = jdl_arg_i32(c, 0);
  if(ms > 0) usleep((unsigned)ms * 1000u);
  jdl_return_bool(c, 1);
}

/* Results replaced or abandoned before the export returns: the last jdl_return_* wins, and
 * nothing an earlier one made may leak. The 777-byte results are easy to spot in `leaks`. */
static const uint8_t TN_FILLER[777];

static void tn_bytes_then_fail(jdlo_ctx* c) {
  jdl_return_bytes_copy(c, TN_FILLER, sizeof TN_FILLER);
  jdl_fail(c, "bytes_then_fail");
}

static void tn_bytes_twice(jdlo_ctx* c) {
  const uint8_t last[1] = {42};
  jdl_return_bytes_copy(c, TN_FILLER, sizeof TN_FILLER);
  jdl_return_bytes_copy(c, last, sizeof last);
}

static void tn_bytes_wrong_kind(jdlo_ctx* c) {
  jdl_return_bytes_copy(c, TN_FILLER, sizeof TN_FILLER);
}

/* A class without retain, whose handles cannot leave their process. */
static const jello_jdll_handle_class TN_PAYLOAD_CLASS = {.name = "tn_payload", .finalize = free};

static void tn_handle_then_i32(jdlo_ctx* c) {
  jdl_return_handle(c, &TN_PAYLOAD_CLASS, malloc(777), 777);
  jdl_return_i32(c, 7);
}

/* Returns nothing, as a Void export may, so its call stores null. */
static void tn_nothing(jdlo_ctx* c) { (void)c; }

static const jello_jdll_export TEST_NATIVE_EXPORTS[] = {
  {"echo_i32", tn_echo_i32, (uint8_t)JELLO_T_I32, 1, {(uint8_t)JELLO_T_I32}},
  {"sum2_i32", tn_sum2_i32, (uint8_t)JELLO_T_I32, 2, {(uint8_t)JELLO_T_I32, (uint8_t)JELLO_T_I32}},
  {"echo_bytes", tn_echo_bytes, (uint8_t)JELLO_T_BYTES, 1, {(uint8_t)JELLO_T_BYTES}},
  {"echo_dyn", tn_echo_dyn, (uint8_t)JELLO_T_DYNAMIC, 1, {(uint8_t)JELLO_T_DYNAMIC}},
  {"sleep_ms", tn_sleep_ms, (uint8_t)JELLO_T_BOOL, 1, {(uint8_t)JELLO_T_I32}},
  {"bytes_then_fail", tn_bytes_then_fail, (uint8_t)JELLO_T_BYTES, 0, {0}},
  {"bytes_twice", tn_bytes_twice, (uint8_t)JELLO_T_BYTES, 0, {0}},
  {"bytes_wrong_kind", tn_bytes_wrong_kind, (uint8_t)JELLO_T_I32, 0, {0}},
  {"handle_then_i32", tn_handle_then_i32, (uint8_t)JELLO_T_I32, 0, {0}},
  {"nothing", tn_nothing, (uint8_t)JELLO_T_VOID, 0, {0}},
};

JELLO_JDLL_LIBRARY(test_native, TEST_NATIVE_EXPORTS);
