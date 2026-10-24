// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What the tools share as hosts; running a module as a program (run.c), for jello and jelloc,
 * and showing its result and traps (host.c), also used by `jlo_tool run --print`. */
#ifndef JELLO_TOOLS_HOST_H
#define JELLO_TOOLS_HOST_H

#include <jello/internal.h>
#include <stdio.h>

/** Readies the standard streams for a program's bytes, before a host first uses them. On Windows
 * they pass bytes through untranslated, and a console shows them as UTF-8 until the program
 * exits. Elsewhere this does nothing. */
void jello_host_start(void);

/** Prints `r`, a result or thrown value of module `m`, to `out` as a program's output; a scalar
 * as a literal and a newline, an atom as its name, a Bytes as its text, followed by a newline only
 * when `bytes_newline` is set. A null result, such as that of a program without a tail
 * expression, prints nothing. */
void jello_print_host_result(const jello_bc_module* m, const jello_numeric_result* r, FILE* out,
                             int bytes_newline);

/** How jello_host_run runs a module. */
typedef struct jello_host_options {
  const char* tool; /**< Names the host in its messages; "jello: ...". */
  int jit;          /**< Create a JIT module for the run. A build without the JIT interprets,
                         and refuses `require_jit` and `jit_report`. */
  int require_jit;  /**< With `jit`; compile every function first, and fail if one cannot be. */
  int jit_report;   /**< With `jit`; report on stderr what the JIT compiled. */
  int raw_bytes;    /**< Print a Bytes result as is, without a newline. */
} jello_host_options;

/** Loads the module image `data` (`size` bytes, called `name` in messages), runs its entry with
 * the `nargs` program arguments `args` and prints the result on stdout. A load error or trap goes
 * to stderr.
 * @return The exit status; 0 when the module ran, 1 otherwise. */
int jello_host_run(const jello_host_options* options, const char* name, const uint8_t* data,
                   size_t size, int nargs, const char* const* args);

#endif
