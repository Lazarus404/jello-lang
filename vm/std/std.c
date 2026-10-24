// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* std.jdll's manifest; every export's name and exact signature. The VM checks
 * a module's native imports against this table when it loads the module. W
 * marks one that may wait on the file system or a child command. */

#include "std.h"

#define B (uint8_t)JELLO_T_BOOL
#define I (uint8_t)JELLO_T_I32
#define Y (uint8_t)JELLO_T_BYTES
#define D (uint8_t)JELLO_T_DYNAMIC
#define L (uint8_t)JELLO_T_I64
#define F (uint8_t)JELLO_T_F64
#define W JELLO_JDLL_BLOCKING
#define UNARY(name, f) {"math_" #name, std_math_##name, F, 1, {F}, 0},
#define BINARY(name, f) {"math_" #name, std_math_##name, F, 2, {F, F}, 0},

static const jello_jdll_export STD_EXPORTS[] = {
    {"regexp_compile", std_regexp_compile, D, 2, {Y, Y}, 0},
    {"regexp_source", std_regexp_source, Y, 1, {D}, 0},
    {"regexp_flags", std_regexp_flags, Y, 1, {D}, 0},
    {"regexp_match_prefix", std_regexp_match_prefix, D, 2, {Y, D}, 0},
    {"regexp_match", std_regexp_match, D, 2, {Y, D}, 0},
    {"regexp_full_match", std_regexp_full_match, B, 2, {Y, D}, 0},
    {"regexp_match_len", std_regexp_match_len, I, 1, {D}, 0},
    {"regexp_capture_count", std_regexp_capture_count, I, 1, {D}, 0},
    {"regexp_capture_bytes", std_regexp_capture_bytes, Y, 2, {D, I}, 0},
    {"regexp_capture_start", std_regexp_capture_start, I, 2, {D, I}, 0},
    {"regexp_text", std_regexp_text, D, 1, {Y}, 0},
    {"regexp_check", std_regexp_check, Y, 2, {Y, Y}, 0},
    {"regexp_match_at", std_regexp_match_at, D, 3, {D, D, I}, 0},
    {"regexp_find_at", std_regexp_find_at, D, 3, {D, D, I}, 0},
    {"regexp_capture_named", std_regexp_capture_named, Y, 2, {D, Y}, 0},
    {"regexp_replace", std_regexp_replace, Y, 3, {Y, D, Y}, 0},
    {"regexp_escape_literal", std_regexp_escape_literal, Y, 1, {Y}, 0},
    {"unicode_validate", std_unicode_validate, B, 1, {Y}, 0},
    {"unicode_length", std_unicode_length, I, 1, {Y}, 0},
    {"unicode_get", std_unicode_get, I, 2, {Y, I}, 0},
    {"unicode_compare", std_unicode_compare, I, 2, {Y, Y}, 0},
    {"unicode_sub", std_unicode_sub, Y, 3, {Y, I, I}, 0},
    {"unicode_from_codepoint", std_unicode_from_codepoint, Y, 1, {I}, 0},
    {"file_exists", std_file_exists, B, 1, {Y}, W},
    {"file_read", std_file_read, Y, 1, {Y}, W},
    {"file_contents", std_file_contents, D, 1, {Y}, W},
    {"file_write", std_file_write, B, 2, {Y, Y}, W},
    {"file_remove", std_file_remove, B, 1, {Y}, W},
    {"dir_list", std_dir_list, Y, 1, {Y}, W},
    {"dir_entries", std_dir_entries, D, 1, {Y}, W},
    {"dir_make", std_dir_make, B, 1, {Y}, W},
    {"file_real_path", std_file_real_path, Y, 1, {Y}, W},
    {"process_executable", std_process_executable, Y, 0, {0}, 0},
    {"get_env", std_get_env, Y, 1, {Y}, 0},
    {"platform_os", std_platform_os, Y, 0, {0}, 0},
    {"platform_arch", std_platform_arch, Y, 0, {0}, 0},
    {"platform_version", std_platform_version, Y, 0, {0}, 0},
    {"platform_temp_dir", std_platform_temp_dir, Y, 0, {0}, 0},
    {"argv_count", std_argv_count, I, 0, {0}, 0},
    {"args_count", std_args_count, I, 0, {0}, 0},
    {"args_get", std_args_get, Y, 1, {I}, 0},
    {"process_exit", std_process_exit, I, 1, {I}, 0},
    {"process_exec", std_process_exec, Y, 1, {Y}, 0},
    {"command_start", std_command_start, D, 2, {Y, Y}, 0},
    {"command_wait", std_command_wait, I, 1, {D}, W},
    {"command_stdout", std_command_stdout, Y, 1, {D}, W},
    {"command_stderr", std_command_stderr, Y, 1, {D}, W},
    {"command_done", std_command_done, B, 1, {D}, 0},
    {"command_kill", std_command_kill, B, 1, {D}, 0},
    {"command_next", std_command_next, B, 0, {0}, W},
    {"cpu_count", std_cpu_count, I, 0, {0}, 0},
    {"date_now", std_date_now, L, 0, {0}, 0},
    {"date_offset", std_date_offset, I, 1, {L}, 0},
    {"random_bytes", std_random_bytes, Y, 1, {I}, 0},
    {"random_int", std_random_int, I, 2, {I, I}, 0},
    {"to_string", std_to_string, Y, 1, {D}, 0},
    {"object_string", std_object_string, Y, 1, {D}, 0},
    {"object_delegates", std_object_delegates, B, 2, {D, D}, 0},
    {"object_seal", std_object_seal, B, 3, {D, D, D}, 0},
    {"u64_string", std_u64_string, Y, 1, {L}, 0},
    {"f64_low_bits", std_f64_low_bits, I, 1, {Y}, 0},
    {"f64_high_bits", std_f64_high_bits, I, 1, {Y}, 0},
    {"f32_bits", std_f32_bits, I, 1, {Y}, 0},
    {"f16_bits", std_f16_bits, I, 1, {Y}, 0},
    {"float_parse", std_float_parse, F, 1, {Y}, 0},
    STD_MATH_UNARY(UNARY) STD_MATH_BINARY(BINARY){"math_random", std_math_random, F, 0, {0}, 0},
    {"math_clz32", std_math_clz32, I, 1, {I}, 0},
    {"float_is_nan", std_float_is_nan, B, 1, {F}, 0},
    {"float_is_infinite", std_float_is_infinite, B, 1, {F}, 0},
    {"float_valid", std_float_valid, B, 1, {Y}, 0},
    {"integer_valid", std_integer_valid, B, 3, {Y, I, B}, 0},
    {"integer_parse", std_integer_parse, L, 1, {Y}, 0},
    {"quote", std_quote, Y, 1, {Y}, 0},
    {"bytes_u8", std_bytes_u8, Y, 1, {I}, 0},
    {"bytes_hex", std_bytes_hex, Y, 1, {Y}, 0},
    {"bytes_from_hex", std_bytes_from_hex, D, 1, {Y}, 0},
    {"bytes_base64", std_bytes_base64, Y, 2, {Y, B}, 0},
    {"bytes_from_base64", std_bytes_from_base64, D, 2, {Y, B}, 0},
    {"bytes_read_i64", std_bytes_read_i64, L, 3, {Y, I, B}, 0},
    {"bytes_read_f64", std_bytes_read_f64, F, 3, {Y, I, B}, 0},
    {"bytes_i64", std_bytes_i64, Y, 2, {L, B}, 0},
    {"bytes_f64", std_bytes_f64, Y, 2, {F, B}, 0},
};

/* The one symbol std.jdll exports; everything else is hidden. */
JELLO_JDLL_LIBRARY(std, STD_EXPORTS);
