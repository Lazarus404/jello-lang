// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Boot images: a copy of jello with a `.jlo` module appended, which runs that module as itself.
 * The file ends with a 24-byte footer of little-endian u64s, the module's size and offset, then
 * the magic `JLYBOOT1`. The executable's own headers are left as they are, so the image is still
 * a valid ELF or Mach-O program; the loader never maps the bytes after them. */
#ifndef JELLO_TOOLS_BOOT_H
#define JELLO_TOOLS_BOOT_H

#include <jello.h>
#include <stddef.h>
#include <stdint.h>

/** The footer's magic, its last eight bytes. */
#define JELLO_BOOT_MAGIC "JLYBOOT1"
/** The footer's size in bytes. */
#define JELLO_BOOT_FOOTER_SIZE 24u

/** Reads the module that the executable at `path` carries.
 * @return 1 with the module in `*data` (malloc'd) and `*size`, or 0 when it carries none or
 *         cannot be read */
int jello_boot_probe(const char* path, uint8_t** data, size_t* size);

/** Writes the executable `out`: the VM at `vm`, which carries no module, then the module of
 * `size` bytes at `module` and the footer, made executable. It is written to a new file and
 * renamed into place, so an earlier `out` that has run is never changed in place.
 * @return 0, or -1 with the reason in `error` */
int jello_boot_create(const char* vm, const uint8_t* module, size_t size, const char* out,
                      char* error, size_t error_size);

/** Copies each `.jdll` that the loaded module `m` uses into the directory `jdll/` beside `out`,
 * made if missing, where `out`, when it is a boot image, looks first. Each is written to a new
 * file and renamed into place, as `out` is.
 * @return 0, or -1 with the reason in `error` */
int jello_boot_bundle(const jello_bc_module* m, const char* out, char* error, size_t error_size);

#endif
