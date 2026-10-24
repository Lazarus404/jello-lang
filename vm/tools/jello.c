// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jello; loads a `.jlo`, validates it and runs its entry function.
 *
 *   jello [--no-jit | --require-jit] [--jit-report] [--schedulers <n>] [--jdll-path <dirs>]
 *         <program.jlo> [args...]
 *   jello --boot [--bundle] [--jdll-path <dirs>] [-o <executable>] <program.jlo>
 *   jello --jit-available
 *   jello --version
 *
 * Exits 1 on a read or load error, a trap or a JIT flag that cannot be honoured, 2 on a usage
 * error. `--jit-available` runs nothing and exits 0 when the VM was built with a JIT, else 1, and
 * `--version` (or `-v`) prints the VM's version.
 * `--schedulers` sets how many OS threads run the program's processes, as JELLO_SCHEDULERS does;
 * one a CPU by default.
 *
 * A program's `.jdll`s are looked for first in `jdll/` beside it, then beside it, then in the
 * `--jdll-path` directories (`;`-separated on Windows, else `:`), then where jello_jdll_load
 * looks. `--boot` writes a boot image (boot.h): this VM carrying the program, by default at its
 * path less `.jlo`; with `--bundle`, the `.jdll`s it uses go into `jdll/` beside it. A boot image
 * runs its program with all its arguments, as `jello` would run it, naming itself in messages by
 * the name it was run as, and looks for `.jdll`s first beside itself, as for a `.jlo`. */
#include <jello/internal.h>
#include <jello/internal/platform.h>
#include "boot.h"
#include "host.h"
#include "jello_version.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(FILE* out, int status) {
  fputs("usage: jello [--no-jit | --require-jit] [--jit-report] [--schedulers <n>] [--jdll-path "
        "<dirs>]\n"
        "             <program.jlo> [args...]\n"
        "       jello --boot [--bundle] [--jdll-path <dirs>] [-o <executable>] <program.jlo>\n"
        "       jello --jit-available\n"
        "       jello --version\n",
        out);
  return status;
}

static uint8_t* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if(!f) return NULL;
  uint8_t* data = NULL;
  long length = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
  if(length >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
     (data = (uint8_t*)malloc(length > 0 ? (size_t)length : 1)) != NULL) {
    *size = fread(data, 1, (size_t)length, f);
  }
  fclose(f);
  return data;
}

int main(int argc, char** argv) {
  jello_host_start();
  /* A boot image runs the program it carries, every argument the program's. */
  char self[PATH_MAX];
  int found_self = jello_executable_path(self);
  uint8_t* carried = NULL;
  size_t carried_size = 0;
  if(found_self && jello_boot_probe(self, &carried, &carried_size)) {
    /* Named as on the command line, less its directory and any `.exe`. */
    const char* slash = jello_last_slash(argv[0]);
    char name[256];
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : argv[0]);
#ifdef _WIN32
    size_t length = strlen(name), suffix = strlen(JELLO_EXE_SUFFIX);
    if(length > suffix && _stricmp(name + length - suffix, JELLO_EXE_SUFFIX) == 0)
      name[length - suffix] = '\0';
#endif
    jello_jdll_search_beside(self, NULL);
    jello_host_options options = {.tool = name, .jit = 1};
    int status = jello_host_run(&options, name, carried, carried_size, argc - 1,
                                (const char* const*)argv + 1);
    free(carried);
    return status;
  }
  int booting = argc > 1 && strcmp(argv[1], "--boot") == 0, bundle = 0;
  const char *path = NULL, *out = NULL, *dirs = NULL;
  int jit = 1, require_jit = 0, jit_report = 0, i = 1 + booting;
  for(; i < argc && !path; i++) {
    if(strcmp(argv[i], "--jdll-path") == 0 && i + 1 < argc) dirs = argv[++i];
    else if(booting && strcmp(argv[i], "--bundle") == 0) bundle = 1;
    else if(booting && strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    else if(booting) {
      if(argv[i][0] == '-') return usage(stderr, 2);
      path = argv[i];
    } else if(strcmp(argv[i], "--no-jit") == 0) jit = 0;
    else if(strcmp(argv[i], "--require-jit") == 0) require_jit = 1;
    else if(strcmp(argv[i], "--jit-report") == 0) jit_report = 1;
    else if(strcmp(argv[i], "--schedulers") == 0 && i + 1 < argc) {
      char* end = NULL;
      long n = strtol(argv[++i], &end, 10);
      if(n < 1 || *end != '\0') return usage(stderr, 2);
      if(jello_setenv("JELLO_SCHEDULERS", argv[i]) != 0) {
        perror("jello");
        return 1;
      }
    } else if(strcmp(argv[i], "--jit-available") == 0 && argc == 2)
      return jello_jit_available() ? 0 : 1;
    else if((strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) && argc == 2) {
      printf("jello %s\n", JELLO_VERSION);
      return 0;
    } else if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return usage(stdout, 0);
    else if(argv[i][0] == '-') return usage(stderr, 2);
    else path = argv[i];
  }
  if(!path || (booting && i != argc) || ((require_jit || jit_report) && !jit))
    return usage(stderr, 2);
  /* A boot image goes at the program's path less `.jlo` by default, as an executable is named. */
  char named[PATH_MAX];
  size_t len = strlen(path);
  if(booting && !out) {
    if(len <= 4 || strcmp(path + len - 4, ".jlo") != 0 ||
       snprintf(named, sizeof named, "%.*s%s", (int)(len - 4), path, JELLO_EXE_SUFFIX) >=
           (int)sizeof named) {
      fprintf(stderr, "jello: --boot without -o needs a program named *.jlo\n");
      return 2;
    }
    out = named;
  }
  // Without a JIT module the VM still compiles each run; only this turns the JIT off.
  if(!jit && jello_setenv("JELLO_JIT_DISABLE", "1") != 0) {
    perror("jello");
    return 1;
  }
  if(!jello_jdll_search_beside(path, dirs)) {
    fprintf(stderr, "jello: %s: path too long\n", path);
    return 1;
  }

  size_t size = 0;
  uint8_t* data = read_file(path, &size);
  if(!data) {
    fprintf(stderr, "jello: cannot read %s\n", path);
    return 1;
  }
  int status = 1;
  if(booting) {
    char error[PATH_MAX + 64];
    /* The program must load here, which also finds the libraries --bundle copies. */
    jello_bc_module* m = NULL;
    jello_bc_result loaded = jello_bc_read(data, size, &m);
    if(loaded.err) fprintf(stderr, "jello: %s: %s (offset %zu)\n", path, loaded.msg, loaded.offset);
    else if(!found_self) fputs("jello: cannot find its own executable\n", stderr);
    else if(jello_boot_create(self, data, size, out, error, sizeof error) != 0 ||
            (bundle && jello_boot_bundle(m, out, error, sizeof error) != 0))
      fprintf(stderr, "jello: %s\n", error);
    else status = 0;
    jello_bc_free(m);
  } else {
    /* A program's load errors, traps and throws are named "jello: ", which harnesses match. */
    jello_host_options options = {
        .tool = "jello", .jit = jit, .require_jit = require_jit, .jit_report = jit_report};
    status = jello_host_run(&options, path, data, size, argc - i, (const char* const*)argv + i);
  }
  free(data);
  return status;
}
