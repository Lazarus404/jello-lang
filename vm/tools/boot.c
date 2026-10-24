// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Boot images (boot.h): making one, and finding the module the running executable carries. */
#define _DARWIN_C_SOURCE 1

#include "boot.h"

#include <jello/internal.h>
#include <jello/internal/platform.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for(unsigned i = 0; i < 8u; i++) v |= (uint64_t)p[i] << (8u * i);
  return v;
}

static void put_u64(uint8_t* p, uint64_t v) {
  for(unsigned i = 0; i < 8u; i++) p[i] = (uint8_t)(v >> (8u * i));
}

/* Whether the open file F ends with a valid footer, the module it describes then at OFFSET and
 * SIZE. */
static int footer(FILE* f, uint64_t* offset, uint64_t* size) {
  off_t end = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : -1;
  uint8_t bytes[JELLO_BOOT_FOOTER_SIZE];
  if(end < (off_t)JELLO_BOOT_FOOTER_SIZE ||
     fseeko(f, end - (off_t)JELLO_BOOT_FOOTER_SIZE, SEEK_SET) != 0 ||
     fread(bytes, 1, sizeof bytes, f) != sizeof bytes ||
     memcmp(bytes + 16, JELLO_BOOT_MAGIC, 8) != 0)
    return 0;
  *size = get_u64(bytes);
  *offset = get_u64(bytes + 8);
  uint64_t limit = (uint64_t)end - JELLO_BOOT_FOOTER_SIZE;
  return *size > 0 && *offset <= limit && *size <= limit - *offset;
}

int jello_boot_probe(const char* path, uint8_t** data, size_t* size) {
  *data = NULL;
  *size = 0;
  FILE* f = fopen(path, "rb");
  if(!f) return 0;
  uint64_t offset = 0, length = 0;
  int found = footer(f, &offset, &length) && length <= SIZE_MAX &&
              fseeko(f, (off_t)offset, SEEK_SET) == 0 &&
              (*data = (uint8_t*)malloc((size_t)length)) != NULL &&
              fread(*data, 1, (size_t)length, f) == (size_t)length;
  fclose(f);
  if(!found) {
    free(*data);
    *data = NULL;
    return 0;
  }
  *size = (size_t)length;
  return 1;
}

/* Copies the rest of IN to OUT. */
static int copy(FILE* in, FILE* out) {
  uint8_t buffer[65536];
  size_t got;
  while((got = fread(buffer, 1, sizeof buffer, in)) > 0) {
    if(fwrite(buffer, 1, got, out) != got) return 0;
  }
  return !ferror(in);
}

static int fail(char* error, size_t error_size, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
  return -1;
}

/* Opens a new file beside OUT to write it through, its name in TEMP. */
static FILE* begin(const char* out, char temp[PATH_MAX], char* error, size_t error_size) {
  if(snprintf(temp, PATH_MAX, "%s.boot-XXXXXX", out) >= PATH_MAX) {
    fail(error, error_size, "%s: path too long", out);
    return NULL;
  }
  int fd = mkstemp(temp);
  FILE* made = fd >= 0 ? fdopen(fd, "wb") : NULL;
  if(!made) {
    fail(error, error_size, "cannot write %s: %s", out, strerror(errno));
    if(fd >= 0) {
      close(fd);
      unlink(temp);
    }
  }
  return made;
}

/* Closes MADE, the file BEGIN opened as TEMP, and renames it to OUT with MODE when WRITTEN says
 * every write succeeded; otherwise removes it. */
static int finish(FILE* made, const char* temp, int written, const char* out, mode_t mode,
                  char* error, size_t error_size) {
  errno = 0;
  if(fclose(made) != 0) written = 0;
  if(written && chmod(temp, mode) == 0 && jello_rename_replace(temp, out) == 0) return 0;
  fail(error, error_size, "cannot write %s: %s", out, errno ? strerror(errno) : "short write");
  unlink(temp);
  return -1;
}

int jello_boot_create(const char* vm, const uint8_t* module, size_t size, const char* out,
                      char* error, size_t error_size) {
  FILE* base = fopen(vm, "rb");
  if(!base) return fail(error, error_size, "cannot read %s: %s", vm, strerror(errno));
  char temp[PATH_MAX];
  FILE* made = begin(out, temp, error, error_size);
  int status = -1;
  if(made) {
    int written = copy(base, made);
    off_t offset = ftello(made);
    uint8_t bytes[JELLO_BOOT_FOOTER_SIZE];
    put_u64(bytes, size);
    put_u64(bytes + 8, (uint64_t)offset);
    memcpy(bytes + 16, JELLO_BOOT_MAGIC, 8);
    written = written && offset > 0 && fwrite(module, 1, size, made) == size &&
              fwrite(bytes, 1, sizeof bytes, made) == sizeof bytes;
    status = finish(made, temp, written, out, 0755, error, error_size);
  }
  fclose(base);
  return status;
}

int jello_boot_bundle(const jello_bc_module* m, const char* out, char* error, size_t error_size) {
  char beside[PATH_MAX], dir[PATH_MAX];
  if(!jello_directory_of(out, beside) ||
     snprintf(dir, sizeof dir, "%s/jdll", beside) >= (int)sizeof dir)
    return fail(error, error_size, "%s: path too long", out);
  if(m->njdll_handles > 0 && jello_mkdir(dir) != 0 && errno != EEXIST)
    return fail(error, error_size, "cannot make %s: %s", dir, strerror(errno));
  const jello_jdll_handle* const* handles = (const jello_jdll_handle* const*)m->jdll_handles;
  for(uint32_t i = 0; i < m->njdll_handles; i++) {
    const char* from = jello_jdll_path(handles[i]);
    int seen = !from; // The built-in library has no file.
    for(uint32_t j = 0; j < i && !seen; j++) {
      const char* other = jello_jdll_path(handles[j]);
      seen = other && strcmp(other, from) == 0;
    }
    if(seen) continue;
    const char* slash = jello_last_slash(from);
    char to[PATH_MAX], temp[PATH_MAX];
    if(snprintf(to, sizeof to, "%s/%s", dir, slash ? slash + 1 : from) >= (int)sizeof to)
      return fail(error, error_size, "%s: path too long", dir);
    FILE* in = fopen(from, "rb");
    if(!in) return fail(error, error_size, "cannot read %s: %s", from, strerror(errno));
    FILE* made = begin(to, temp, error, error_size);
    int status = made ? finish(made, temp, copy(in, made), to, 0755, error, error_size) : -1;
    fclose(in);
    if(status != 0) return -1;
  }
  return 0;
}
