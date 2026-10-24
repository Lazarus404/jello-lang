// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* A library built for ABI 3, whose calls resolved against the program that loaded it: the VM
 * must refuse it, saying to rebuild it, rather than call through a table it never had. */

#include <jello/jdll.h>

static void old_noop(jdlo_ctx* c) { (void)c; }

static const jello_jdll_export OLD_EXPORTS[] = {
  {"noop", old_noop, (uint8_t)JELLO_T_VOID, 0, {0}, 0},
};

/* ABI 3's manifest, which ends before the table. */
typedef struct abi3_manifest {
  uint32_t magic;
  uint32_t abi_version;
  const char* library_name;
  const jello_jdll_export* exports;
  uint32_t nexports;
} abi3_manifest;

JELLO_JDLL_EXPORT const abi3_manifest jello_jdll_manifest_test_old_abi = {
  JELLO_JDLL_MANIFEST_MAGIC, 3u, "test_old_abi", OLD_EXPORTS, 1u,
};
