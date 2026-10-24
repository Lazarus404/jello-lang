// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Math.*, Float.is_nan, Float.is_infinite, and parsing the text of a number for Float.parse and
 * Integer.parse. */

#include "std.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* The sign of X; NaN and either zero are themselves. */
static double sign(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : x; }
/* The cube root of X, exact for an exact cube, which glibc's cbrt can miss by an ulp. */
static double exact_cbrt(double x) {
  double r = cbrt(x), n = round(r);
  return n * n * n == x ? n : r;
}
/* The lesser of X and Y, NaN if either is, and -0 below +0. */
static double min(double x, double y) {
  if(isnan(x) || isnan(y)) return NAN;
  return x < y || (x == y && signbit(x)) ? x : y;
}
/* The greater of X and Y, NaN if either is, and +0 above -0. */
static double max(double x, double y) {
  if(isnan(x) || isnan(y)) return NAN;
  return x > y || (x == y && !signbit(x)) ? x : y;
}

#define STD_UNARY(name, f)                                                                         \
  void std_math_##name(jdlo_ctx* c) { jdl_return_f64(c, f(jdl_arg_f64(c, 0))); }
#define STD_BINARY(name, f)                                                                        \
  void std_math_##name(jdlo_ctx* c) { jdl_return_f64(c, f(jdl_arg_f64(c, 0), jdl_arg_f64(c, 1))); }
STD_MATH_UNARY(STD_UNARY)
STD_MATH_BINARY(STD_BINARY)

/* A value in [0, 1) from splitmix64 over a counter, which an atomic add makes safe across threads;
 * the counter starts from the clock. Not for cryptography. */
void std_math_random(jdlo_ctx* c) {
  static uint64_t counter;
  static int seeded;
  if(!__atomic_load_n(&seeded, __ATOMIC_RELAXED) &&
     !__atomic_exchange_n(&seeded, 1, __ATOMIC_RELAXED)) {
    __atomic_store_n(&counter, (uint64_t)time(NULL) * 0x9e3779b97f4a7c15u ^ (uint64_t)clock(),
                     __ATOMIC_RELAXED);
  }
  uint64_t z = __atomic_add_fetch(&counter, 0x9e3779b97f4a7c15u, __ATOMIC_RELAXED);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
  z ^= z >> 31;
  jdl_return_f64(c, (double)(z >> 11) * 0x1p-53);
}

/* The leading zero bits of the I32 as 32 bits; 32 for 0. */
void std_math_clz32(jdlo_ctx* c) {
  uint32_t x = (uint32_t)jdl_arg_i32(c, 0);
  jdl_return_i32(c, x == 0 ? 32 : __builtin_clz(x));
}

void std_float_is_nan(jdlo_ctx* c) { jdl_return_bool(c, isnan(jdl_arg_f64(c, 0))); }
void std_float_is_infinite(jdlo_ctx* c) { jdl_return_bool(c, isinf(jdl_arg_f64(c, 0))); }

/* Whether S[0, LEN) is ASCII digits, at least one, from S[*AT]; *AT moves past them. */
static int digits(const uint8_t* s, uint32_t len, uint32_t* at) {
  uint32_t start = *at;
  while(*at < len && s[*at] >= '0' && s[*at] <= '9') (*at)++;
  return *at > start;
}

/* Whether the text is a number Float.parse reads; a sign, digits with a point among or after them
 * or a point and digits, then an exponent, or NaN, Infinity or -Infinity. */
void std_float_valid(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0), at = 0;
  if(len == 3 && memcmp(s, "NaN", 3) == 0) {
    jdl_return_bool(c, 1);
    return;
  }
  if(len > 0 && (s[0] == '+' || s[0] == '-')) at = 1;
  if(len - at == 8 && memcmp(s + at, "Infinity", 8) == 0) {
    jdl_return_bool(c, 1);
    return;
  }
  int whole = digits(s, len, &at), fraction = 0;
  if(at < len && s[at] == '.') {
    at++;
    fraction = digits(s, len, &at);
  }
  int ok = whole || fraction;
  if(ok && at < len && (s[at] == 'e' || s[at] == 'E')) {
    at++;
    if(at < len && (s[at] == '+' || s[at] == '-')) at++;
    ok = digits(s, len, &at);
  }
  jdl_return_bool(c, ok && at == len);
}

/* The text as an integer: its bits as an I64, and whether it is a sign then decimal digits whose
 * value a type of BITS bits, unsigned with UNSIGNED, holds. An unsigned type takes no minus. */
static int integer_value(jdlo_ctx* c, int bits, int unsigned_, int64_t* value) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0), at = 0;
  int negative = len > 0 && s[0] == '-';
  if(len > 0 && (s[0] == '+' || s[0] == '-')) at = 1;
  uint64_t limit = unsigned_ ? (bits == 64 ? UINT64_MAX : (1ull << bits) - 1)
                             : (1ull << (bits - 1)) - (negative ? 0 : 1);
  uint64_t magnitude = 0;
  uint32_t start = at;
  for(; at < len && s[at] >= '0' && s[at] <= '9'; at++) {
    uint64_t digit = (uint64_t)(s[at] - '0');
    if(magnitude > (limit - digit) / 10) return 0;
    magnitude = magnitude * 10 + digit;
  }
  *value = (int64_t)(negative ? 0 - magnitude : magnitude);
  return at > start && at == len && !(negative && unsigned_);
}
void std_integer_valid(jdlo_ctx* c) {
  int64_t value;
  jdl_return_bool(c, integer_value(c, jdl_arg_i32(c, 1), jdl_arg_bool(c, 2), &value));
}
/* The bits of valid text (std_integer_valid); a U64 above I64's range comes out negative. */
void std_integer_parse(jdlo_ctx* c) {
  int64_t value = 0;
  integer_value(c, 64, jdl_arg_bytes_len(c, 0) > 0 && jdl_arg_bytes_data(c, 0)[0] != '-', &value);
  jdl_return_i64(c, value);
}
