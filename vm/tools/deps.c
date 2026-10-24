// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jlo_tool's init and get-deps commands; create a project and fetch its dependencies into
 * packages/, pinned by jello.lock. */

#include "deps.h"

#include <jello/internal/platform.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winioctl.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

#define LOCK_VERSION 2

typedef struct text {
  char* data;
  size_t len, cap;
} text;

static void* xrealloc(void* p, size_t n) {
  void* q = realloc(p, n ? n : 1);
  if(!q) {
    fputs("jlo_tool: out of memory\n", stderr);
    exit(1);
  }
  return q;
}

static char* xstrdup(const char* s) {
  size_t n = strlen(s) + 1;
  return memcpy(xrealloc(NULL, n), s, n);
}

static void text_add(text* t, const char* s, size_t n) {
  if(t->len + n + 1 > t->cap) {
    t->cap = (t->len + n + 1) * 2;
    t->data = xrealloc(t->data, t->cap);
  }
  memcpy(t->data + t->len, s, n);
  t->len += n;
  t->data[t->len] = '\0';
}

static void text_puts(text* t, const char* s) { text_add(t, s, strlen(s)); }

/** Returns `format` formatted with `ap`, malloc'd. */
static char* vformat(const char* format, va_list ap) {
  va_list again;
  va_copy(again, ap);
  int n = vsnprintf(NULL, 0, format, ap);
  if(n < 0) {
    fputs("jlo_tool: cannot format text\n", stderr);
    exit(1);
  }
  char* s = xrealloc(NULL, (size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, format, again);
  va_end(again);
  return s;
}

static void text_printf(text* t, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  char* s = vformat(format, ap);
  va_end(ap);
  text_puts(t, s);
  free(s);
}

/** Returns a path formatted from `format` and its arguments, malloc'd. */
static char* path_of(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  char* s = vformat(format, ap);
  va_end(ap);
  return s;
}

/** The bytes of the file at `path`, NUL-terminated and malloc'd, with their count in `*len`
 * when `len` is not NULL; NULL with errno set on failure. */
static char* read_file(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if(!f) return NULL;
  text t = {0};
  char buf[65536];
  size_t n;
  while((n = fread(buf, 1, sizeof buf, f)) > 0) text_add(&t, buf, n);
  int failed = ferror(f);
  fclose(f);
  if(failed) {
    free(t.data);
    errno = EIO;
    return NULL;
  }
  if(!t.data) text_add(&t, "", 0);
  if(len) *len = t.len;
  return t.data;
}

static int write_file(const char* path, const char* data, size_t len) {
  FILE* f = fopen(path, "wb");
  if(!f) {
    fprintf(stderr, "jlo_tool: cannot write %s: %s\n", path, strerror(errno));
    return 0;
  }
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  if(!ok) fprintf(stderr, "jlo_tool: cannot write %s\n", path);
  return ok;
}

static int is_file(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Enough of RFC 8259 JSON for manifests and locks; objects keep their keys' order, and numbers
 * keep their text. */

typedef enum json_kind {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
} json_kind;

typedef struct json {
  json_kind kind;
  char* string; /**< A string's value, a number's text, "true" or "false". */
  size_t count;
  char** keys; /**< An object's keys, parallel to `items`; NULL for an array. */
  struct json** items;
} json;

typedef struct json_parser {
  const char* s;
  size_t len, at;
  const char* error;
} json_parser;

static void json_free(json* j) {
  if(!j) return;
  for(size_t i = 0; i < j->count; i++) {
    if(j->keys) free(j->keys[i]);
    json_free(j->items[i]);
  }
  free(j->keys);
  free(j->items);
  free(j->string);
  free(j);
}

static void json_space(json_parser* p) {
  while(p->at < p->len && strchr(" \t\r\n", p->s[p->at])) p->at++;
}

static json* json_value(json_parser* p, int depth);

static char* json_string(json_parser* p) {
  if(p->at >= p->len || p->s[p->at] != '"') {
    p->error = "expected a string";
    return NULL;
  }
  p->at++;
  text t = {0};
  text_add(&t, "", 0);
  while(p->at < p->len && p->s[p->at] != '"') {
    unsigned char c = (unsigned char)p->s[p->at++];
    if(c < 0x20) {
      p->error = "control character in a string";
      free(t.data);
      return NULL;
    }
    if(c != '\\') {
      char ch = (char)c;
      text_add(&t, &ch, 1);
      continue;
    }
    if(p->at >= p->len) break;
    char e = p->s[p->at++];
    const char* plain = strchr("\"\\/bfnrt", e);
    if(plain && e != '\0') {
      char ch = "\"\\/\b\f\n\r\t"[plain - "\"\\/bfnrt"];
      text_add(&t, &ch, 1);
      continue;
    }
    if(e != 'u' || p->at + 4 > p->len) {
      p->error = "bad escape in a string";
      free(t.data);
      return NULL;
    }
    unsigned code = 0;
    for(int k = 0; k < 4; k++) {
      char h = p->s[p->at++];
      unsigned v = h >= '0' && h <= '9'   ? (unsigned)(h - '0')
                   : h >= 'a' && h <= 'f' ? (unsigned)(h - 'a' + 10)
                   : h >= 'A' && h <= 'F' ? (unsigned)(h - 'A' + 10)
                                          : 16u;
      if(v == 16) {
        p->error = "bad \\u escape in a string";
        free(t.data);
        return NULL;
      }
      code = code * 16 + v;
    }
    // Surrogates are not paired here; manifests are ASCII in practice.
    char out[3];
    size_t n;
    if(code < 0x80) {
      out[0] = (char)code;
      n = 1;
    } else if(code < 0x800) {
      out[0] = (char)(0xC0 | (code >> 6));
      out[1] = (char)(0x80 | (code & 0x3F));
      n = 2;
    } else {
      out[0] = (char)(0xE0 | (code >> 12));
      out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
      out[2] = (char)(0x80 | (code & 0x3F));
      n = 3;
    }
    text_add(&t, out, n);
  }
  if(p->at >= p->len) {
    p->error = "unterminated string";
    free(t.data);
    return NULL;
  }
  p->at++;
  return t.data;
}

static json* json_new(json_kind kind) {
  json* j = xrealloc(NULL, sizeof *j);
  memset(j, 0, sizeof *j);
  j->kind = kind;
  return j;
}

static void json_push(json* j, char* key, json* item) {
  j->items = xrealloc(j->items, (j->count + 1) * sizeof *j->items);
  if(j->kind == JSON_OBJECT) j->keys = xrealloc(j->keys, (j->count + 1) * sizeof *j->keys);
  if(key) j->keys[j->count] = key;
  j->items[j->count++] = item;
}

/** Parses the elements of the array or object `j`, after its opening bracket, up to `close`.
 * Frees `j` and returns NULL on error. */
static json* json_elements(json_parser* p, json* j, char close, int depth) {
  json_space(p);
  if(p->at < p->len && p->s[p->at] == close) {
    p->at++;
    return j;
  }
  for(;;) {
    char* key = NULL;
    json_space(p);
    if(j->kind == JSON_OBJECT) {
      if(!(key = json_string(p))) break;
      json_space(p);
      if(p->at >= p->len || p->s[p->at] != ':') {
        p->error = "expected ':'";
        free(key);
        break;
      }
      p->at++;
    }
    json* item = json_value(p, depth + 1);
    if(!item) {
      free(key);
      break;
    }
    json_push(j, key, item);
    json_space(p);
    if(p->at < p->len && p->s[p->at] == ',') {
      p->at++;
      continue;
    }
    if(p->at < p->len && p->s[p->at] == close) {
      p->at++;
      return j;
    }
    p->error = close == '}' ? "expected ',' or '}'" : "expected ',' or ']'";
    break;
  }
  json_free(j);
  return NULL;
}

static json* json_value(json_parser* p, int depth) {
  json_space(p);
  if(depth > 64) {
    p->error = "nested too deeply";
    return NULL;
  }
  if(p->at >= p->len) {
    p->error = "unexpected end";
    return NULL;
  }
  char c = p->s[p->at];
  if(c == '{' || c == '[') {
    p->at++;
    return json_elements(p, json_new(c == '{' ? JSON_OBJECT : JSON_ARRAY), c == '{' ? '}' : ']',
                         depth);
  }
  if(c == '"') {
    char* s = json_string(p);
    if(!s) return NULL;
    json* j = json_new(JSON_STRING);
    j->string = s;
    return j;
  }
  static const char* const WORDS[] = {"null", "true", "false"};
  for(size_t w = 0; w < 3; w++) {
    size_t n = strlen(WORDS[w]);
    if(p->len - p->at >= n && memcmp(p->s + p->at, WORDS[w], n) == 0) {
      p->at += n;
      json* j = json_new(w == 0 ? JSON_NULL : JSON_BOOL);
      if(w) j->string = xstrdup(WORDS[w]);
      return j;
    }
  }
  size_t start = p->at;
  while(p->at < p->len && strchr("+-0123456789.eE", p->s[p->at])) p->at++;
  if(p->at == start) {
    p->error = "unexpected character";
    return NULL;
  }
  json* j = json_new(JSON_NUMBER);
  j->string = xrealloc(NULL, p->at - start + 1);
  memcpy(j->string, p->s + start, p->at - start);
  j->string[p->at - start] = '\0';
  return j;
}

/** The JSON in the file at `path`, or NULL, after saying why on stderr when `loud`. */
static json* json_parse(const char* path, int loud) {
  size_t len;
  char* s = read_file(path, &len);
  if(!s) {
    if(loud) fprintf(stderr, "jlo_tool: cannot read %s: %s\n", path, strerror(errno));
    return NULL;
  }
  json_parser p = {s, len, 0, NULL};
  json* j = json_value(&p, 0);
  json_space(&p);
  if(j && p.at != p.len) {
    p.error = "text after the value";
    json_free(j);
    j = NULL;
  }
  if(!j && loud)
    fprintf(stderr, "jlo_tool: %s: invalid JSON at byte %zu: %s\n", path, p.at, p.error);
  free(s);
  return j;
}

static json* json_read(const char* path) { return json_parse(path, 1); }

static json* json_get(const json* j, const char* key) {
  if(!j || j->kind != JSON_OBJECT) return NULL;
  for(size_t i = 0; i < j->count; i++)
    if(strcmp(j->keys[i], key) == 0) return j->items[i];
  return NULL;
}

/** `key`'s string in object `j`, or NULL when absent or not a string. */
static const char* json_string_at(const json* j, const char* key) {
  const json* v = json_get(j, key);
  return v && v->kind == JSON_STRING ? v->string : NULL;
}

static void json_quote(text* t, const char* s) {
  text_puts(t, "\"");
  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(c == '"' || c == '\\') {
      char e[2] = {'\\', (char)c};
      text_add(t, e, 2);
    } else if(c < 0x20) text_printf(t, "\\u%04x", c);
    else text_add(t, s, 1);
  }
  text_puts(t, "\"");
}

// SHA-256, per FIPS 180-4.

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const unsigned char* b) {
  uint32_t w[64];
  for(int i = 0; i < 16; i++)
    w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 |
           b[4 * i + 3];
  for(int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for(int i = 0; i < 64; i++) {
    uint32_t t1 =
        hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
    uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = bb;
    bb = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += bb;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

/** Writes the SHA-256 of the `len` bytes at `data` to `hex`, as 64 lowercase hex digits. */
static void sha256_hex(const unsigned char* data, size_t len, char hex[65]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for(; len - i >= 64; i += 64) sha256_block(h, data + i);
  unsigned char tail[128] = {0};
  size_t rest = len - i;
  memcpy(tail, data + i, rest);
  tail[rest] = 0x80;
  size_t blocks = rest + 9 > 64 ? 2 : 1;
  uint64_t bits = (uint64_t)len * 8;
  for(int k = 0; k < 8; k++) tail[blocks * 64 - 1 - (size_t)k] = (unsigned char)(bits >> (8 * k));
  for(size_t k = 0; k < blocks; k++) sha256_block(h, tail + 64 * k);
  for(int k = 0; k < 8; k++) snprintf(hex + 8 * k, 9, "%08x", h[k]);
}

/** Runs `argv` without a shell, its output passed through; whether it exited 0. The caller says
 * what failed. */
static int run(char* const argv[]) {
#ifdef _WIN32
  char program[PATH_MAX];
  if(jello_find_program(argv[0], program, sizeof program) != 0) {
    fprintf(stderr, "jlo_tool: cannot run %s: %s\n", argv[0], strerror(ENOENT));
    return 0;
  }
  char* line = jello_command_line(argv);
  STARTUPINFOA info;
  PROCESS_INFORMATION made;
  memset(&info, 0, sizeof info);
  info.cb = sizeof info;
  info.dwFlags = STARTF_USESTDHANDLES;
  info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  info.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  BOOL started =
      line && CreateProcessA(program, line, NULL, NULL, TRUE, 0, NULL, NULL, &info, &made);
  free(line);
  if(!started) {
    fprintf(stderr, "jlo_tool: cannot run %s: error %lu\n", argv[0], (unsigned long)GetLastError());
    return 0;
  }
  DWORD code = 1;
  WaitForSingleObject(made.hProcess, INFINITE);
  GetExitCodeProcess(made.hProcess, &code);
  CloseHandle(made.hThread);
  CloseHandle(made.hProcess);
  return code == 0;
#else
  pid_t pid;
  int err = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
  if(err) {
    fprintf(stderr, "jlo_tool: cannot run %s: %s\n", argv[0], strerror(err));
    return 0;
  }
  int status;
  while(waitpid(pid, &status, 0) < 0) {
    if(errno != EINTR) {
      fprintf(stderr, "jlo_tool: waiting for %s: %s\n", argv[0], strerror(errno));
      return 0;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

/** What is at `path`, not following a link: 0 for nothing, 'l' for a link (on Windows also a
 * junction), 'd' for a directory and 'f' for anything else. */
static int kind_of(const char* path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  if(attrs == INVALID_FILE_ATTRIBUTES) return 0;
  if(attrs & FILE_ATTRIBUTE_REPARSE_POINT) return 'l';
  return attrs & FILE_ATTRIBUTE_DIRECTORY ? 'd' : 'f';
#else
  struct stat st;
  if(lstat(path, &st) != 0) return 0;
  return S_ISLNK(st.st_mode) ? 'l' : S_ISDIR(st.st_mode) ? 'd' : 'f';
#endif
}

/** Calls `visit` with each entry under `path` and its kind_of, then with `path`, depth first,
 * never following a link; returns the first nonzero `visit` returns, or -1 when a directory
 * cannot be read, else 0. */
static int walk(const char* path, int (*visit)(const char* path, int kind)) {
  int kind = kind_of(path);
  if(kind == 'd') {
    DIR* d = opendir(path);
    if(!d) return -1;
    int r = 0;
    struct dirent* e;
    while(!r && (e = readdir(d))) {
      if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char* child = path_of("%s/%s", path, e->d_name);
      r = walk(child, visit);
      free(child);
    }
    closedir(d);
    if(r) return r;
  }
  return visit(path, kind);
}

/** Removes `path`, of kind_of `kind`, a directory being empty; 0, or -1 with errno set. */
static int remove_entry(const char* path, int kind) {
#ifdef _WIN32
  /* A file unpacked read-only would refuse to go, and a junction is a directory to remove. */
  DWORD attrs = GetFileAttributesA(path);
  SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
  if(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
    return RemoveDirectoryA(path) ? 0 : (errno = EACCES, -1);
  return DeleteFileA(path) ? 0 : (errno = EACCES, -1);
#else
  return remove(path);
#endif
}

/** Removes `path`, a link, file or directory tree, if it is there; whether that worked. */
static int remove_tree(const char* path) {
  if(!kind_of(path)) return 1;
  int ok = walk(path, remove_entry) == 0;
  if(!ok) fprintf(stderr, "jlo_tool: cannot remove %s: %s\n", path, strerror(errno));
  return ok;
}

/** Makes a new directory from `template`, its last six characters XXXXXX, which it replaces;
 * whether it could, with errno set when not. */
static int make_temp_dir(char* template) {
#ifdef _WIN32
  /* _mktemp_s gives only 26 names a template; these come from the clock and a count. */
  static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char* x = template + strlen(template) - 6;
  uint64_t seed = GetTickCount64() * 1000003u + GetCurrentProcessId();
  for(int tries = 0; tries < 100; tries++) {
    uint64_t v = seed + (uint64_t)tries * 0x9E3779B97F4A7C15ull;
    for(int i = 0; i < 6; i++, v /= 36) x[i] = digits[v % 36];
    if(jello_mkdir(template) == 0) return 1;
    if(errno != EEXIST) return 0;
  }
  errno = EEXIST;
  return 0;
#else
  return mkdtemp(template) != NULL;
#endif
}

static int make_dir(const char* path) {
  if(jello_mkdir(path) == 0 || errno == EEXIST) return 1;
  fprintf(stderr, "jlo_tool: cannot make %s: %s\n", path, strerror(errno));
  return 0;
}

typedef enum dep_kind { DEP_ARCHIVE, DEP_PATH } dep_kind;

typedef struct dep {
  char* name;
  dep_kind kind;
  char* source;  //< The archive's URL, or the path from the project's root.
  char* version; //< The version the archive must hold, or NULL.
  char* by;      //< Who asked for it; "jello.json" or a package's name.
} dep;

typedef struct deps {
  dep* items;
  size_t count;
} deps;

static void deps_free(deps* d) {
  for(size_t i = 0; i < d->count; i++) {
    free(d->items[i].name);
    free(d->items[i].source);
    free(d->items[i].version);
    free(d->items[i].by);
  }
  free(d->items);
  d->items = NULL;
  d->count = 0;
}

static int alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/** Whether `word` is a letter or digit followed by letters, digits and characters of `others`. */
static int valid_word(const char* word, const char* others) {
  if(!alnum(*word)) return 0;
  for(const char* c = word; *c; c++)
    if(!alnum(*c) && !strchr(others, *c)) return 0;
  return 1;
}

/** Whether `name` can name a package, a directory under packages/, or an owner or repository in a
 * URL. */
static int valid_name(const char* name) { return valid_word(name, "._-"); }

/** Whether `ref` can be a version, tag or commit; a name that may also hold semver's '+'. */
static int valid_ref(const char* ref) { return valid_word(ref, "._+-"); }

/** Whether `repo` is OWNER/NAME, both valid names. */
static int valid_repo(const char* repo) {
  const char* slash = strchr(repo, '/');
  if(!slash || slash == repo || strchr(slash + 1, '/')) return 0;
  char* owner = path_of("%.*s", (int)(slash - repo), repo);
  int ok = valid_name(owner) && valid_name(slash + 1);
  free(owner);
  return ok;
}

static int by_name(const void* a, const void* b) {
  return strcmp(((const dep*)a)->name, ((const dep*)b)->name);
}

/** Appends to `out`, in name order, the dependencies that `key` of `manifest` (read from `path`,
 * of package `by`) names. Only the project's own manifest (`root` set) may name a path, relative to
 * the project's root; a fetched package naming one could reach anywhere on disk. */
static int deps_in(const json* manifest, const char* key, const char* path, const char* by,
                   int root, deps* out) {
  const json* list = json_get(manifest, key);
  if(!list || list->kind == JSON_NULL) return 1;
  if(list->kind != JSON_OBJECT) {
    fprintf(stderr, "jlo_tool: %s: \"%s\" must be an object\n", path, key);
    return 0;
  }
  const char* org = getenv("JELLO_DEPS_GITHUB_ORG");
  const char* registry = getenv("JELLO_REGISTRY_URL");
  if(!org || !*org) org = "Lazarus404";
  size_t first = out->count;
  for(size_t i = 0; i < list->count; i++) {
    const char* name = list->keys[i];
    const json* spec = list->items[i];
    if(!valid_name(name)) {
      fprintf(stderr, "jlo_tool: %s: bad dependency name \"%s\"\n", path, name);
      return 0;
    }
    dep d = {xstrdup(name), DEP_ARCHIVE, NULL, NULL, xstrdup(by)};
    const char *repo = json_string_at(spec, "github"), *tag = json_string_at(spec, "tag");
    const char *commit = json_string_at(spec, "commit"), *local = json_string_at(spec, "path");
    if(spec->kind == JSON_STRING && valid_ref(spec->string)) {
      d.source = registry && *registry
                     ? path_of("%s/packages/%s/%s/artifact", registry, name, spec->string)
                     : path_of("https://github.com/%s/%s/archive/refs/tags/%s.zip", org, name,
                               spec->string);
      d.version = xstrdup(spec->string);
    } else if(repo && (!tag != !commit) && valid_repo(repo) && valid_ref(tag ? tag : commit)) {
      d.source = tag ? path_of("https://github.com/%s/archive/refs/tags/%s.zip", repo, tag)
                     : path_of("https://github.com/%s/archive/%s.zip", repo, commit);
    } else if(local && *local && root) {
      d.kind = DEP_PATH;
      d.source = xstrdup(local);
    } else {
      fprintf(stderr,
              "jlo_tool: %s: dependency \"%s\" takes a version, {\"github\": \"owner/repo\", "
              "\"tag\" or \"commit\"}%s\n",
              path, name,
              root ? " or {\"path\"}" : "; only the project's own jello.json names a path");
      free(d.name);
      free(d.by);
      return 0;
    }
    out->items = xrealloc(out->items, (out->count + 1) * sizeof *out->items);
    out->items[out->count++] = d;
  }
  qsort(out->items + first, out->count - first, sizeof *out->items, by_name);
  return 1;
}

/** Appends to `out` the dependencies `manifest` asks for, as deps_in does: its `dependencies`, and
 * for the project's own manifest its `dev_dependencies` too, which only its own tests and tools
 * use, so a package's are never fetched. */
static int manifest_deps(const json* manifest, const char* path, const char* by, int root,
                         deps* out) {
  return deps_in(manifest, "dependencies", path, by, root, out) &&
         (!root || deps_in(manifest, "dev_dependencies", path, by, root, out));
}

/** Whether the package at `dir` is `name`, at `version` when that is not NULL. When not, says why
 * on stderr, naming `from` (where `dir` came from), unless `from` is NULL. When it is, its manifest
 * goes to `*manifest` if `manifest` is not NULL. */
static int package_is(const char* dir, const char* from, const char* name, const char* version,
                      json** manifest) {
  char* path = path_of("%s/jello.json", dir);
  json* m = NULL;
  int ok = 0;
  if(!is_file(path)) {
    if(from) fprintf(stderr, "jlo_tool: %s has no jello.json\n", from);
  } else if((m = json_parse(path, from != NULL))) {
    const char *got = json_string_at(m, "name"), *have = json_string_at(m, "version");
    if(!got || strcmp(got, name) != 0) {
      if(from)
        fprintf(stderr, "jlo_tool: %s holds package \"%s\", not \"%s\"\n", from, got ? got : "",
                name);
    } else if(!have || !*have) {
      if(from) fprintf(stderr, "jlo_tool: %s holds %s with no version\n", from, name);
    } else if(version && strcmp(have, version) != 0) {
      if(from)
        fprintf(stderr, "jlo_tool: %s holds %s version %s, not %s\n", from, name, have, version);
    } else {
      ok = 1;
    }
  }
  free(path);
  if(ok && manifest) *manifest = m;
  else json_free(m);
  return ok;
}

typedef struct locked {
  char* name;
  char* version;
  char* url;
  char* sha256;
} locked;

typedef struct lock {
  locked* items;
  size_t count;
} lock;

static locked* lock_find(lock* l, const char* name) {
  for(size_t i = 0; i < l->count; i++)
    if(strcmp(l->items[i].name, name) == 0) return &l->items[i];
  return NULL;
}

static locked locked_new(const char* name, const char* version, const char* url,
                         const char* sha256) {
  return (locked){xstrdup(name), xstrdup(version), xstrdup(url), xstrdup(sha256)};
}

static void locked_free(locked* e) {
  free(e->name);
  free(e->version);
  free(e->url);
  free(e->sha256);
}

static void lock_free(lock* l) {
  for(size_t i = 0; i < l->count; i++) locked_free(&l->items[i]);
  free(l->items);
}

/** Reads the lock at `path` into `l`; no lock is not an error. Version 2 nests url and sha256 in
 * "source"; version 1 has them beside "version". */
static int lock_read(const char* path, lock* l) {
  if(!is_file(path)) return 1;
  json* j = json_read(path);
  if(!j) return 0;
  const json* packages = json_get(j, "packages");
  int ok = packages && packages->kind == JSON_OBJECT;
  for(size_t i = 0; ok && i < packages->count; i++) {
    const json* p = packages->items[i];
    const json* source = json_get(p, "source");
    if(!source) source = p;
    const char *version = json_string_at(p, "version"), *url = json_string_at(source, "url");
    const char* sha = json_string_at(source, "sha256");
    if(!version || !url || !sha) {
      ok = 0;
      break;
    }
    l->items = xrealloc(l->items, (l->count + 1) * sizeof *l->items);
    l->items[l->count++] = locked_new(packages->keys[i], version, url, sha);
  }
  if(!ok)
    fprintf(stderr,
            "jlo_tool: %s: not a jello.lock (a \"packages\" object of version, url and sha256)\n",
            path);
  json_free(j);
  return ok;
}

static int by_locked_name(const void* a, const void* b) {
  return strcmp(((const locked*)a)->name, ((const locked*)b)->name);
}

static void lock_text(lock* l, text* t) {
  qsort(l->items, l->count, sizeof *l->items, by_locked_name);
  text_printf(t, "{\n  \"version\": %d,\n  \"packages\": {", LOCK_VERSION);
  for(size_t i = 0; i < l->count; i++) {
    const locked* e = &l->items[i];
    text_puts(t, i ? ",\n    " : "\n    ");
    json_quote(t, e->name);
    text_puts(t, ": {\n      \"version\": ");
    json_quote(t, e->version);
    text_puts(t, ",\n      \"source\": {\n        \"url\": ");
    json_quote(t, e->url);
    text_puts(t, ",\n        \"sha256\": ");
    json_quote(t, e->sha256);
    text_puts(t, "\n      }\n    }");
  }
  text_puts(t, l->count ? "\n  }\n}\n" : "}\n}\n");
}

static int refuse_link(const char* path, int kind) { return kind == 'l'; }

/** The package root of an archive unpacked in `tmp`; `tmp` itself when jello.json is at its top,
 * else its one directory, as in GitHub's archives; NULL when neither. Malloc'd. */
static char* unpacked_root(const char* tmp) {
  char* manifest = path_of("%s/jello.json", tmp);
  int top = is_file(manifest);
  free(manifest);
  if(top) return xstrdup(tmp);
  DIR* d = opendir(tmp);
  if(!d) return NULL;
  char* found = NULL;
  int entries = 0;
  struct dirent* e;
  while((e = readdir(d))) {
    if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    entries++;
    free(found);
    found = path_of("%s/%s", tmp, e->d_name);
  }
  closedir(d);
  if(entries != 1 || kind_of(found) != 'd') {
    free(found);
    return NULL;
  }
  return found;
}

/** Fetches `d`'s archive into `dest`, checking it against the SHA-256 `pinned` (NULL for none),
 * and gives its lock entry in `*entry` and its manifest in `*manifest`. */
static int fetch(const dep* d, const char* packages, const char* dest, const char* pinned,
                 locked* entry, json** manifest) {
  char* zip = path_of("%s/.%s.zip", packages, d->name);
  char* tmp = path_of("%s/.%s.XXXXXX", packages, d->name);
  char* root = NULL;
  int ok = 0;
  char* curl[] = {"curl", "-fsSL", "--retry", "2", "-o", zip, d->source, NULL};
#ifdef _WIN32
  /* Windows' own tar, bsdtar, reads zip; the GNU tar that Git for Windows and MSYS2 put first on
   * PATH does not, so it is named in the system directory. */
  char system_tar[MAX_PATH];
  UINT dir_len = GetSystemDirectoryA(system_tar, MAX_PATH);
  char* tar = "tar";
  if(dir_len > 0 && dir_len + sizeof "\\tar.exe" <= MAX_PATH) {
    strcat(system_tar, "\\tar.exe");
    tar = system_tar;
  }
  char* unzip[] = {tar, "-xf", zip, "-C", tmp, NULL};
#else
  char* unzip[] = {"unzip", "-q", zip, "-d", tmp, NULL};
#endif
  size_t len;
  char* data = NULL;
  char sha[65];
  if(!make_temp_dir(tmp)) {
    fprintf(stderr, "jlo_tool: cannot make %s: %s\n", tmp, strerror(errno));
    free(tmp);
    tmp = NULL;
    goto done;
  }
  if(!run(curl)) {
    fprintf(stderr, "jlo_tool: cannot fetch %s for %s\n", d->source, d->name);
    goto done;
  }
  if(!(data = read_file(zip, &len))) {
    fprintf(stderr, "jlo_tool: cannot read %s: %s\n", zip, strerror(errno));
    goto done;
  }
  sha256_hex((const unsigned char*)data, len, sha);
  if(pinned && strcmp(sha, pinned) != 0) {
    fprintf(stderr, "jlo_tool: %s has SHA-256 %s, but jello.lock pins %s for %s\n", d->source, sha,
            pinned, d->name);
    goto done;
  }
  if(!run(unzip)) {
    fprintf(stderr, "jlo_tool: cannot unpack %s for %s\n", d->source, d->name);
    goto done;
  }
  // A link in a package could reach anywhere once unpacked.
  if(walk(tmp, refuse_link) != 0) {
    fprintf(stderr, "jlo_tool: %s holds a symbolic link; packages may not\n", d->source);
    goto done;
  }
  if(!(root = unpacked_root(tmp))) {
    fprintf(stderr, "jlo_tool: %s holds no package (one directory, with jello.json)\n", d->source);
    goto done;
  }
  if(!package_is(root, d->source, d->name, d->version, manifest)) goto done;
  if(!remove_tree(dest)) goto done;
  if(rename(root, dest) != 0) {
    fprintf(stderr, "jlo_tool: cannot move %s to %s: %s\n", root, dest, strerror(errno));
    goto done;
  }
  *entry = locked_new(d->name, json_string_at(*manifest, "version"), d->source, sha);
  fprintf(stderr, "fetched %s %s\n", d->name, entry->version);
  ok = 1;
done:
  if(!ok && *manifest) {
    json_free(*manifest);
    *manifest = NULL;
  }
  if(tmp) remove_tree(tmp);
  remove_tree(zip);
  free(data);
  free(root);
  free(tmp);
  free(zip);
  return ok;
}

#ifdef _WIN32
/** A junction's reparse data, as FSCTL_SET_REPARSE_POINT takes it. */
typedef struct mount_point {
  DWORD tag;
  WORD data_len, reserved;
  WORD substitute_at, substitute_len, print_at, print_len;
  WCHAR names[1];
} mount_point;
#endif

/** Whether `path` is a link, its target in `out` of `cap` bytes: its text on POSIX, and on
 * Windows the full path of the directory it reaches. */
static int read_link(const char* path, char* out, size_t cap) {
#ifdef _WIN32
  if(kind_of(path) != 'l') return 0;
  HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if(h == INVALID_HANDLE_VALUE) return 0;
  DWORD n = GetFinalPathNameByHandleA(h, out, (DWORD)cap, FILE_NAME_NORMALIZED);
  CloseHandle(h);
  if(n == 0 || n >= cap) return 0;
  if(strncmp(out, "\\\\?\\", 4) == 0) memmove(out, out + 4, n - 3);
  jello_slashes_forward(out);
  return 1;
#else
  ssize_t n = readlink(path, out, cap - 1);
  if(n < 0) return 0;
  out[n] = '\0';
  return 1;
#endif
}

/** Makes `dest` a link to directory `target`, which `link_text` names from beside `dest`: a
 * symbolic link holding `link_text`, or on Windows a junction to `target`'s full path, which needs
 * no privilege; 0, or -1 with errno set. */
static int make_link(const char* dest, const char* target, const char* link_text) {
#ifdef _WIN32
  char full[PATH_MAX];
  WCHAR wide[PATH_MAX];
  int n = jello_real_path(target, full, sizeof full)
              ? MultiByteToWideChar(CP_UTF8, 0, full, -1, wide, PATH_MAX) - 1
              : -1;
  if(n <= 0) return errno = ENOENT, -1;
  for(int i = 0; i < n; i++)
    if(wide[i] == L'/') wide[i] = L'\\';
  /* The names: the substitute, \??\ and the path, then the path to print, each NUL-ended. */
  size_t substitute = (size_t)(n + 4) * sizeof(WCHAR), print = (size_t)n * sizeof(WCHAR);
  size_t data = 8 + substitute + sizeof(WCHAR) + print + sizeof(WCHAR);
  mount_point* m = (mount_point*)calloc(1, 8 + data);
  if(!m) return errno = ENOMEM, -1;
  m->tag = IO_REPARSE_TAG_MOUNT_POINT;
  m->data_len = (WORD)data;
  m->substitute_len = (WORD)substitute;
  m->print_at = (WORD)(substitute + sizeof(WCHAR));
  m->print_len = (WORD)print;
  memcpy(m->names, L"\\??\\", 4 * sizeof(WCHAR));
  memcpy(m->names + 4, wide, print);
  memcpy((char*)m->names + m->print_at, wide, print);
  int ok = CreateDirectoryA(dest, NULL) != 0;
  HANDLE h = ok ? CreateFileA(dest, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                : INVALID_HANDLE_VALUE;
  DWORD bytes = 0;
  ok = h != INVALID_HANDLE_VALUE &&
       DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, m, (DWORD)(8 + data), NULL, 0, &bytes, NULL);
  DWORD why = ok ? 0 : GetLastError();
  if(h != INVALID_HANDLE_VALUE) CloseHandle(h);
  if(!ok) RemoveDirectoryA(dest);
  free(m);
  (void)link_text;
  if(ok) return 0;
  errno = why == ERROR_NOT_SUPPORTED || why == ERROR_INVALID_FUNCTION ? ENOTSUP : EACCES;
  return -1;
#else
  (void)target;
  return symlink(link_text, dest);
#endif
}

/** Links `dest`, packages/NAME, to `d`'s directory, which must hold the package; sets `*changed`
 * when it relinks. */
static int link_path(const dep* d, const char* root, const char* dest, json** manifest,
                     int* changed) {
  int absolute = jello_path_root_len(d->source) > 0;
  char* target = absolute ? xstrdup(d->source) : path_of("%s/%s", root, d->source);
  // From packages/, the project's root is one level up.
  char* link = absolute ? xstrdup(d->source) : path_of("../%s", d->source);
  int ok = package_is(target, d->source, d->name, NULL, manifest);
  char now[PATH_MAX];
#ifdef _WIN32
  char want[PATH_MAX];
  int same = ok && read_link(dest, now, sizeof now) && jello_real_path(target, want, sizeof want) &&
             _stricmp(now, want) == 0;
#else
  int same = ok && read_link(dest, now, sizeof now) && strcmp(now, link) == 0;
#endif
  if(ok && !same) {
    ok = remove_tree(dest);
    if(ok && make_link(dest, target, link) != 0) {
      fprintf(stderr, "jlo_tool: cannot link %s: %s\n", dest, strerror(errno));
      ok = 0;
    }
    if(ok) {
      fprintf(stderr, "linked %s to %s\n", d->name, d->source);
      *changed = 1;
    }
  }
  free(target);
  free(link);
  return ok;
}

/** Removes what `packages` holds that nothing in `wanted` asks for; sets `*changed` if any. */
static int prune(const char* packages, const deps* wanted, int* changed) {
  DIR* dir = opendir(packages);
  if(!dir) return 1;
  int ok = 1;
  struct dirent* e;
  while(ok && (e = readdir(dir))) {
    int needed = e->d_name[0] == '.';
    for(size_t i = 0; i < wanted->count && !needed; i++)
      needed = strcmp(wanted->items[i].name, e->d_name) == 0;
    if(needed) continue;
    char* path = path_of("%s/%s", packages, e->d_name);
    ok = remove_tree(path);
    free(path);
    if(ok) {
      fprintf(stderr, "removed %s\n", e->d_name);
      *changed = 1;
    }
  }
  closedir(dir);
  return ok;
}

static int get_deps(const char* root) {
  char* manifest_path = path_of("%s/jello.json", root);
  char* lock_path = path_of("%s/jello.lock", root);
  char* packages = path_of("%s/packages", root);
  json* manifest = NULL;
  lock old = {0}, now = {0};
  deps queue = {0};
  int ok = 0, changed = 0;
  if(!is_file(manifest_path)) {
    fprintf(stderr, "jlo_tool: no jello.json in %s (jlo_tool init makes one)\n", root);
    goto done;
  }
  if(!(manifest = json_read(manifest_path)) || !lock_read(lock_path, &old)) goto done;
  if(!json_string_at(manifest, "name")) {
    fprintf(stderr, "jlo_tool: %s has no \"name\"\n", manifest_path);
    goto done;
  }
  if(!manifest_deps(manifest, manifest_path, "jello.json", 1, &queue) ||
     (queue.count && !make_dir(packages)))
    goto done;
  for(size_t i = 0; i < queue.count; i++) {
    const dep* d = &queue.items[i];
    const dep* first = NULL;
    for(size_t k = 0; k < i && !first; k++)
      if(strcmp(queue.items[k].name, d->name) == 0) first = &queue.items[k];
    if(first) {
      if(first->kind == d->kind && strcmp(first->source, d->source) == 0) continue;
      fprintf(
          stderr,
          "jlo_tool: %s asks for %s from %s, and %s from %s; there is one copy of each package\n",
          first->by, d->name, first->source, d->by, d->source);
      goto done;
    }
    char* dest = path_of("%s/%s", packages, d->name);
    json* pm = NULL;
    int got;
    if(d->kind == DEP_PATH) {
      got = link_path(d, root, dest, &pm, &changed);
    } else {
      locked* pin = lock_find(&old, d->name);
      if(pin && strcmp(pin->url, d->source) != 0) pin = NULL;
      locked entry;
      if(pin && package_is(dest, NULL, d->name, pin->version, &pm)) {
        entry = locked_new(pin->name, pin->version, pin->url, pin->sha256);
        got = 1;
      } else {
        got = fetch(d, packages, dest, pin ? pin->sha256 : NULL, &entry, &pm);
        changed |= got;
      }
      if(got) {
        now.items = xrealloc(now.items, (now.count + 1) * sizeof *now.items);
        now.items[now.count++] = entry;
      }
    }
    char* where = path_of("%s/jello.json", dest);
    // `d` may dangle once `queue` grows; its name does not.
    got = got && manifest_deps(pm, where, d->name, 0, &queue);
    free(where);
    free(dest);
    json_free(pm);
    if(!got) goto done;
  }
  if(!prune(packages, &queue, &changed)) goto done;
  text t = {0};
  lock_text(&now, &t);
  size_t len = 0;
  char* before = read_file(lock_path, &len);
  if((now.count || before) && !(before && len == t.len && memcmp(before, t.data, len) == 0)) {
    ok = write_file(lock_path, t.data, t.len);
    changed = 1;
  } else {
    ok = 1;
  }
  free(before);
  free(t.data);
  if(ok && !changed) fputs("dependencies up to date\n", stderr);
done:
  json_free(manifest);
  lock_free(&old);
  lock_free(&now);
  deps_free(&queue);
  free(manifest_path);
  free(lock_path);
  free(packages);
  return ok ? 0 : 1;
}

/** Writes `body` to `path` unless something is already there. */
static int write_new(const char* path, const char* body) {
  if(kind_of(path)) return 1;
  return write_file(path, body, strlen(body));
}

static int init(const char* dir, const char* name) {
  char* manifest = path_of("%s/jello.json", dir);
  char* src = path_of("%s/src", dir);
  char* main_path = path_of("%s/src/Main.jello", dir);
  char* ignore = path_of("%s/.gitignore", dir);
  char resolved[PATH_MAX];
  text t = {0};
  int ok = 0;
  if(!make_dir(dir)) goto done;
  if(is_file(manifest)) {
    fprintf(stderr, "jlo_tool: %s is already a project: it has jello.json\n", dir);
    goto done;
  }
  if(!name) {
    if(!jello_real_path(dir, resolved, sizeof resolved)) {
      fprintf(stderr, "jlo_tool: %s: %s\n", dir, strerror(errno ? errno : ENOENT));
      goto done;
    }
    const char* slash = jello_last_slash(resolved);
    name = slash ? slash + 1 : resolved;
  }
  if(!valid_name(name)) {
    fprintf(
        stderr,
        "jlo_tool: \"%s\" cannot name a package (letters, digits, '.', '_' and '-'); give --name\n",
        name);
    goto done;
  }
  text_puts(&t, "{\n  \"name\": ");
  json_quote(&t, name);
  text_puts(&t, ",\n  \"version\": \"0.1.0\",\n  \"dependencies\": {}\n}\n");
  ok = make_dir(src) &&
       write_new(main_path, "// The program's last value is its output.\n\"Hello, world!\"\n") &&
       write_new(ignore, "/build/\n/packages/\n") && write_file(manifest, t.data, t.len);
  if(ok)
    fprintf(stderr, "made %s in %s: jlo_tool get-deps fetches what jello.json depends on\n", name,
            dir);
done:
  free(t.data);
  free(manifest);
  free(src);
  free(main_path);
  free(ignore);
  return ok ? 0 : 1;
}

int jlo_get_deps_main(int argc, char** argv) {
  if(argc > 1 || (argc == 1 && argv[0][0] == '-')) {
    fputs("usage: jlo_tool get-deps [DIR]\n", stderr);
    return 2;
  }
  return get_deps(argc ? argv[0] : ".");
}

int jlo_init_main(int argc, char** argv) {
  const char* dir = NULL;
  const char* name = NULL;
  for(int i = 0; i < argc; i++) {
    if(strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
    else if(argv[i][0] != '-' && !dir) dir = argv[i];
    else {
      fputs("usage: jlo_tool init [DIR] [--name NAME]\n", stderr);
      return 2;
    }
  }
  return init(dir ? dir : ".", name);
}
