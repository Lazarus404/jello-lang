// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The JIT entry points of a build without the JIT (JIT=0); no module can be created, so every
 * run is interpreted. */

#include <jello/internal/jit_internal.h>

int jello_jit_available(void) { return 0; }

jello_jit_module* jit_module_create(const jello_bc_module* m, int eager) { return NULL; }

jello_jit_module* jello_jit_module_create(const jello_bc_module* m) { return NULL; }

void jello_jit_module_destroy(jello_jit_module* jm) {}
