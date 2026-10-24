// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Regular expressions over Bytes, backed by the vendored PCRE2 10.30
 * (vm/third_party/pcre2-10.30). Patterns compile with PCRE2_UTF.
 *
 * A compiled regexp and a match are native handles. A match owns a copy of
 * its captures, so it never points into a haystack the collector may free.
 * A failed match (including PCRE2's match limit or invalid UTF-8 in the
 * haystack) returns null, as stage 0's std.jdll did.
 *
 * PCRE2 checks that a Bytes haystack is UTF-8 from the start of the match to
 * its end, on every call. A text handle (regexp_text) is a copy validated
 * once; regexp_match_at matches it from any position without that check, so
 * scanning a text costs the matched bytes, not the rest of the text. */

#include "std.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_STATIC
#include "pcre2.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGEXP_MAX_PATTERN_LEN 8192u
#define REGEXP_MAX_CAPTURES 32u
#define REGEXP_MATCH_LIMIT 100000u
#define REGEXP_DEPTH_LIMIT 1000u
#define REGEXP_FLAGS "the flags are i, m, s, x and U"

typedef struct regexp {
  jdl_refs refs;
  pcre2_code* code;
  uint8_t* text; /* the pattern, then the flags */
  uint32_t pattern_len;
  uint32_t flags_len;
  uint32_t name_count; /* entries of name_size bytes in the name table, names */
  uint32_t name_size;
  PCRE2_SPTR names;
} regexp;

/* Capture i spans data[start[i] - base, +len[i]]; an unset capture has
 * start -1 and length 0. data starts with a copy of the regexp's name table,
 * name_count entries of name_size bytes. One allocation holds the whole
 * match, which never needs its regexp. */
typedef struct regexp_match {
  jdl_refs refs;
  uint32_t count;
  uint32_t name_count;
  uint32_t name_size;
  int32_t start[REGEXP_MAX_CAPTURES];
  uint32_t len[REGEXP_MAX_CAPTURES];
  uint32_t offset[REGEXP_MAX_CAPTURES]; /* each capture's bytes within data */
  uint8_t data[];
} regexp_match;

static void regexp_free(void* payload) {
  if(!jdl_release(payload)) return;
  regexp* re = (regexp*)payload;
  pcre2_code_free(re->code);
  free(re->text);
  free(re);
}

/* Valid UTF-8, immutable once made. */
typedef struct regexp_text {
  jdl_refs refs;
  uint32_t len;
  uint8_t data[];
} regexp_text;

static void plain_free(void* payload) {
  if(jdl_release(payload)) free(payload);
}

/* All three are immutable once made, so processes on other threads share them. */
static const jello_jdll_handle_class REGEXP = {"regexp", regexp_free, jdl_retain};
static const jello_jdll_handle_class MATCH = {"regexp match", plain_free, jdl_retain};
static const jello_jdll_handle_class TEXT = {"regexp text", plain_free, jdl_retain};

static pcre2_match_context* match_context;
static pthread_once_t match_context_once = PTHREAD_ONCE_INIT;

static void make_match_context(void) {
  match_context = pcre2_match_context_create(NULL);
  if(match_context) {
    pcre2_set_match_limit(match_context, REGEXP_MATCH_LIMIT);
    pcre2_set_depth_limit(match_context, REGEXP_DEPTH_LIMIT);
  }
}

/* The compile options of FLAGS, or 0 when one of them is no flag; *BAD is then that byte. */
static uint32_t options(const uint8_t* flags, uint32_t len, uint8_t* bad) {
  uint32_t opts = PCRE2_UTF;
  for(uint32_t i = 0; i < len; i++) {
    switch(flags[i]) {
      case 'i': opts |= PCRE2_CASELESS; break;
      case 'm': opts |= PCRE2_MULTILINE; break;
      case 's': opts |= PCRE2_DOTALL; break;
      case 'x': opts |= PCRE2_EXTENDED; break;
      case 'U': opts |= PCRE2_UNGREEDY; break;
      default: *bad = flags[i]; return 0;
    }
  }
  return opts;
}

/* Compiles PATTERN with FLAGS, or returns NULL with what is wrong written to MESSAGE. */
static pcre2_code* compile(const uint8_t* pattern, uint32_t pattern_len, const uint8_t* flags,
                           uint32_t flags_len, char* message, size_t size) {
  if(pattern_len > REGEXP_MAX_PATTERN_LEN) {
    snprintf(message, size, "regexp pattern is longer than %u bytes", REGEXP_MAX_PATTERN_LEN);
    return NULL;
  }
  uint8_t bad = 0;
  uint32_t opts = options(flags, flags_len, &bad);
  if(!opts) {
    if(bad > ' ' && bad < 0x7f)
      snprintf(message, size, "unknown regexp flag %c; " REGEXP_FLAGS, bad);
    else snprintf(message, size, "unknown regexp flag \\x%02X; " REGEXP_FLAGS, bad);
    return NULL;
  }
  int error = 0;
  PCRE2_SIZE error_offset = 0;
  pcre2_code* code = pcre2_compile(pattern_len ? (PCRE2_SPTR)pattern : (PCRE2_SPTR) "",
                                   (PCRE2_SIZE)pattern_len, opts, &error, &error_offset, NULL);
  if(!code) {
    char reason[160];
    pcre2_get_error_message(error, (PCRE2_UCHAR*)reason, sizeof reason);
    snprintf(message, size, "invalid regexp pattern: %s, at byte %u", reason,
             (unsigned)error_offset);
  }
  return code;
}

/* What is wrong with a pattern and flags, or empty when they compile. */
void std_regexp_check(jdlo_ctx* c) {
  char message[256];
  pcre2_code* code =
      compile(jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0), jdl_arg_bytes_data(c, 1),
              jdl_arg_bytes_len(c, 1), message, sizeof message);
  if(code) {
    pcre2_code_free(code);
    message[0] = 0;
  }
  jdl_return_bytes_copy(c, (const uint8_t*)message, (uint32_t)strlen(message));
}

void std_regexp_compile(jdlo_ctx* c) {
  const uint8_t* pattern = jdl_arg_bytes_data(c, 0);
  uint32_t pattern_len = jdl_arg_bytes_len(c, 0);
  const uint8_t* flags = jdl_arg_bytes_data(c, 1);
  uint32_t flags_len = jdl_arg_bytes_len(c, 1);
  pcre2_code* code = compile(pattern, pattern_len, flags, flags_len, NULL, 0);
  // Null lets std.core.RegExp.compile ask regexp_check why, compiling twice only then.
  if(!code) {
    jdl_return_null(c);
    return;
  }
  regexp* re = (regexp*)calloc(1, sizeof(regexp));
  uint8_t* text = (uint8_t*)malloc((size_t)pattern_len + flags_len + 1u);
  if(!re || !text) {
    free(re);
    free(text);
    pcre2_code_free(code);
    jdl_fail(c, "out of memory");
    return;
  }
  if(pattern_len) memcpy(text, pattern, pattern_len);
  if(flags_len) memcpy(text + pattern_len, flags, flags_len);
  re->refs = 1;
  re->code = code;
  re->text = text;
  re->pattern_len = pattern_len;
  re->flags_len = flags_len;
  pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &re->name_count);
  pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &re->name_size);
  pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &re->names);
  size_t size = 0;
  pcre2_pattern_info(code, PCRE2_INFO_SIZE, &size);
  jdl_return_handle(c, &REGEXP, re, (uint32_t)(size + pattern_len + flags_len));
}

static regexp* regexp_arg(jdlo_ctx* c, int index) {
  regexp* re = (regexp*)jdl_arg_handle(c, index, &REGEXP);
  if(!re) jdl_fail(c, "null regexp");
  return re;
}

void std_regexp_source(jdlo_ctx* c) {
  regexp* re = regexp_arg(c, 0);
  if(re) jdl_return_bytes_copy(c, re->text, re->pattern_len);
}

void std_regexp_flags(jdlo_ctx* c) {
  regexp* re = regexp_arg(c, 0);
  if(re) jdl_return_bytes_copy(c, re->text + re->pattern_len, re->flags_len);
}

/* Runs RE on HAY from byte START and returns the match's ovector size, or 0
 * when it does not match. *MD holds the match data on success. */
static int run(jdlo_ctx* c, regexp* re, const uint8_t* hay, uint32_t hay_len, uint32_t start,
               uint32_t pcre_options, pcre2_match_data** md) {
  pthread_once(&match_context_once, make_match_context);
  *md = pcre2_match_data_create_from_pattern(re->code, NULL);
  if(!*md) {
    jdl_fail(c, "out of memory");
    return 0;
  }
  int rc = pcre2_match(re->code, hay_len ? (PCRE2_SPTR)hay : (PCRE2_SPTR) "", (PCRE2_SIZE)hay_len,
                       (PCRE2_SIZE)start, pcre_options, *md, match_context);
  if(rc > (int)REGEXP_MAX_CAPTURES) {
    jdl_fail(c, "too many captures");
    rc = 0;
  }
  if(rc < 1) {
    pcre2_match_data_free(*md);
    *md = NULL;
    return 0;
  }
  return rc;
}

/* Returns a match of RE on HAY from byte START, or null. */
static void match(jdlo_ctx* c, regexp* re, const uint8_t* hay, uint32_t hay_len, uint32_t start,
                  uint32_t pcre_options) {
  pcre2_match_data* md = NULL;
  int rc = run(c, re, hay, hay_len, start, pcre_options, &md);
  if(rc == 0) {
    jdl_return_null(c);
    return;
  }
  PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
  size_t total = 0;
  for(int i = 0; i < rc; i++) {
    if(ovector[2 * i] != PCRE2_UNSET) total += ovector[2 * i + 1] - ovector[2 * i];
  }
  uint32_t names = re->name_count * re->name_size;
  total += names;
  regexp_match* m = (regexp_match*)malloc(sizeof(regexp_match) + total);
  if(!m) {
    pcre2_match_data_free(md);
    jdl_fail(c, "out of memory");
    return;
  }
  m->refs = 1;
  m->count = (uint32_t)rc;
  m->name_count = re->name_count;
  m->name_size = re->name_size;
  if(names) memcpy(m->data, re->names, names);
  uint32_t at = names;
  for(uint32_t i = 0; i < m->count; i++) {
    PCRE2_SIZE from = ovector[2 * i], to = ovector[2 * i + 1];
    int unset = from == PCRE2_UNSET;
    m->start[i] = unset ? -1 : (int32_t)from;
    m->len[i] = unset ? 0u : (uint32_t)(to - from);
    m->offset[i] = at;
    if(m->len[i]) memcpy(m->data + at, hay + from, m->len[i]);
    at += m->len[i];
  }
  pcre2_match_data_free(md);
  jdl_return_handle(c, &MATCH, m, (uint32_t)(sizeof(regexp_match) + total));
}

/* A match of the haystack argument 0 against the regexp argument 1. */
static void match_bytes(jdlo_ctx* c, uint32_t pcre_options) {
  regexp* re = regexp_arg(c, 1);
  if(re) match(c, re, jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0), 0, pcre_options);
}

void std_regexp_match_prefix(jdlo_ctx* c) { match_bytes(c, PCRE2_ANCHORED); }
void std_regexp_match(jdlo_ctx* c) { match_bytes(c, 0); }

void std_regexp_full_match(jdlo_ctx* c) {
  regexp* re = regexp_arg(c, 1);
  if(!re) return;
  pcre2_match_data* md = NULL;
  int rc = run(c, re, jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0), 0, PCRE2_ANCHORED, &md);
  int full = 0;
  if(rc > 0) {
    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(md);
    full = ovector[1] - ovector[0] == (PCRE2_SIZE)jdl_arg_bytes_len(c, 0);
    pcre2_match_data_free(md);
  }
  jdl_return_bool(c, full);
}

/* A null match has no captures; its length is 0 and its groups are empty. */
void std_regexp_match_len(jdlo_ctx* c) {
  regexp_match* m = (regexp_match*)jdl_arg_handle(c, 0, &MATCH);
  jdl_return_i32(c, m ? (int32_t)m->len[0] : 0);
}

void std_regexp_capture_count(jdlo_ctx* c) {
  regexp_match* m = (regexp_match*)jdl_arg_handle(c, 0, &MATCH);
  jdl_return_i32(c, m ? (int32_t)m->count : 0);
}

void std_regexp_capture_bytes(jdlo_ctx* c) {
  regexp_match* m = (regexp_match*)jdl_arg_handle(c, 0, &MATCH);
  int32_t i = jdl_arg_i32(c, 1);
  if(!m || i < 0 || (uint32_t)i >= m->count) {
    jdl_return_bytes_copy(c, NULL, 0);
    return;
  }
  jdl_return_bytes_copy(c, m->data + m->offset[i], m->len[i]);
}

void std_regexp_capture_start(jdlo_ctx* c) {
  regexp_match* m = (regexp_match*)jdl_arg_handle(c, 0, &MATCH);
  int32_t i = jdl_arg_i32(c, 1);
  jdl_return_i32(c, (!m || i < 0 || (uint32_t)i >= m->count) ? -1 : m->start[i]);
}

/* Invalid UTF-8 fails here, once, rather than failing every match. */
void std_regexp_text(jdlo_ctx* c) {
  const uint8_t* data = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  if(!std_utf8_valid(data, len)) {
    jdl_fail(c, "invalid UTF-8");
    return;
  }
  regexp_text* t = (regexp_text*)malloc(sizeof(regexp_text) + len);
  if(!t) {
    jdl_fail(c, "out of memory");
    return;
  }
  t->refs = 1;
  t->len = len;
  if(len) memcpy(t->data, data, len);
  jdl_return_handle(c, &TEXT, t, (uint32_t)(sizeof(regexp_text) + len));
}

/* A match of the regexp argument 1 on the text argument 0 from byte argument
 * 2, or null. The position must start a character (or be the end); PCRE2
 * skips its UTF-8 check, which the text already passed. */
static void match_text(jdlo_ctx* c, uint32_t pcre_options) {
  regexp_text* t = (regexp_text*)jdl_arg_handle(c, 0, &TEXT);
  if(!t) {
    jdl_fail(c, "null text");
    return;
  }
  regexp* re = regexp_arg(c, 1);
  if(!re) return;
  int32_t at = jdl_arg_i32(c, 2);
  if(at < 0 || (uint32_t)at > t->len || ((uint32_t)at < t->len && (t->data[at] & 0xC0u) == 0x80u)) {
    jdl_fail(c, "position is not at a character");
    return;
  }
  match(c, re, t->data, t->len, (uint32_t)at, pcre_options | PCRE2_NO_UTF_CHECK);
}

/* A match starting exactly at the position. */
void std_regexp_match_at(jdlo_ctx* c) { match_text(c, PCRE2_ANCHORED); }
/* The first match at or after the position. */
void std_regexp_find_at(jdlo_ctx* c) { match_text(c, 0); }

/* The capture of the match argument 0 named by argument 1; the first of that
 * name that is set, or empty when none is. A name the regexp does not have fails. */
void std_regexp_capture_named(jdlo_ctx* c) {
  regexp_match* m = (regexp_match*)jdl_arg_handle(c, 0, &MATCH);
  const uint8_t* name = jdl_arg_bytes_data(c, 1);
  uint32_t name_len = jdl_arg_bytes_len(c, 1);
  int found = !m;
  /* An entry is the group's number in two bytes, then the name, NUL-terminated. */
  for(uint32_t e = 0; m && name_len + 3u <= m->name_size && e < m->name_count; e++) {
    const uint8_t* entry = m->data + (size_t)e * m->name_size;
    if(memcmp(entry + 2, name, name_len) != 0 || entry[2 + name_len] != 0) continue;
    found = 1;
    uint32_t group = ((uint32_t)entry[0] << 8) | entry[1];
    if(group < m->count && m->start[group] >= 0) {
      jdl_return_bytes_copy(c, m->data + m->offset[group], m->len[group]);
      return;
    }
  }
  if(found) jdl_return_bytes_copy(c, NULL, 0);
  else jdl_fail(c, "no group of that name");
}

/* Substitutes WITH for every match of RE on HAY into OUT, of *LEN bytes; *LEN
 * becomes the result's length, or with PCRE2_ERROR_NOMEMORY the size it needs. */
static int substitute(regexp* re, pcre2_match_data* md, const uint8_t* hay, uint32_t hay_len,
                      const uint8_t* with, uint32_t with_len, uint8_t* out, PCRE2_SIZE* len) {
  return pcre2_substitute(
      re->code, hay_len ? (PCRE2_SPTR)hay : (PCRE2_SPTR) "", (PCRE2_SIZE)hay_len, 0,
      PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH | PCRE2_SUBSTITUTE_UNSET_EMPTY, md,
      match_context, with_len ? (PCRE2_SPTR)with : (PCRE2_SPTR) "", (PCRE2_SIZE)with_len, out, len);
}

/* Argument 0 with every match of the regexp argument 1 replaced by argument
 * 2, in which $0 to $99, ${name} and $$ stand for the match's captures and $.
 * An unset capture is empty; an unknown one, or invalid UTF-8, fails. */
void std_regexp_replace(jdlo_ctx* c) {
  regexp* re = regexp_arg(c, 1);
  if(!re) return;
  const uint8_t* hay = jdl_arg_bytes_data(c, 0);
  uint32_t hay_len = jdl_arg_bytes_len(c, 0);
  const uint8_t* with = jdl_arg_bytes_data(c, 2);
  uint32_t with_len = jdl_arg_bytes_len(c, 2);
  pthread_once(&match_context_once, make_match_context);
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(re->code, NULL);
  PCRE2_SIZE len = (PCRE2_SIZE)hay_len + with_len + 64u;
  uint8_t* out = (uint8_t*)malloc(len);
  int rc = md && out ? substitute(re, md, hay, hay_len, with, with_len, out, &len)
                     : PCRE2_ERROR_NOMEMORY;
  if(rc == PCRE2_ERROR_NOMEMORY && md && out) {
    free(out);
    out = (uint8_t*)malloc(len);
    if(out) rc = substitute(re, md, hay, hay_len, with, with_len, out, &len);
  }
  if(md) pcre2_match_data_free(md);
  if(rc == PCRE2_ERROR_NOMEMORY) jdl_fail(c, "out of memory");
  else if(rc < 0) jdl_fail(c, "invalid replacement or subject");
  else jdl_return_bytes_copy(c, out, (uint32_t)len);
  free(out);
}

/* Argument 0 with a backslash before each ASCII byte that is not a letter or
 * a digit, so that a regexp of it matches it literally under any flags. */
void std_regexp_escape_literal(jdlo_ctx* c) {
  const uint8_t* text = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  uint8_t* out = (uint8_t*)malloc((size_t)len * 2u + 1u);
  if(!out) {
    jdl_fail(c, "out of memory");
    return;
  }
  uint32_t n = 0;
  for(uint32_t i = 0; i < len; i++) {
    uint8_t b = text[i];
    int word = (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
    if(b < 0x80 && !word) out[n++] = '\\';
    out[n++] = b;
  }
  jdl_return_bytes_copy(c, out, n);
  free(out);
}
