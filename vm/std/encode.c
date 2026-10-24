// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Bytes as text and back, for std.core.Bytes: hexadecimal, and Base64 in its standard alphabet,
 * padded, or its URL alphabet, unpadded (RFC 4648). Reading text that is not in the encoding
 * gives null; running out of memory fails the call. Also the eight bytes of an I64 or F64, either
 * way round, and back. */

#include "std.h"

#include <stdlib.h>
#include <string.h>

static const char HEX[] = "0123456789abcdef";
static const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char BASE64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Returns OUT's first LEN bytes as Bytes, or fails the call when OUT is NULL; then frees OUT. */
static void return_owned(jdlo_ctx* c, uint8_t* out, uint32_t len) {
  if(out) jdl_return_bytes_copy(c, out, len);
  else jdl_fail(c, "out of memory");
  free(out);
}

/* Returns null, for text not in the encoding, and frees OUT. */
static void return_bad(jdlo_ctx* c, uint8_t* out) {
  jdl_return_null(c);
  free(out);
}

/* The value of the hexadecimal digit D, in either case, or -1. */
static int hex_value(uint8_t d) {
  if(d >= '0' && d <= '9') return d - '0';
  if(d >= 'a' && d <= 'f') return d - 'a' + 10;
  if(d >= 'A' && d <= 'F') return d - 'A' + 10;
  return -1;
}

/* The value of the Base64 digit D in the standard alphabet, or the URL one when URL, or -1. */
static int base64_value(uint8_t d, int url) {
  if(d >= 'A' && d <= 'Z') return d - 'A';
  if(d >= 'a' && d <= 'z') return d - 'a' + 26;
  if(d >= '0' && d <= '9') return d - '0' + 52;
  if(d == (url ? '-' : '+')) return 62;
  if(d == (url ? '_' : '/')) return 63;
  return -1;
}

/* Argument 0 as lowercase hexadecimal, two digits a byte. */
void std_bytes_hex(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  if(len > UINT32_MAX / 2) {
    jdl_fail(c, "too long");
    return;
  }
  uint8_t* out = (uint8_t*)malloc((size_t)len * 2 + 1);
  for(uint32_t i = 0; out && i < len; i++) {
    out[2 * i] = (uint8_t)HEX[s[i] >> 4];
    out[2 * i + 1] = (uint8_t)HEX[s[i] & 15];
  }
  return_owned(c, out, len * 2);
}

/* The bytes the hexadecimal text in argument 0 gives, or null when its length is odd or it has
 * something other than digits. */
void std_bytes_from_hex(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  if(len % 2) {
    jdl_return_null(c);
    return;
  }
  uint8_t* out = (uint8_t*)malloc((size_t)len / 2 + 1);
  for(uint32_t i = 0; out && i < len; i += 2) {
    int high = hex_value(s[i]), low = hex_value(s[i + 1]);
    if(high < 0 || low < 0) {
      return_bad(c, out);
      return;
    }
    out[i / 2] = (uint8_t)(high << 4 | low);
  }
  return_owned(c, out, len / 2);
}

/* Argument 0 in Base64: the standard alphabet with `=` padding, or, when argument 1 is true, the
 * URL alphabet without it. */
void std_bytes_base64(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int url = jdl_arg_bool(c, 1);
  const char* digits = url ? BASE64_URL : BASE64;
  if(len > UINT32_MAX / 4 * 3 - 3) {
    jdl_fail(c, "too long");
    return;
  }
  uint8_t* out = (uint8_t*)malloc(((size_t)len + 2) / 3 * 4 + 1);
  if(!out) {
    jdl_fail(c, "out of memory");
    return;
  }
  uint32_t n = 0, i = 0;
  for(; i + 3 <= len; i += 3) {
    uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
    out[n++] = (uint8_t)digits[v >> 18];
    out[n++] = (uint8_t)digits[v >> 12 & 63];
    out[n++] = (uint8_t)digits[v >> 6 & 63];
    out[n++] = (uint8_t)digits[v & 63];
  }
  if(i < len) {
    uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < len ? (uint32_t)s[i + 1] << 8 : 0);
    out[n++] = (uint8_t)digits[v >> 18];
    out[n++] = (uint8_t)digits[v >> 12 & 63];
    if(i + 1 < len) out[n++] = (uint8_t)digits[v >> 6 & 63];
    else if(!url) out[n++] = '=';
    if(!url) out[n++] = '=';
  }
  return_owned(c, out, n);
}

/* The bytes the Base64 text in argument 0 gives, in the URL alphabet when argument 1 is true; the
 * padding is optional. Null when it has anything else, or its last digit has bits left over. */
void std_bytes_from_base64(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int url = jdl_arg_bool(c, 1);
  if(len % 4 == 0 && len > 0 && s[len - 1] == '=') len -= s[len - 2] == '=' ? 2 : 1;
  if(len % 4 == 1) {
    jdl_return_null(c);
    return;
  }
  uint8_t* out = (uint8_t*)malloc((size_t)len / 4 * 3 + 3);
  uint32_t n = 0, v = 0;
  for(uint32_t i = 0; out && i < len; i++) {
    int d = base64_value(s[i], url);
    if(d < 0) {
      return_bad(c, out);
      return;
    }
    v = v << 6 | (uint32_t)d;
    if(i % 4 == 3) {
      out[n++] = (uint8_t)(v >> 16);
      out[n++] = (uint8_t)(v >> 8);
      out[n++] = (uint8_t)v;
    }
  }
  /* Two or three digits left give one or two bytes, and the bits after them must be 0. */
  uint32_t left = len % 4;
  if((left == 2 && (v & 15)) || (left == 3 && (v & 3))) {
    return_bad(c, out);
    return;
  }
  if(out && left == 2) out[n++] = (uint8_t)(v >> 4);
  if(out && left == 3) {
    out[n++] = (uint8_t)(v >> 10);
    out[n++] = (uint8_t)(v >> 2);
  }
  return_owned(c, out, n);
}

/* The eight bytes of Bytes argument 0 from index argument 1, as a 64-bit word, most significant
 * byte first when argument 2 is true; 0 when the index leaves no room, which fails the call. */
static int read_word(jdlo_ctx* c, uint64_t* word) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int32_t at = jdl_arg_i32(c, 1);
  int big = jdl_arg_bool(c, 2);
  if(at < 0 || len < 8 || (uint32_t)at > len - 8) {
    jdl_fail(c, "index out of range");
    return 0;
  }
  const uint8_t* p = s + at;
  *word = 0;
  for(int i = 0; i < 8; i++) *word |= (uint64_t)p[big ? 7 - i : i] << (8 * i);
  return 1;
}

/* Returns a 64-bit word as eight bytes, most significant first when BIG. */
static void return_word(jdlo_ctx* c, uint64_t word, int big) {
  uint8_t out[8];
  for(int i = 0; i < 8; i++) out[big ? 7 - i : i] = (uint8_t)(word >> (8 * i));
  jdl_return_bytes_copy(c, out, sizeof out);
}

/* The I64 at an index of Bytes, as read_word reads it. */
void std_bytes_read_i64(jdlo_ctx* c) {
  uint64_t word;
  if(read_word(c, &word)) jdl_return_i64(c, (int64_t)word);
}

/* The F64 at an index of Bytes, as read_word reads it. */
void std_bytes_read_f64(jdlo_ctx* c) {
  uint64_t word;
  double value;
  if(!read_word(c, &word)) return;
  memcpy(&value, &word, sizeof value);
  jdl_return_f64(c, value);
}

/* The eight bytes of the I64 argument 0, most significant first when argument 1 is true. */
void std_bytes_i64(jdlo_ctx* c) { return_word(c, (uint64_t)jdl_arg_i64(c, 0), jdl_arg_bool(c, 1)); }

/* The eight bytes of the F64 argument 0, most significant first when argument 1 is true. */
void std_bytes_f64(jdlo_ctx* c) {
  double value = jdl_arg_f64(c, 0);
  uint64_t word;
  memcpy(&word, &value, sizeof word);
  return_word(c, word, jdl_arg_bool(c, 1));
}
