// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Child commands. command_start runs a program without waiting for it, its
 * standard input /dev/null and its standard output and error captured to
 * unlinked temporary files, so a child never blocks on a full pipe and
 * several can run at once. command_wait reaps it; its outputs are read then. A
 * handle that is collected first kills and reaps its child.
 *
 * ARGV and ENV are Bytes of NUL-terminated entries. ENV applies over the
 * inherited environment, in order; NAME=VALUE sets NAME, a bare NAME unsets
 * it. The status is the exit status, 128 + the signal for a child a signal
 * ended, and 127 for a program that cannot run, its reason on stderr, as a
 * POSIX shell reports them; 255, with the reason on stderr, when the child
 * cannot be waited for (when SIGCHLD is ignored, say), so a lost child never
 * reads as a success.
 *
 * command_done tells, without waiting, whether a child has ended, reaping it if
 * so, and command_kill ends one with SIGKILL.
 *
 * command_next reaps whichever child ends first and records its result in
 * its handle, which command_done then reports, so a caller with several
 * running waits for whichever ends first; one that ends outside the caller's
 * set is recorded too, and never waited for again. The live handles are in a
 * list for it to search, and nothing else in the VM starts children.
 *
 * Processes on other schedulers call these at once, and a collection frees a
 * handle on whichever thread runs it, so `lock` guards the list and each
 * handle's result. Waiting never reaps: it waits for some child to end, then,
 * with `lock` held, reaps one that has and records its result in its handle,
 * whoever's it is. A pid is never reused while a handle still names it. */

#define _DARWIN_C_SOURCE 1

#include "std.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

typedef struct std_command {
  jdl_refs refs;
  struct std_command *prev, *next;
  pid_t pid;
  int out_fd, err_fd;
  int waited;
  int32_t status;
  uint8_t* out;
  uint8_t* err;
  uint32_t out_len, err_len;
} std_command;

static void command_close(std_command* p) {
  if(p->out_fd >= 0) close(p->out_fd);
  if(p->err_fd >= 0) close(p->err_fd);
  p->out_fd = p->err_fd = -1;
}

/* The handles whose children may still be running. */
static std_command* live = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void unlist(std_command* p) {
  if(p->prev) p->prev->next = p->next;
  else if(live == p) live = p->next;
  if(p->next) p->next->prev = p->prev;
  p->prev = p->next = NULL;
}

static void command_free(void* payload) {
  if(!jdl_release(payload)) return;
  std_command* p = (std_command*)payload;
  pthread_mutex_lock(&lock);
  unlist(p);
  if(!p->waited && p->pid > 0) {
    kill(p->pid, SIGKILL);
    while(waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) {}
  }
  pthread_mutex_unlock(&lock);
  command_close(p);
  free(p->out);
  free(p->err);
  free(p);
}

static const jello_jdll_handle_class COMMAND = {"command", command_free, jdl_retain};

/* An unlinked temporary file, open for reading and writing; -1 on failure. */
static int temp_file(void) {
  char dir[STD_MAX_PATH], path[STD_MAX_PATH];
  if(!std_temp_dir(dir, sizeof dir)) return -1;
  int n = snprintf(path, sizeof path, "%s/jello-command-XXXXXX", dir);
  if(n < 0 || (size_t)n >= sizeof path) return -1;
  int fd = mkstemp(path);
  if(fd < 0) return -1;
  unlink(path);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

/* Whether environment entry ENTRY ("NAME=VALUE") is for NAME, of LEN bytes. */
static int names(const char* entry, const char* name, size_t len) {
  return strncmp(entry, name, len) == 0 && entry[len] == '=';
}

/* The inherited environment with CHANGES applied; NULL if out of memory. */
static char** environment(char** changes, size_t count) {
  size_t inherited = 0;
  while(environ[inherited]) inherited++;
  char** env = (char**)calloc(inherited + count + 1, sizeof(char*));
  if(!env) return NULL;
  size_t n = 0;
  for(size_t i = 0; i < inherited; i++) env[n++] = environ[i];
  for(size_t i = 0; i < count; i++) {
    const char* eq = strchr(changes[i], '=');
    size_t len = eq ? (size_t)(eq - changes[i]) : strlen(changes[i]);
    size_t kept = 0;
    for(size_t j = 0; j < n; j++) {
      if(!names(env[j], changes[i], len)) env[kept++] = env[j];
    }
    n = kept;
    if(eq) env[n++] = changes[i];
  }
  env[n] = NULL;
  return env;
}

/* Records that P ended with STATUS and only MESSAGE (PROGRAM, REASON) on its stderr. */
static void failed(std_command* p, int32_t status, const char* message, const char* program,
                   const char* reason) {
  char text[512];
  int n = snprintf(text, sizeof text, message, program, reason);
  p->waited = 1;
  p->status = status;
  free(p->err);
  p->err_len = n < 0 ? 0u : (uint32_t)((size_t)n < sizeof text ? (size_t)n : sizeof text - 1);
  p->err = (uint8_t*)malloc(p->err_len + 1u);
  if(p->err) memcpy(p->err, text, p->err_len);
  else p->err_len = 0;
}

/* The file PROGRAM names, found on PATH as posix_spawnp would, into OUT; 0, or why none can run.
 * Searching here makes a missing program fail to start wherever posix_spawnp cannot report a
 * failed exec (glibc under emulation), rather than start a child that exits 127. */
static int find_program(const char* program, char* out, size_t cap) {
  if(strchr(program, '/')) return snprintf(out, cap, "%s", program) < (int)cap ? 0 : ENAMETOOLONG;
  const char* path = getenv("PATH");
  int error = ENOENT;
  for(const char* dir = path ? path : "/usr/bin:/bin";;) {
    const char* end = strchr(dir, ':');
    int len = end ? (int)(end - dir) : (int)strlen(dir);
    /* An empty entry is the current directory. */
    int n =
        len ? snprintf(out, cap, "%.*s/%s", len, dir, program) : snprintf(out, cap, "%s", program);
    struct stat st;
    if(n >= 0 && (size_t)n < cap && stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
      if(access(out, X_OK) == 0) return 0;
      error = EACCES;
    }
    if(!end) return error;
    dir = end + 1;
  }
}

void std_command_start(jdlo_ctx* c) {
  char *argv_store = NULL, *env_store = NULL;
  size_t argc = 0, envc = 0;
  char** argv = std_entries(c, 0, &argv_store, &argc);
  char** changes = std_entries(c, 1, &env_store, &envc);
  char** env = changes ? environment(changes, envc) : NULL;
  std_command* p = (std_command*)calloc(1, sizeof(std_command));
  if(p) p->refs = 1;
  if(!argv || argc == 0 || !env || !p) {
    jdl_fail(c,
             argv && argc > 0 && changes
                 ? "command_start: out of memory"
                 : "command_start: argv and env must be NUL-terminated entries, argv at least one");
  } else {
    p->out_fd = temp_file();
    p->err_fd = temp_file();
    posix_spawn_file_actions_t actions;
    char program[PATH_MAX];
    int error = p->out_fd < 0 || p->err_fd < 0 ? (errno ? errno : EIO)
                                               : find_program(argv[0], program, sizeof program);
    if(error == 0) error = posix_spawn_file_actions_init(&actions);
    if(error == 0) {
      posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_adddup2(&actions, p->out_fd, 1);
      posix_spawn_file_actions_adddup2(&actions, p->err_fd, 2);
      /* Listed before the lock is released, so a child that ends at once is found as ours. */
      pthread_mutex_lock(&lock);
      error = posix_spawn(&p->pid, program, &actions, NULL, argv, env);
      if(error == 0) {
        p->next = live;
        if(live) live->prev = p;
        live = p;
      }
      pthread_mutex_unlock(&lock);
      posix_spawn_file_actions_destroy(&actions);
    }
    if(error != 0) {
      command_close(p);
      failed(p, 127, "cannot run %s: %s\n", argv[0], strerror(error));
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

/* FD's whole contents, up to STD_MAX_FILE bytes, into *DATA and *LEN. */
static void read_all(int fd, uint8_t** data, uint32_t* len) {
  off_t size = lseek(fd, 0, SEEK_END);
  *data = NULL;
  *len = 0;
  if(size <= 0) return;
  if(size > (off_t)STD_MAX_FILE) size = (off_t)STD_MAX_FILE;
  *data = (uint8_t*)malloc((size_t)size);
  if(!*data) return;
  ssize_t got = pread(fd, *data, (size_t)size, 0);
  *len = got > 0 ? (uint32_t)got : 0u;
}

/* Reaps P's child, which has ended, and records its result, or that it could
 * not be waited for, LOST being the error. Called with `lock` held. */
static void finish(std_command* p, int lost) {
  int status = 0;
  while(!lost && waitpid(p->pid, &status, 0) < 0) lost = errno == EINTR ? 0 : errno;
  unlist(p);
  p->waited = 1;
  p->status = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
  read_all(p->out_fd, &p->out, &p->out_len);
  read_all(p->err_fd, &p->err, &p->err_len);
  command_close(p);
  if(lost) failed(p, 255, "cannot wait for %s: %s\n", "the command", strerror(lost));
}

/* Waits, without reaping, until some child has ended; 0, or the error when
 * there is none to wait for. */
static int await_child(void) {
  siginfo_t info;
  while(waitid(P_ALL, 0, &info, WEXITED | WNOWAIT) < 0) {
    if(errno != EINTR) return errno;
  }
  return 0;
}

/* Reaps a child that has ended, if one has, recording its result when a
 * handle names it. Called with `lock` held. */
static void reap_one(void) {
  siginfo_t info;
  memset(&info, 0, sizeof info);
  if(waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) < 0 || info.si_pid == 0) return;
  std_command* p = live;
  while(p && p->pid != info.si_pid) p = p->next;
  if(p) finish(p, 0);
  else
    while(waitpid(info.si_pid, NULL, 0) < 0 && errno == EINTR) {}
}

/* The command in argument 0, reaped; NULL (the call failed) for null. */
static std_command* waited(jdlo_ctx* c) {
  std_command* p = (std_command*)jdl_arg_handle(c, 0, &COMMAND);
  if(!p) {
    jdl_fail(c, "null command");
    return NULL;
  }
  pthread_mutex_lock(&lock);
  while(!p->waited) {
    pthread_mutex_unlock(&lock);
    int lost = await_child();
    pthread_mutex_lock(&lock);
    if(!lost) reap_one();
    else if(!p->waited) finish(p, lost);
  }
  pthread_mutex_unlock(&lock);
  return p;
}

void std_command_wait(jdlo_ctx* c) {
  std_command* p = waited(c);
  if(p) jdl_return_i32(c, p->status);
}

void std_command_stdout(jdlo_ctx* c) {
  std_command* p = waited(c);
  if(p) jdl_return_bytes_copy(c, p->out, p->out_len);
}

void std_command_stderr(jdlo_ctx* c) {
  std_command* p = waited(c);
  if(p) jdl_return_bytes_copy(c, p->err, p->err_len);
}

/* Reaps P's child if it has ended, without waiting. Called with `lock` held. */
static void reap_ended(std_command* p) {
  if(p->waited || p->pid <= 0) return;
  siginfo_t info;
  memset(&info, 0, sizeof info);
  if(waitid(P_PID, (id_t)p->pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == p->pid)
    finish(p, 0);
}

/* Whether the command in argument 0 has ended, without waiting; one that has
 * is reaped, so waiting for it then does not block. */
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

/* Kills the child of the command in argument 0 with SIGKILL, if it has not
 * ended, its status then being 137; whether it had not. An unreaped child
 * keeps its pid, so the signal reaches no other process. */
void std_command_kill(jdlo_ctx* c) {
  std_command* p = (std_command*)jdl_arg_handle(c, 0, &COMMAND);
  if(!p) {
    jdl_fail(c, "null command");
    return;
  }
  pthread_mutex_lock(&lock);
  reap_ended(p);
  int running = !p->waited && p->pid > 0;
  if(running) kill(p->pid, SIGKILL);
  pthread_mutex_unlock(&lock);
  jdl_return_bool(c, running);
}

/* Waits for a child to end, then reaps one that has, recording its result in
 * its handle; false when there is no child to wait for. */
void std_command_next(jdlo_ctx* c) {
  int lost = await_child();
  if(!lost) {
    pthread_mutex_lock(&lock);
    reap_one();
    pthread_mutex_unlock(&lock);
  }
  jdl_return_bool(c, !lost);
}
