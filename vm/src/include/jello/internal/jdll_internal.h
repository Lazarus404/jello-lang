// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The layout of jdlo_ctx, shared by the native-call instruction and the jdl_* accessors. */

#ifndef JELLO_JDLL_INTERNAL_H
#define JELLO_JDLL_INTERNAL_H

#include <jello/internal/vm_internal.h>
#include <jello/jdll.h>

/** A native call in progress. op_call_native builds one on its C stack for the duration of the
 * export; arguments are read straight from the caller's registers. */
struct jdlo_ctx {
  jello_vm* vm;
  const jello_bc_module* m;
  reg_frame* caller_rf;
  const jello_insn* ins; /**< The CALL_NATIVE instruction; `imm` is the import, `b` the first
                          * argument register. */
  const jello_import_sig* sig;

  int returned;
  int failed;
  jello_trap_code trap; /**< The trap a failed built-in export asks for; TYPE_MISMATCH when 0. */
  int fail_said;        /**< Whether jdl_fail gave a message, which is then the VM's trap detail. */
  jello_type_kind ret_kind_given; /**< The kind of the last jdl_return_* called. */
  union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    jello_value dyn;
  } ret;
  /** A Bytes result, malloc'd by jdl_return_bytes_copy. A later jdl_return_* frees it, since
   * the last one wins; otherwise op_call_native frees it when the call fails or once it has built
   * the Bytes. */
  uint8_t* ret_bytes;
  uint32_t ret_bytes_len;
  /* Set by jdl_return_handle; a later jdl_return_* finalises the payload. op_call_native
   * allocates the handle once the export has returned and the thread is running again, since
   * an export may not allocate. */
  const jello_jdll_handle_class* handle_class;
  void* handle_payload;
  uint32_t handle_bytes;
};

/** The VM's functions as libraries call them; the loader hands each library this table. */
extern const jello_jdll_api jello_jdll_host_api;

/** The built-in `vm` library's manifest; jello_jdll_load returns it for "vm". */
extern const jello_jdll_manifest vm_builtin_manifest;
/** Whether `e` is the built-in library's. Its exports run with their VM running, not quiescent. */
int vm_builtin_export(const jello_jdll_export* e);

/** Drops the result a jdl_return_* left; frees a Bytes result and finalises a pending handle's
 * payload. */
void jdl_release_ret(jdlo_ctx* c);

#endif /* JELLO_JDLL_INTERNAL_H */
