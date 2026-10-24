// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* The exports of std.jdll, the standard native library (std.c lists them). */

#ifndef JELLO_STD_H
#define JELLO_STD_H

#include <jello/jdll.h>

/* The longest path, and the largest file, a native reads. */
#define STD_MAX_PATH 4096u
#define STD_MAX_FILE (256u * 1024u * 1024u)

/* The NUL-terminated entries of Bytes argument INDEX, copied into *STORE, as a
 * NULL-terminated array of *COUNT entries; NULL if the Bytes do not end with a NUL or memory
 * runs out. The caller frees both. */
char** std_entries(jdlo_ctx* c, int index, char** store, size_t* count);

/* Writes the directory for temporary files to OUT, CAP bytes, NUL-terminated, with "/" between
 * its components and none at the end unless it is the root: $TMPDIR, then on Windows %TEMP% or
 * %TMP%, else /tmp. Returns 0 when it does not fit. */
int std_temp_dir(char* out, size_t cap);

#ifdef _WIN32
/* Starts the program ARGV[0] names, found as command_start finds it, with ARGV, environment
 * block ENV (NULL for this one's), and the HANDLEs IN, OUT and ERR, which must be inheritable,
 * as its standard streams, and no other handle; its process HANDLE, or NULL with why in REASON,
 * CAP bytes. */
void* std_spawn(char** argv, const char* env, void* in, void* out, void* err, char* reason,
                size_t cap);
#endif

/* Whether V points at a heap object of KIND. */
static inline int std_is_heap(jello_value v, jello_obj_kind kind) {
  return jello_is_ptr(v) && jello_as_ptr(v) &&
         ((const jello_obj_header*)jello_as_ptr(v))->kind == (uint8_t)kind;
}

void std_regexp_compile(jdlo_ctx* c);
void std_regexp_source(jdlo_ctx* c);
void std_regexp_flags(jdlo_ctx* c);
void std_regexp_match_prefix(jdlo_ctx* c);
void std_regexp_match(jdlo_ctx* c);
void std_regexp_full_match(jdlo_ctx* c);
void std_regexp_match_len(jdlo_ctx* c);
void std_regexp_capture_count(jdlo_ctx* c);
void std_regexp_capture_bytes(jdlo_ctx* c);
void std_regexp_capture_start(jdlo_ctx* c);
void std_regexp_text(jdlo_ctx* c);
void std_regexp_check(jdlo_ctx* c);
void std_regexp_match_at(jdlo_ctx* c);
void std_regexp_find_at(jdlo_ctx* c);
void std_regexp_capture_named(jdlo_ctx* c);
void std_regexp_replace(jdlo_ctx* c);
void std_regexp_escape_literal(jdlo_ctx* c);

void std_unicode_validate(jdlo_ctx* c);
void std_unicode_length(jdlo_ctx* c);
void std_unicode_get(jdlo_ctx* c);
void std_unicode_compare(jdlo_ctx* c);
void std_unicode_sub(jdlo_ctx* c);
void std_unicode_from_codepoint(jdlo_ctx* c);

/* Whether S[0, LEN) is well-formed UTF-8 (no overlong forms or surrogates). */
int std_utf8_valid(const uint8_t* s, uint32_t len);

void std_file_exists(jdlo_ctx* c);
void std_file_read(jdlo_ctx* c);
void std_file_contents(jdlo_ctx* c);
void std_file_write(jdlo_ctx* c);
void std_file_remove(jdlo_ctx* c);
void std_dir_list(jdlo_ctx* c);
void std_dir_entries(jdlo_ctx* c);
void std_dir_make(jdlo_ctx* c);
void std_file_real_path(jdlo_ctx* c);
void std_process_executable(jdlo_ctx* c);
void std_get_env(jdlo_ctx* c);
void std_platform_os(jdlo_ctx* c);
void std_platform_arch(jdlo_ctx* c);
void std_platform_version(jdlo_ctx* c);
void std_platform_temp_dir(jdlo_ctx* c);
void std_argv_count(jdlo_ctx* c);
void std_args_count(jdlo_ctx* c);
void std_args_get(jdlo_ctx* c);
void std_process_exit(jdlo_ctx* c);
void std_process_exec(jdlo_ctx* c);

void std_command_start(jdlo_ctx* c);
void std_command_wait(jdlo_ctx* c);
void std_command_stdout(jdlo_ctx* c);
void std_command_stderr(jdlo_ctx* c);
void std_command_done(jdlo_ctx* c);
void std_command_kill(jdlo_ctx* c);
void std_command_next(jdlo_ctx* c);
void std_cpu_count(jdlo_ctx* c);
void std_date_now(jdlo_ctx* c);
void std_date_offset(jdlo_ctx* c);
void std_random_bytes(jdlo_ctx* c);
void std_random_int(jdlo_ctx* c);

void std_to_string(jdlo_ctx* c);
void std_object_string(jdlo_ctx* c);
void std_object_delegates(jdlo_ctx* c);
void std_object_seal(jdlo_ctx* c);
void std_u64_string(jdlo_ctx* c);

void std_f64_low_bits(jdlo_ctx* c);
void std_f64_high_bits(jdlo_ctx* c);
void std_f32_bits(jdlo_ctx* c);
void std_f16_bits(jdlo_ctx* c);

/* Writes VALUE, a float of WIDTH bits, to TEXT as Std.string shows it. */
void std_float_text(char* text, size_t size, double value, int width);
/* The value of the binary16 BITS. */
double std_half_value(uint16_t bits);
void std_float_parse(jdlo_ctx* c);

/* Math's functions of one F64 and of two, as X(name, C function). */
#define STD_MATH_UNARY(X)                                                                          \
  X(abs, fabs)                                                                                     \
  X(ceil, ceil) X(floor, floor) X(trunc, trunc) X(round, round) X(sqrt, sqrt) X(cbrt, exact_cbrt)  \
      X(exp, exp) X(expm1, expm1) X(log, log) X(log10, log10) X(log2, log2) X(log1p, log1p)        \
          X(sin, sin) X(cos, cos) X(tan, tan) X(asin, asin) X(acos, acos) X(atan, atan)            \
              X(sinh, sinh) X(cosh, cosh) X(tanh, tanh) X(asinh, asinh) X(acosh, acosh)            \
                  X(atanh, atanh) X(sign, sign)
#define STD_MATH_BINARY(X) X(pow, pow) X(atan2, atan2) X(hypot, hypot) X(min, min) X(max, max)
#define STD_DECLARE(name, f) void std_math_##name(jdlo_ctx* c);
STD_MATH_UNARY(STD_DECLARE)
STD_MATH_BINARY(STD_DECLARE)
#undef STD_DECLARE
void std_math_random(jdlo_ctx* c);
void std_math_clz32(jdlo_ctx* c);
void std_float_is_nan(jdlo_ctx* c);
void std_float_is_infinite(jdlo_ctx* c);
void std_float_valid(jdlo_ctx* c);
void std_integer_valid(jdlo_ctx* c);
void std_integer_parse(jdlo_ctx* c);

void std_quote(jdlo_ctx* c);
void std_bytes_u8(jdlo_ctx* c);
void std_bytes_hex(jdlo_ctx* c);
void std_bytes_from_hex(jdlo_ctx* c);
void std_bytes_base64(jdlo_ctx* c);
void std_bytes_from_base64(jdlo_ctx* c);
void std_bytes_read_i64(jdlo_ctx* c);
void std_bytes_read_f64(jdlo_ctx* c);
void std_bytes_i64(jdlo_ctx* c);
void std_bytes_f64(jdlo_ctx* c);

#endif /* JELLO_STD_H */
