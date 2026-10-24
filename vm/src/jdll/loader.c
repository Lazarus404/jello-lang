// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Opening native libraries; finds and opens `<name>.jdll` and checks its jello_jdll_manifest. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/platform.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct jello_jdll_handle {
  void* dl;
  const jello_jdll_manifest* manifest;
  char* path;
};

#define JELLO_JDLL_SONAME_FMT "%s.jdll"

/** The directories the host searches first, as a JELLO_PATH_LIST_SEP list; NULL for none. */
static char* g_search;
/** Why the last jello_jdll_load on this thread failed. */
static _Thread_local char g_jdll_err[4608];

int jello_directory_of(const char* path, char* dir) {
  const char* slash = jello_last_slash(path);
  int len = slash ? (slash == path ? 1 : (int)(slash - path)) : 1;
  return snprintf(dir, PATH_MAX, "%.*s", len, slash ? path : ".") < PATH_MAX;
}

int jello_jdll_search_beside(const char* program, const char* dirs) {
  char dir[PATH_MAX], search[3 * PATH_MAX];
  int n = 0;
  if(program) {
    if(!jello_directory_of(program, dir)) return 0;
    n = snprintf(search, sizeof search, "%s/jdll%c%s", dir, JELLO_PATH_LIST_SEP, dir);
  }
  if(dirs && n >= 0 && n < (int)sizeof search) {
    if(n > 0) search[n++] = JELLO_PATH_LIST_SEP;
    n += snprintf(search + n, sizeof search - (size_t)n, "%s", dirs);
  }
  if(n < 0 || n >= (int)sizeof search) return 0;
  free(g_search);
  g_search = n > 0 ? strdup(search) : NULL;
  return 1;
}

const char* jello_jdll_last_error(void) { return g_jdll_err; }

int jello_executable_path(char* out) { return jello_self_path(out, PATH_MAX); }

/** A search for one library; the file's name, the path opened and its handle, the directories
 * tried, and why the last file found would not open. */
typedef struct search {
  char soname[256];
  char path[PATH_MAX];
  void* dl;
  char tried[4096];
  char unopened[sizeof "cannot open : " + PATH_MAX + 256];
} search;

/** Whether directory `dir` is among those `s` has tried. */
static int already_tried(const search* s, const char* dir, size_t dir_len) {
  for(const char* at = s->tried; *at;) {
    const char* end = strstr(at, ", ");
    size_t len = end ? (size_t)(end - at) : strlen(at);
    if(len == dir_len && memcmp(at, dir, len) == 0) return 1;
    at += len + (end ? 2u : 0u);
  }
  return 0;
}

/** Whether directory `dir` holds the library and it opens, its path then in `s->path`; each
 * directory tried is noted for the error. One that will not open, as one built for another
 * machine, is passed over. */
static int found_in(search* s, const char* dir, size_t dir_len) {
  if(dir_len == 0 || already_tried(s, dir, dir_len)) return 0;
  size_t used = strlen(s->tried);
  snprintf(s->tried + used, sizeof s->tried - used, "%s%.*s", used ? ", " : "", (int)dir_len, dir);
  int slash = !jello_is_slash(dir[dir_len - 1]);
  if(snprintf(s->path, sizeof s->path, "%.*s%s%s", (int)dir_len, dir, slash ? "/" : "",
              s->soname) >= (int)sizeof s->path)
    return 0;
  struct stat st;
  if(stat(s->path, &st) != 0) return 0;
  char why[256];
  s->dl = jello_dl_open(s->path, why, sizeof why);
  if(!s->dl) snprintf(s->unopened, sizeof s->unopened, "cannot open %s: %s", s->path, why);
  return s->dl != NULL;
}

/** Whether one of the directories of list `dirs` (which may be NULL) holds the library. */
static int found_in_each(search* s, const char* dirs) {
  while(dirs && *dirs) {
    const char* sep = strchr(dirs, JELLO_PATH_LIST_SEP);
    size_t len = sep ? (size_t)(sep - dirs) : strlen(dirs);
    if(found_in(s, dirs, len)) return 1;
    dirs = sep ? sep + 1 : NULL;
  }
  return 0;
}

/** Finds the library, in the host's directories, the working directory, JELLO_JDLL_PATH, the
 * VM's directory and then `../lib` from there, where an installed VM keeps them. */
static int find(search* s) {
  if(found_in_each(s, g_search) || found_in(s, ".", 1) ||
     found_in_each(s, getenv("JELLO_JDLL_PATH")))
    return 1;
  char self[PATH_MAX], dir[PATH_MAX], lib[PATH_MAX];
  if(!jello_executable_path(self) || !jello_directory_of(self, dir)) return 0;
  if(found_in(s, dir, strlen(dir))) return 1;
  char* slash = (char*)jello_last_slash(dir);
  if(!slash || slash == dir) return 0;
  *slash = '\0';
  return snprintf(lib, sizeof lib, "%s/lib", dir) < (int)sizeof lib &&
         found_in(s, lib, strlen(lib));
}

jello_jdll_handle* jello_jdll_load(const char* library_name) {
  g_jdll_err[0] = '\0';
  if(!library_name || !library_name[0]) {
    snprintf(g_jdll_err, sizeof g_jdll_err, "a .jdll needs a name");
    return NULL;
  }
  if(strcmp(library_name, "vm") == 0) {
    jello_jdll_handle* h = (jello_jdll_handle*)calloc(1, sizeof(jello_jdll_handle));
    if(!h) {
      snprintf(g_jdll_err, sizeof g_jdll_err, "out of memory");
      return NULL;
    }
    h->manifest = &vm_builtin_manifest;
    return h;
  }
  search s = {.tried = "", .unopened = ""};
  snprintf(s.soname, sizeof s.soname, JELLO_JDLL_SONAME_FMT, library_name);
  if(!find(&s)) {
    if(s.unopened[0]) snprintf(g_jdll_err, sizeof g_jdll_err, "%s", s.unopened);
    else snprintf(g_jdll_err, sizeof g_jdll_err, "cannot find %s in %s", s.soname, s.tried);
    return NULL;
  }
  void* dl = s.dl;

  char sym[256];
  snprintf(sym, sizeof sym, "jello_jdll_manifest_%s", library_name);
  const jello_jdll_manifest* manifest = (const jello_jdll_manifest*)jello_dl_symbol(dl, sym);
  if(!manifest || manifest->magic != JELLO_JDLL_MANIFEST_MAGIC || !manifest->library_name ||
     strcmp(manifest->library_name, library_name) != 0) {
    jello_dl_close(dl);
    snprintf(g_jdll_err, sizeof g_jdll_err, "%s is not a %s library", s.path, library_name);
    return NULL;
  }
  if(manifest->abi_version != JELLO_JDLL_ABI_VERSION || !manifest->api) {
    snprintf(g_jdll_err, sizeof g_jdll_err,
             "%s was built for ABI %u; rebuild it against this VM's jello/jdll.h (ABI %u)", s.path,
             (unsigned)manifest->abi_version, (unsigned)JELLO_JDLL_ABI_VERSION);
    jello_dl_close(dl);
    return NULL;
  }
  /* Every load stores the same table, so a library loaded twice, or by two threads, agrees. */
  __atomic_store_n(manifest->api, &jello_jdll_host_api, __ATOMIC_RELEASE);

  jello_jdll_handle* h = (jello_jdll_handle*)malloc(sizeof(jello_jdll_handle));
  char* path = strdup(s.path);
  if(!h || !path) {
    free(h);
    free(path);
    jello_dl_close(dl);
    snprintf(g_jdll_err, sizeof g_jdll_err, "out of memory");
    return NULL;
  }
  h->dl = dl;
  h->manifest = manifest;
  h->path = path;
  return h;
}

const char* jello_jdll_path(const jello_jdll_handle* h) { return h->path; }

void jello_jdll_unload(jello_jdll_handle* h) {
  if(!h) return;
  if(h->dl) jello_dl_close(h->dl);
  free(h->path);
  free(h);
}

void* jello_jdll_symbol(const jello_jdll_handle* h, const char* name) {
  return h && h->dl ? jello_dl_symbol(h->dl, name) : NULL;
}

const jello_jdll_export* jello_jdll_find_export(const jello_jdll_handle* h, const char* name) {
  if(!h || !h->manifest) return NULL;
  for(uint32_t i = 0; i < h->manifest->nexports; i++) {
    if(strcmp(h->manifest->exports[i].name, name) == 0) return &h->manifest->exports[i];
  }
  return NULL;
}
