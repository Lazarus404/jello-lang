// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Child commands on Windows, as command.c runs them elsewhere: command_start runs a program
 * without waiting for it, its standard input the null device and its standard output and error
 * captured to temporary files that go when closed, and command_wait waits for it; a handle that
 * is collected first ends its child.
 *
 * A program named without a separator is looked for in each PATH directory, as POSIX does, not
 * the current directory first, as Windows does, with each PATHEXT extension when it has none of
 * its own. A batch file (.bat or .cmd) runs through cmd.exe, which takes no argument holding a
 * double quote, a percent sign or a line break, as it would read them as its own. The arguments
 * are quoted so that the C runtime's parsing gives the child them as they are.
 *
 * The status is the exit code & 255, or for a child a crash ended, 128 + the POSIX signal for the
 * crash: 139 for an access violation (SIGSEGV), 130 for Ctrl-C (SIGINT), and 134 (SIGABRT) for
 * any other; 127 for a program that cannot run, its reason on stderr, as command.c's. */

#include "std.h"

#include <jello/internal/platform.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct std_command {
  jdl_refs refs;
  struct std_command *prev, *next;
  HANDLE process; /* NULL once waited for, or when it never started. */
  DWORD pid;      /* The child's, which command_next finds it by. */
  HANDLE out, err;
  int waited;
  int32_t status;
  uint8_t* out_data;
  uint8_t* err_data;
  uint32_t out_len, err_len;
} std_command;

/* The handles whose children may still be running, and the lock over it and their results. */
static std_command* live = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void unlist(std_command* p) {
  if(p->prev) p->prev->next = p->next;
  else if(live == p) live = p->next;
  if(p->next) p->next->prev = p->prev;
  p->prev = p->next = NULL;
}

static void close_handle(HANDLE* h) {
  if(*h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
  *h = NULL;
}

static void command_free(void* payload) {
  if(!jdl_release(payload)) return;
  std_command* p = (std_command*)payload;
  pthread_mutex_lock(&lock);
  unlist(p);
  if(!p->waited && p->process) {
    TerminateProcess(p->process, 1);
    WaitForSingleObject(p->process, INFINITE);
  }
  pthread_mutex_unlock(&lock);
  close_handle(&p->process);
  close_handle(&p->out);
  close_handle(&p->err);
  free(p->out_data);
  free(p->err_data);
  free(p);
}

static const jello_jdll_handle_class COMMAND = {"command", command_free, jdl_retain};

/* The status a POSIX shell would report for a process that exited with Windows exit CODE. */
static int32_t exit_status(unsigned long code) {
  if(code == 0xC0000005ul) return 139; /* STATUS_ACCESS_VIOLATION */
  if(code == 0xC000013Aul) return 130; /* STATUS_CONTROL_C_EXIT */
  if(code >= 0xC0000000ul) return 134;
  return (int32_t)(code & 255u);
}

/* A temporary file, to read and write, that a child may inherit, gone once its last handle
 * closes; NULL on failure. */
static HANDLE temp_file(void) {
  char dir[STD_MAX_PATH], path[MAX_PATH];
  if(!std_temp_dir(dir, sizeof dir) || !GetTempFileNameA(dir, "jlo", 0, path)) return NULL;
  SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if(h == INVALID_HANDLE_VALUE) {
    DeleteFileA(path);
    return NULL;
  }
  return h;
}

/* A growing string. */
typedef struct text {
  char* data;
  size_t len, cap;
} text;

static int append(text* t, const char* s, size_t n) {
  if(t->len + n + 1 > t->cap) {
    size_t cap = t->cap ? t->cap * 2 : 256;
    while(cap < t->len + n + 1) cap *= 2;
    char* grown = (char*)realloc(t->data, cap);
    if(!grown) return 0;
    t->data = grown;
    t->cap = cap;
  }
  memcpy(t->data + t->len, s, n);
  t->len += n;
  t->data[t->len] = '\0';
  return 1;
}

/* Whether PATH ends with extension EXT, ignoring case. */
static int has_extension(const char* path, const char* ext) {
  size_t n = strlen(path), e = strlen(ext);
  return n > e && _stricmp(path + n - e, ext) == 0;
}

/* The command line to run PROGRAM, found from ARGV[0], with ARGV, into *LINE, and the program
 * to name to CreateProcess in APPLICATION; 0, or why it cannot be run. */
static int command_line(const char* program, char** argv, char** line, char* application,
                        size_t cap) {
  int batch = has_extension(program, ".bat") || has_extension(program, ".cmd");
  if(!batch) {
    if(snprintf(application, cap, "%s", program) >= (int)cap) return ENAMETOOLONG;
    *line = jello_command_line(argv);
    return *line ? 0 : ENOMEM;
  }
  /* cmd.exe reads its line after /c as a command of its own, the program quoted as a whole. */
  char system[MAX_PATH];
  UINT n = GetSystemDirectoryA(system, sizeof system);
  if(n == 0 || n >= sizeof system || snprintf(application, cap, "%s\\cmd.exe", system) >= (int)cap)
    return ENAMETOOLONG;
  for(size_t i = 1; argv[i]; i++)
    if(strpbrk(argv[i], "\"%\r\n")) return EINVAL;
  text t = {0};
  char quoted[STD_MAX_PATH * 2];
  int ok = jello_quote_arg(NULL, program) < sizeof quoted;
  if(ok) quoted[jello_quote_arg(quoted, program)] = '\0';
  ok = ok && append(&t, "cmd.exe /d /e:on /v:off /c \"", 28) && append(&t, quoted, strlen(quoted));
  for(size_t i = 1; ok && argv[i]; i++) {
    ok = append(&t, " \"", 2) && append(&t, argv[i], strlen(argv[i])) && append(&t, "\"", 1);
  }
  ok = ok && append(&t, "\"", 1);
  if(!ok) free(t.data);
  *line = ok ? t.data : NULL;
  return ok ? 0 : ENOMEM;
}

/* Whether environment entry ENTRY ("NAME=VALUE") is for NAME, of LEN bytes, as Windows matches
 * names, ignoring case. */
static int names(const char* entry, const char* name, size_t len) {
  return _strnicmp(entry, name, len) == 0 && entry[len] == '=';
}

/* The inherited environment with CHANGES applied, as a block for CreateProcess; NULL if out of
 * memory. */
static char* environment_block(char** changes, size_t count) {
  char* inherited = GetEnvironmentStringsA();
  if(!inherited) return NULL;
  size_t n = 0;
  for(const char* e = inherited; *e; e += strlen(e) + 1) n++;
  const char** entries = (const char**)calloc(n + count + 1, sizeof(char*));
  if(!entries) {
    FreeEnvironmentStringsA(inherited);
    return NULL;
  }
  n = 0;
  for(const char* e = inherited; *e; e += strlen(e) + 1) entries[n++] = e;
  for(size_t i = 0; i < count; i++) {
    const char* eq = strchr(changes[i], '=');
    size_t len = eq ? (size_t)(eq - changes[i]) : strlen(changes[i]);
    size_t kept = 0;
    for(size_t j = 0; j < n; j++) {
      /* The entries for the drives' working directories, "=C:=C:\\...", are no one's to change. */
      if(entries[j][0] == '=' || !names(entries[j], changes[i], len)) entries[kept++] = entries[j];
    }
    n = kept;
    if(eq) entries[n++] = changes[i];
  }
  text block = {0};
  int ok = 1;
  for(size_t i = 0; ok && i < n; i++) ok = append(&block, entries[i], strlen(entries[i]) + 1);
  ok = ok && append(&block, "", 1);
  free(entries);
  FreeEnvironmentStringsA(inherited);
  if(!ok) free(block.data);
  return ok ? block.data : NULL;
}

void* std_spawn(char** argv, const char* env, void* in, void* out, void* err, char* reason,
                size_t cap) {
  char program[STD_MAX_PATH], application[STD_MAX_PATH];
  char* line = NULL;
  int error = jello_find_program(argv[0], program, sizeof program);
  if(!error) error = command_line(program, argv, &line, application, sizeof application);
  if(error) {
    snprintf(reason, cap, "cannot run %s: %s", argv[0],
             error == EINVAL ? "a batch file's argument holds \", % or a line break"
                             : strerror(error));
    return NULL;
  }
  /* Only the three standard handles are inherited, not every inheritable handle open now. */
  HANDLE handles[3] = {(HANDLE)in, (HANDLE)out, (HANDLE)err};
  DWORD nhandles = 3;
  if(handles[2] == handles[1]) nhandles = 2;
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
  LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(bytes);
  STARTUPINFOEXA info;
  memset(&info, 0, sizeof info);
  info.StartupInfo.cb = sizeof info;
  info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  info.StartupInfo.hStdInput = handles[0];
  info.StartupInfo.hStdOutput = handles[1];
  info.StartupInfo.hStdError = handles[2];
  PROCESS_INFORMATION made;
  memset(&made, 0, sizeof made);
  BOOL started = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &bytes) &&
                 UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                           nhandles * sizeof(HANDLE), NULL, NULL);
  if(started) {
    info.lpAttributeList = attrs;
    started = CreateProcessA(application, line, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                             (LPVOID)env, NULL, &info.StartupInfo, &made);
    DeleteProcThreadAttributeList(attrs);
  }
  DWORD why = started ? 0 : GetLastError();
  free(attrs);
  free(line);
  if(!started) {
    snprintf(reason, cap, "cannot run %s: %s", argv[0],
             why == ERROR_FILE_NOT_FOUND   ? strerror(ENOENT)
             : why == ERROR_ACCESS_DENIED  ? strerror(EACCES)
             : why == ERROR_BAD_EXE_FORMAT ? "Exec format error"
                                           : "the system could not start it");
    return NULL;
  }
  CloseHandle(made.hThread);
  return made.hProcess;
}

/* Records that P ended with STATUS and only MESSAGE on its stderr. */
static void failed(std_command* p, int32_t status, const char* message) {
  size_t len = strlen(message);
  p->waited = 1;
  p->status = status;
  free(p->err_data);
  p->err_data = (uint8_t*)malloc(len + 2);
  p->err_len = p->err_data ? (uint32_t)len + 1u : 0u;
  if(p->err_data) {
    memcpy(p->err_data, message, len);
    p->err_data[len] = '\n';
  }
}

void std_command_start(jdlo_ctx* c) {
  char *argv_store = NULL, *env_store = NULL;
  size_t argc = 0, envc = 0;
  char** argv = std_entries(c, 0, &argv_store, &argc);
  char** changes = std_entries(c, 1, &env_store, &envc);
  char* env = changes ? environment_block(changes, envc) : NULL;
  std_command* p = (std_command*)calloc(1, sizeof(std_command));
  if(p) p->refs = 1;
  if(!argv || argc == 0 || !env || !p) {
    jdl_fail(c, argv && argc > 0 && changes ? "command_start: out of memory"
                                            : "command_start: argv and env must be NUL-terminated "
                                              "entries, argv at least one");
  } else {
    char reason[STD_MAX_PATH + 128];
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    HANDLE in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                            OPEN_EXISTING, 0, NULL);
    p->out = temp_file();
    p->err = temp_file();
    if(in == INVALID_HANDLE_VALUE || !p->out || !p->err) {
      snprintf(reason, sizeof reason, "cannot run %s: %s", argv[0], strerror(EIO));
    } else {
      /* Listed before the lock is released, so that command_next finds a child that ends at
       * once as this one. */
      pthread_mutex_lock(&lock);
      p->process = std_spawn(argv, env, in, p->out, p->err, reason, sizeof reason);
      if(p->process) {
        p->pid = GetProcessId(p->process);
        p->next = live;
        if(live) live->prev = p;
        live = p;
      }
      pthread_mutex_unlock(&lock);
    }
    if(in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if(!p->process) {
      close_handle(&p->out);
      close_handle(&p->err);
      failed(p, 127, reason);
    }
    jdl_return_handle(c, &COMMAND, p, (uint32_t)sizeof(std_command));
    p = NULL;
  }
  free(p);
  free(env);
  free(changes);
  free(env_store);
  free(argv);
  free(argv_store);
}

/* H's whole contents, up to STD_MAX_FILE bytes, into *DATA and *LEN. */
static void read_all(HANDLE h, uint8_t** data, uint32_t* len) {
  LARGE_INTEGER size;
  *data = NULL;
  *len = 0;
  if(!h || !GetFileSizeEx(h, &size) || size.QuadPart <= 0) return;
  DWORD want = size.QuadPart > (LONGLONG)STD_MAX_FILE ? STD_MAX_FILE : (DWORD)size.QuadPart;
  *data = (uint8_t*)malloc(want);
  OVERLAPPED at;
  memset(&at, 0, sizeof at);
  DWORD got = 0;
  if(*data && ReadFile(h, *data, want, &got, &at)) *len = got;
}

/* Records the result of P's child, which has ended. Called with `lock` held. */
static void finish(std_command* p) {
  DWORD code = 255;
  if(!GetExitCodeProcess(p->process, &code)) code = 255;
  unlist(p);
  p->waited = 1;
  p->status = exit_status(code);
  read_all(p->out, &p->out_data, &p->out_len);
  read_all(p->err, &p->err_data, &p->err_len);
  close_handle(&p->out);
  close_handle(&p->err);
  close_handle(&p->process);
}

/* Records P's result if its child has ended, without waiting. Called with `lock` held. */
static void reap_ended(std_command* p) {
  if(!p->waited && p->process && WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0) finish(p);
}

/* The command in argument 0, waited for; NULL (the call failed) for null. */
static std_command* waited(jdlo_ctx* c) {
  std_command* p = (std_command*)jdl_arg_handle(c, 0, &COMMAND);
  if(!p) {
    jdl_fail(c, "null command");
    return NULL;
  }
  pthread_mutex_lock(&lock);
  HANDLE copy = NULL;
  /* A copy, as command_next may finish it, closing its own, meanwhile; without one, the wait
   * holds the lock. */
  if(!p->waited) {
    if(DuplicateHandle(GetCurrentProcess(), p->process, GetCurrentProcess(), &copy, SYNCHRONIZE,
                       FALSE, 0)) {
      pthread_mutex_unlock(&lock);
      WaitForSingleObject(copy, INFINITE);
      CloseHandle(copy);
      pthread_mutex_lock(&lock);
    } else {
      WaitForSingleObject(p->process, INFINITE);
    }
  }
  if(!p->waited) finish(p);
  pthread_mutex_unlock(&lock);
  return p;
}

void std_command_wait(jdlo_ctx* c) {
  std_command* p = waited(c);
  if(p) jdl_return_i32(c, p->status);
}

void std_command_stdout(jdlo_ctx* c) {
  std_command* p = waited(c);
  if(p) jdl_return_bytes_copy(c, p->out_data, p->out_len);
}

void std_command_stderr(jdlo_ctx* c) {
  std_command* p = waited(c);
  if(p) jdl_return_bytes_copy(c, p->err_data, p->err_len);
}

void std_command_done(jdlo_ctx* c) {
  std_command* p = (std_command*)jdl_arg_handle(c, 0, &COMMAND);
  if(!p) {
    jdl_fail(c, "null command");
    return;
  }
  pthread_mutex_lock(&lock);
  reap_ended(p);
  int done = p->waited;
  pthread_mutex_unlock(&lock);
  jdl_return_bool(c, done != 0);
}

/* Ends the child of the command in argument 0, if it has not ended, with the status
 * 137 that SIGKILL gives elsewhere; whether it had not. */
void std_command_kill(jdlo_ctx* c) {
  std_command* p = (std_command*)jdl_arg_handle(c, 0, &COMMAND);
  if(!p) {
    jdl_fail(c, "null command");
    return;
  }
  pthread_mutex_lock(&lock);
  reap_ended(p);
  int running = !p->waited && p->process;
  if(running) TerminateProcess(p->process, 137);
  pthread_mutex_unlock(&lock);
  jdl_return_bool(c, running);
}

/* Waits for a child to end, then records its result in its handle; false when there is no child
 * to wait for. A wait takes copies of at most MAXIMUM_WAIT_OBJECTS children's handles, so that a
 * collection meanwhile cannot close one, and with more, looks at each group in turn. */
void std_command_next(jdlo_ctx* c) {
  for(size_t group = 0;;) {
    HANDLE copies[MAXIMUM_WAIT_OBJECTS];
    DWORD n = 0;
    size_t total = 0, first = group * MAXIMUM_WAIT_OBJECTS;
    pthread_mutex_lock(&lock);
    for(std_command* p = live; p; p = p->next, total++) {
      if(total >= first && n < MAXIMUM_WAIT_OBJECTS &&
         DuplicateHandle(GetCurrentProcess(), p->process, GetCurrentProcess(), &copies[n],
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
        n++;
    }
    pthread_mutex_unlock(&lock);
    if(total == 0) {
      jdl_return_bool(c, 0);
      return;
    }
    DWORD r =
        n ? WaitForMultipleObjects(n, copies, FALSE, total > n ? 10 : INFINITE) : WAIT_TIMEOUT;
    int ended = r < WAIT_OBJECT_0 + n;
    if(ended) {
      DWORD pid = GetProcessId(copies[r - WAIT_OBJECT_0]);
      pthread_mutex_lock(&lock);
      for(std_command* p = live; p; p = p->next) {
        if(p->pid == pid) {
          finish(p);
          break;
        }
      }
      pthread_mutex_unlock(&lock);
    }
    for(DWORD i = 0; i < n; i++) CloseHandle(copies[i]);
    if(ended) {
      jdl_return_bool(c, 1);
      return;
    }
    group = first + MAXIMUM_WAIT_OBJECTS < total ? group + 1 : 0;
  }
}
