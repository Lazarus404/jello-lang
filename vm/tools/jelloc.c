// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jelloc, the Jello compiler as a native program; the VM with a compiled compiler module embedded
 * (jelloc_image.S).
 *
 *   jelloc <input.jello> --out <path> [options]   (jelloc --help lists them)
 *
 * Exits 1 when the program does not compile or traps, 2 on a bad command line. */
#include <jello/internal.h>
#include "host.h"

extern const uint8_t jello_jelloc_image[];
extern const uint8_t jello_jelloc_image_end[];

int main(int argc, char** argv) {
  jello_host_start();
  jello_host_options options = {.tool = "jelloc", .jit = 1, .raw_bytes = 1};
  return jello_host_run(&options, "jelloc", jello_jelloc_image,
                        (size_t)(jello_jelloc_image_end - jello_jelloc_image), argc - 1,
                        (const char* const*)argv + 1);
}
