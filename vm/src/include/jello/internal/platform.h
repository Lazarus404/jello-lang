// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What the VM and its libraries ask of the system in more than one place, behind one name. */
#ifndef JELLO_INTERNAL_PLATFORM_H
#define JELLO_INTERNAL_PLATFORM_H

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> /* Before windows.h, which would otherwise bring in the older winsock.h. */
#include <windows.h>
#include <direct.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* Apple's sockets are made SO_NOSIGPIPE instead; Windows has no SIGPIPE.   \
                        */
#endif

/** The processors this process may run on, at least 1. On Linux, those its affinity mask allows,
 * so a container given a set of the machine's processors (a cpuset) uses only those. */
static inline long jello_cpu_count(void) {
#ifdef _WIN32
  DWORD_PTR process_mask = 0, system_mask = 0;
  if(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask) {
    long allowed = 0;
    for(; process_mask; process_mask &= process_mask - 1) allowed++;
    return allowed;
  }
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors < 1 ? 1 : (long)info.dwNumberOfProcessors;
#else
#ifdef __linux__
  cpu_set_t set;
  int allowed = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : 0;
  if(allowed > 0) return allowed;
#endif
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n < 1 ? 1 : n;
#endif
}

/** The size of a page of memory, which jello_pages_map and jello_pages_protect work in. */
static inline size_t jello_page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

/** `bytes` rounded up to whole pages. */
static inline uint64_t jello_page_round_up(uint64_t bytes) {
  uint64_t page = (uint64_t)jello_page_size();
  return (bytes + page - 1u) & ~(page - 1u);
}

/** What jello_pages_protect lets code do with pages. */
typedef enum {
  JELLO_PAGES_NONE, /**< Nothing: any access faults, as a guard page's does. */
  JELLO_PAGES_RW,   /**< Read and write. */
  JELLO_PAGES_RX    /**< Read and run, as compiled code is. */
} jello_pages_access;

/** Maps `bytes`, rounded up to whole pages, of zeroed memory to read and write, at an address
 * that never moves; NULL when it cannot. Pages take physical memory only once touched. */
static inline void* jello_pages_map(size_t bytes) {
#ifdef _WIN32
  return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void* mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return mem == MAP_FAILED ? NULL : mem;
#endif
}

/** Reserves `bytes`, rounded up to whole pages, of address space that never moves, which
 * jello_pages_commit makes usable a run at a time; NULL when it cannot. Windows charges for a
 * page once it is committed, not once it is touched, so there only what is committed costs;
 * elsewhere this maps the whole, as jello_pages_map does, and committing is free. */
static inline void* jello_pages_reserve(size_t bytes) {
#ifdef _WIN32
  /* Readable and writable, not inaccessible, so that the system can grow a stack into it. */
  return VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
  return jello_pages_map(bytes);
#endif
}

/** Whether committing reserved pages costs anything, so that they are better committed only as
 * they are needed; only Windows charges for a page it has not been asked for yet. */
#ifdef _WIN32
#define JELLO_PAGES_COMMIT_COSTS 1
#else
#define JELLO_PAGES_COMMIT_COSTS 0
#endif

/** Makes the pages holding the `bytes` from `mem`, within one jello_pages_reserve reservation,
 * readable and writable, keeping what those already committed hold; whether it could. */
static inline int jello_pages_commit(void* mem, size_t bytes) {
#ifdef _WIN32
  return VirtualAlloc(mem, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
  (void)mem;
  (void)bytes;
  return 1;
#endif
}

/** Makes the `bytes` of pages from `mem`, within one jello_pages_reserve reservation, fault on
 * any access, as a guard page's do; whether it could. On Windows reserved pages already do, so
 * long as nothing commits them. */
static inline int jello_pages_guard(void* mem, size_t bytes) {
#ifdef _WIN32
  (void)mem;
  (void)bytes;
  return 1;
#else
  return mprotect(mem, bytes, PROT_NONE) == 0;
#endif
}

/** Readies the reserved page `page`, of jello_page_size() `bytes`, to hold the top of a native
 * stack growing down through the reservation below it; whether it could. Windows commits a
 * stack's pages as it grows into the guard page under them, as it does a thread's, so this
 * commits the page and makes the one under it that guard; elsewhere pages are there already. */
static inline int jello_pages_stack(void* page, size_t bytes) {
#ifdef _WIN32
  return jello_pages_commit(page, bytes) &&
         VirtualAlloc((char*)page - bytes, bytes, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD) != NULL;
#else
  (void)page;
  (void)bytes;
  return 1;
#endif
}

/** Sets what code may do with the `bytes` of pages from `mem`, within one jello_pages_map
 * mapping; whether it could. */
static inline int jello_pages_protect(void* mem, size_t bytes, jello_pages_access access) {
#ifdef _WIN32
  static const DWORD protection[] = {PAGE_NOACCESS, PAGE_READWRITE, PAGE_EXECUTE_READ};
  DWORD old;
  return VirtualProtect(mem, bytes, protection[access], &old) != 0;
#else
  static const int protection[] = {PROT_NONE, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC};
  return mprotect(mem, bytes, protection[access]) == 0;
#endif
}

/** Unmaps the whole of a jello_pages_map mapping, or jello_pages_reserve reservation, of
 * `bytes` at `mem`. */
static inline void jello_pages_unmap(void* mem, size_t bytes) {
#ifdef _WIN32
  (void)bytes;
  VirtualFree(mem, 0, MEM_RELEASE);
#else
  munmap(mem, bytes);
#endif
}

/** Allocates `bytes` aligned to `align`, a power of two; NULL when it cannot. Freed only by
 * jello_aligned_free. */
static inline void* jello_aligned_alloc(size_t align, size_t bytes) {
#ifdef _WIN32
  return _aligned_malloc(bytes, align);
#else
  return aligned_alloc(align, (bytes + align - 1u) / align * align);
#endif
}

/** Frees what jello_aligned_alloc allocated, or nothing for NULL. */
static inline void jello_aligned_free(void* p) {
#ifdef _WIN32
  _aligned_free(p);
#else
  free(p);
#endif
}

/** What separates the directories of a list such as JELLO_JDLL_PATH: `;` on Windows, where `:`
 * follows a drive letter, else `:`. */
#ifdef _WIN32
#define JELLO_PATH_LIST_SEP ';'
#else
#define JELLO_PATH_LIST_SEP ':'
#endif

/** What an executable's name ends with: `.exe` on Windows, else nothing. */
#ifdef _WIN32
#define JELLO_EXE_SUFFIX ".exe"
#else
#define JELLO_EXE_SUFFIX ""
#endif

/** Whether `c` separates a path's parts: `/`, or on Windows `/` or `\`. */
static inline int jello_is_slash(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

/** The last of `path`'s separators, NULL when it has none. */
static inline const char* jello_last_slash(const char* path) {
  const char* last = NULL;
  for(; *path; path++)
    if(jello_is_slash(*path)) last = path;
  return last;
}

/** Writes `path`'s separators as `/`, as Jello paths are, on Windows; elsewhere they are. */
static inline void jello_slashes_forward(char* path) {
#ifdef _WIN32
  for(; *path; path++)
    if(*path == '\\') *path = '/';
#else
  (void)path;
#endif
}

/** The length of the root `path` starts with, 0 when it is relative: `/`, or on Windows also a
 * drive's `C:/` or `C:`, or a share's `//server/share/`. */
static inline size_t jello_path_root_len(const char* path) {
#ifdef _WIN32
  if(((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':')
    return jello_is_slash(path[2]) ? 3u : 2u;
  if(jello_is_slash(path[0]) && jello_is_slash(path[1])) {
    size_t at = 2;
    for(int part = 0; part < 2 && path[at]; part++) {
      while(path[at] && !jello_is_slash(path[at])) at++;
      if(path[at]) at++;
    }
    return at;
  }
#endif
  return jello_is_slash(path[0]) ? 1u : 0u;
}

/** Makes directory `path`, readable and writable by all the umask allows; 0 when it could, else
 * -1 with errno set, EEXIST when it was already there. */
static inline int jello_mkdir(const char* path) {
#ifdef _WIN32
  return mkdir(path);
#else
  return mkdir(path, 0777);
#endif
}

/** Renames file `from` to `to`, replacing any file there, as POSIX rename does; 0 when it could,
 * else -1 with errno set. */
static inline int jello_rename_replace(const char* from, const char* to) {
#ifdef _WIN32
  if(MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
  errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO;
  return -1;
#else
  return rename(from, to);
#endif
}

/** Sets environment variable `name` to `value` for this process and those it starts; 0 when it
 * could. */
static inline int jello_setenv(const char* name, const char* value) {
#ifdef _WIN32
  return _putenv_s(name, value) == 0 ? 0 : -1;
#else
  return setenv(name, value, 1);
#endif
}

/** Removes `name` from the environment of this process and those it starts; 0 when it could. */
static inline int jello_unsetenv(const char* name) {
#ifdef _WIN32
  return _putenv_s(name, "") == 0 ? 0 : -1;
#else
  return unsetenv(name);
#endif
}

/** Writes `path` made absolute, with `.` and `..` resolved (and on POSIX symbolic links), into
 * `out` of `out_len` bytes, with `/` separators; whether the file exists and the path fits. */
static inline int jello_real_path(const char* path, char* out, size_t out_len) {
#ifdef _WIN32
  DWORD n = GetFullPathNameA(path, (DWORD)out_len, out, NULL);
  if(n == 0 || n >= out_len || GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return 0;
  jello_slashes_forward(out);
  return 1;
#else
  char resolved[PATH_MAX];
  if(!realpath(path, resolved) || strlen(resolved) >= out_len) return 0;
  memcpy(out, resolved, strlen(resolved) + 1u);
  return 1;
#endif
}

/** Writes the absolute path of the running executable into `out` of `out_len` bytes; whether it
 * could. */
static inline int jello_self_path(char* out, size_t out_len) {
  char path[PATH_MAX];
#if defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
  if(n == 0 || n >= sizeof path) return 0;
#elif defined(__APPLE__)
  uint32_t size = sizeof path;
  if(_NSGetExecutablePath(path, &size) != 0) return 0;
#else
  ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1u);
  if(n <= 0) return 0;
  path[n] = '\0';
#endif
  return jello_real_path(path, out, out_len);
}

#ifdef _WIN32
/** Writes `arg` into `out`, unless NULL, quoted where it must be so that the C runtime's parsing of
 * a command line gives it back as it is; the bytes it takes. */
static inline size_t jello_quote_arg(char* out, const char* arg) {
  size_t n = 0;
#define JELLO_PUT(c) (out ? (void)(out[n] = (c)) : (void)0, n++)
  if(*arg && !strpbrk(arg, " \t\n\v\"")) {
    for(; *arg; arg++) JELLO_PUT(*arg);
    return n;
  }
  JELLO_PUT('"');
  for(;; arg++) {
    size_t slashes = 0;
    for(; *arg == '\\'; arg++) slashes++;
    /* Backslashes double before a quote, theirs or the closing one, and stand as they are else. */
    if(*arg == '"' || !*arg) slashes *= 2;
    for(size_t i = 0; i < slashes; i++) JELLO_PUT('\\');
    if(!*arg) break;
    if(*arg == '"') JELLO_PUT('\\');
    JELLO_PUT(*arg);
  }
  JELLO_PUT('"');
#undef JELLO_PUT
  return n;
}

/** The command line of `argv`, NULL-terminated, each quoted by jello_quote_arg, with a space
 * between; NULL when out of memory. The caller frees it. */
static inline char* jello_command_line(char* const argv[]) {
  size_t len = 0;
  for(size_t i = 0; argv[i]; i++) len += jello_quote_arg(NULL, argv[i]) + 1u;
  char* line = (char*)malloc(len + 1u);
  if(!line) return NULL;
  size_t at = 0;
  for(size_t i = 0; argv[i]; i++) {
    if(i) line[at++] = ' ';
    at += jello_quote_arg(line + at, argv[i]);
  }
  line[at] = '\0';
  return line;
}

/** Whether `path`, with each extension of PATHEXT tried in turn when `name` has none, is a file;
 * the first that is in `out`, of `cap` bytes. */
static inline int jello_program_file(const char* path, const char* name, char* out, size_t cap) {
  const char* dot = strrchr(name, '.');
  int bare = !dot || jello_last_slash(dot);
  const char* exts = getenv("PATHEXT");
  if(!exts || !*exts) exts = ".COM;.EXE;.BAT;.CMD";
  for(const char* ext = bare ? exts : "";;) {
    const char* end = bare ? strchr(ext, ';') : NULL;
    int len = end ? (int)(end - ext) : (int)strlen(ext);
    int n = snprintf(out, cap, "%s%.*s", path, len, ext);
    DWORD attrs = n >= 0 && (size_t)n < cap ? GetFileAttributesA(out) : INVALID_FILE_ATTRIBUTES;
    if(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    if(!end) return 0;
    ext = end + 1;
  }
}

/** The file of program `program`, into `out` of `cap` bytes: `program` itself when it has a
 * directory, else the first match on PATH, never the current directory, each tried with the
 * extensions of PATHEXT when it has none. Returns 0, or ENOENT. */
static inline int jello_find_program(const char* program, char* out, size_t cap) {
  if(jello_last_slash(program) || jello_path_root_len(program) > 0)
    return jello_program_file(program, program, out, cap) ? 0 : ENOENT;
  const char* path = getenv("PATH");
  char dir_path[PATH_MAX];
  for(const char* dir = path ? path : ""; *dir;) {
    const char* end = strchr(dir, JELLO_PATH_LIST_SEP);
    int len = end ? (int)(end - dir) : (int)strlen(dir);
    int slash = len > 0 && !jello_is_slash(dir[len - 1]);
    int n = snprintf(dir_path, sizeof dir_path, "%.*s%s%s", len, dir, slash ? "/" : "", program);
    if(len > 0 && n >= 0 && (size_t)n < sizeof dir_path &&
       jello_program_file(dir_path, program, out, cap))
      return 0;
    if(!end) break;
    dir = end + 1;
  }
  return ENOENT;
}
#endif

/** Opens the shared library at `path`; NULL when it cannot, with why in `why` of `why_len`. */
static inline void* jello_dl_open(const char* path, char* why, size_t why_len) {
#ifdef _WIN32
  /* LoadLibrary takes `\` alone in a path. */
  char native[MAX_PATH];
  size_t len = strlen(path);
  if(len >= sizeof native) {
    snprintf(why, why_len, "path too long");
    return NULL;
  }
  for(size_t i = 0; i <= len; i++) native[i] = path[i] == '/' ? '\\' : path[i];
  HMODULE dl = LoadLibraryExA(native, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if(!dl) {
    DWORD err = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                             0, why, (DWORD)why_len, NULL);
    while(n > 0 && (why[n - 1] == '\n' || why[n - 1] == '\r' || why[n - 1] == '.')) why[--n] = '\0';
    if(n == 0) snprintf(why, why_len, "error %lu", (unsigned long)err);
  }
  return (void*)dl;
#else
  void* dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if(!dl) snprintf(why, why_len, "%s", dlerror());
  return dl;
#endif
}

/** The address of `name` in shared library `dl`; NULL when it has none. */
static inline void* jello_dl_symbol(void* dl, const char* name) {
#ifdef _WIN32
  FARPROC f = GetProcAddress((HMODULE)dl, name);
  void* p;
  memcpy(&p, &f, sizeof p); /* A function's address as data, which ISO C has no cast for. */
  return p;
#else
  return dlsym(dl, name);
#endif
}

/** Closes shared library `dl`. */
static inline void jello_dl_close(void* dl) {
#ifdef _WIN32
  FreeLibrary((HMODULE)dl);
#else
  dlclose(dl);
#endif
}

#endif /* JELLO_INTERNAL_PLATFORM_H */
