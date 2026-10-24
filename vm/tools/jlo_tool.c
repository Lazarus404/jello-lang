// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jlo_tool, the `.jlo` workbench; `asm` writes a module from its text form, `disasm` prints a
 * loaded module in that form, `run` runs one (also as a benchmark), `dumpir` prints the JIT's IR,
 * and `init` and `get-deps` manage a project (deps.c). It reads modules only through
 * jello_bc_read and uses the VM's internal headers. */

#include <jello/internal.h>
#include "deps.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#define JLO_MAGIC 0x4A4C5942u

// Type kinds by their names in the text.
typedef struct kind_name_entry {
  const char* name;
  jello_type_kind kind;
} kind_name_entry;

static const kind_name_entry KIND_NAMES[] = {
    {"VOID", JELLO_T_VOID},       {"BOOL", JELLO_T_BOOL},         {"ATOM", JELLO_T_ATOM},
    {"I8", JELLO_T_I8},           {"I16", JELLO_T_I16},           {"I32", JELLO_T_I32},
    {"I64", JELLO_T_I64},         {"F16", JELLO_T_F16},           {"F32", JELLO_T_F32},
    {"F64", JELLO_T_F64},         {"BYTES", JELLO_T_BYTES},       {"LIST", JELLO_T_LIST},
    {"ARRAY", JELLO_T_ARRAY},     {"STRUCT", JELLO_T_STRUCT},     {"OBJECT", JELLO_T_OBJECT},
    {"DYNAMIC", JELLO_T_DYNAMIC}, {"FUNCTION", JELLO_T_FUNCTION}, {"ABSTRACT", JELLO_T_ABSTRACT},
    {"ENUM", JELLO_T_ENUM},
};
#define NKIND_NAMES (sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]))

typedef struct flag_name_entry {
  const char* name;
  jello_bc_feature_flags bit;
  int supported; /**< Whether `asm` can write the flag's section, and so accepts the flag. */
} flag_name_entry;

static const flag_name_entry FLAG_NAMES[] = {
    {"CONST64", JELLO_BC_FEAT_CONST64, 1},
    {"CONSTBYTES", JELLO_BC_FEAT_CONSTBYTES, 1},
    {"CAP_START", JELLO_BC_FEAT_CAP_START, 1},
    {"LINE_TABLE", JELLO_BC_FEAT_LINE_TABLE, 1},
    {"LINE_COL", JELLO_BC_FEAT_LINE_COL, 0},
    {"LINE_DELTA", JELLO_BC_FEAT_LINE_DELTA, 0},
    {"COMPRESSED_INSN", JELLO_BC_FEAT_COMPRESSED_INSN, 1},
    {"LOOP_ANCHOR", JELLO_BC_FEAT_LOOP_ANCHOR, 0},
    {"TRAP_MAP", JELLO_BC_FEAT_TRAP_MAP, 0},
    {"HANDLER_MAP", JELLO_BC_FEAT_HANDLER_MAP, 0},
    {"TYPE_PROFILE", JELLO_BC_FEAT_TYPE_PROFILE, 0},
    {"STACK_MAP", JELLO_BC_FEAT_STACK_MAP, 0},
    {"PKG_DEPS", JELLO_BC_FEAT_PKG_DEPS, 0},
    {"IMPORTS", JELLO_BC_FEAT_IMPORTS, 1},
    {"ENUM_SCHEMA", JELLO_BC_FEAT_ENUM_SCHEMA, 1},
    {"GLOBALS", JELLO_BC_FEAT_GLOBALS, 1},
};
#define NFLAG_NAMES (sizeof(FLAG_NAMES) / sizeof(FLAG_NAMES[0]))

static const char* kind_name_of(jello_type_kind k) {
  for(size_t i = 0; i < NKIND_NAMES; i++)
    if(KIND_NAMES[i].kind == k) return KIND_NAMES[i].name;
  return "?";
}

/** Prints type `tid` as the assembler reads it back; its kind name, with #`tid` unless it is the
 * first type of its kind. */
static void print_type_ref(FILE* out, const jello_bc_module* m, jello_type_id tid) {
  jello_type_kind kind = m->types[tid].kind;
  fprintf(out, "%s", kind_name_of(kind));
  for(uint32_t i = 0; i < tid; i++) {
    if(m->types[i].kind == kind) {
      fprintf(out, "#%u", tid);
      return;
    }
  }
}
static int kind_from_name(const char* name, jello_type_kind* out) {
  for(size_t i = 0; i < NKIND_NAMES; i++) {
    if(strcmp(KIND_NAMES[i].name, name) == 0) {
      *out = KIND_NAMES[i].kind;
      return 1;
    }
  }
  return 0;
}
// Opcode names come from the VM's jello_op_table in both directions.
static const char* op_name_of(jello_op op) {
  if((uint32_t)op >= (uint32_t)JOP_COUNT) return NULL;
  return jello_op_table[op].name;
}
static int op_from_name(const char* name, jello_op* out) {
  for(size_t i = 0; i < (size_t)JOP_COUNT; i++) {
    if(strcmp(jello_op_table[i].name, name) == 0) {
      *out = (jello_op)i;
      return 1;
    }
  }
  return 0;
}
static const flag_name_entry* flag_entry_from_name(const char* name) {
  for(size_t i = 0; i < NFLAG_NAMES; i++)
    if(strcmp(FLAG_NAMES[i].name, name) == 0) return &FLAG_NAMES[i];
  return NULL;
}
typedef struct asm_type_entry {
  jello_type_kind kind;
  /** LIST, ARRAY: element type. STRUCT, FUNCTION, ENUM: index of its sig. ABSTRACT: its id. */
  uint32_t p0;
} asm_type_entry;

typedef struct asm_fun_sig {
  uint32_t ret;
  uint16_t nargs;
  uint32_t* args;
} asm_fun_sig;

typedef struct asm_struct_sig {
  uint16_t nfields;
  uint8_t named;
  uint8_t is_mutable;
  uint32_t* field_type;      /**< Type-table indexes. */
  uint32_t* field_name_atom; /**< Atom indexes; only when `named`. */
} asm_struct_sig;

typedef struct asm_enum_variant_sig {
  uint16_t nfields;
  uint32_t* field_type;
} asm_enum_variant_sig;

typedef struct asm_enum_sig {
  uint16_t nvariants;
  asm_enum_variant_sig* variants;
} asm_enum_sig;

typedef struct asm_import_sig {
  uint32_t library_atom;
  uint32_t export_atom;
  jello_type_kind ret_kind;
  uint8_t arity;
  jello_type_kind arg_kinds[8];
} asm_import_sig;

typedef struct func_t {
  uint32_t name_atom; /**< JELLO_BC_NO_NAME when the text gives no name. */
  uint32_t nregs;
  uint32_t cap_start;
  uint32_t nargs; /**< nregs when the text gives none, as is cap_start. */
  uint32_t* reg_type_idx;
  uint32_t ninsns;
  jello_insn* insns;
} func_t;

typedef struct module_t {
  uint32_t version;
  uint32_t features;
  uint32_t entry;
  uint32_t ntypes;
  asm_type_entry* types;
  uint32_t nsigs;
  asm_fun_sig* sigs;
  uint32_t natoms;
  char** atoms;
  uint32_t nstruct_sigs;
  asm_struct_sig* struct_sigs;
  uint32_t nenum_sigs;
  asm_enum_sig* enum_sigs;
  // The CONST_I64 and CONST_F64 pools, in the text and the binary only with CONST64.
  uint32_t nconst_i64;
  int64_t* const_i64;
  uint32_t nconst_f64;
  double* const_f64;
  uint32_t nimports;
  asm_import_sig* imports;
  uint32_t nglobals;
  uint32_t* global_types;
  /* LINE_TABLE; source paths, and runs of five numbers (function, pc, file, line, col),
   * written per function, in text order, after the functions. */
  uint32_t nsource_files;
  char** source_files;
  uint32_t nline_runs;
  uint32_t* line_runs;
  uint32_t nfuncs;
  func_t* funcs;
} module_t;

static void module_free(module_t* m) {
  if(!m) return;
  free(m->types);
  for(uint32_t i = 0; i < m->nsigs; i++) free(m->sigs[i].args);
  free(m->sigs);
  for(uint32_t i = 0; i < m->natoms; i++) free(m->atoms[i]);
  free(m->atoms);
  for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
    free(m->struct_sigs[i].field_type);
    free(m->struct_sigs[i].field_name_atom);
  }
  free(m->struct_sigs);
  for(uint32_t i = 0; i < m->nenum_sigs; i++) {
    for(uint16_t j = 0; j < m->enum_sigs[i].nvariants; j++)
      free(m->enum_sigs[i].variants[j].field_type);
    free(m->enum_sigs[i].variants);
  }
  free(m->enum_sigs);
  free(m->const_i64);
  free(m->const_f64);
  free(m->imports);
  free(m->global_types);
  for(uint32_t i = 0; i < m->nsource_files; i++) free(m->source_files[i]);
  free(m->source_files);
  free(m->line_runs);
  for(uint32_t i = 0; i < m->nfuncs; i++) {
    free(m->funcs[i].reg_type_idx);
    free(m->funcs[i].insns);
  }
  free(m->funcs);
  memset(m, 0, sizeof(*m));
}

/* The text form's reader. Whitespace separates tokens, but a comment is a line starting with '#'
 * and a PC label is digits directly followed by ':'. Parsing stops at the first error, reported
 * on stderr. */

typedef struct cursor {
  const char* buf;
  size_t len;
  size_t pos;
  const char* filename;
} cursor;

/** Skips whitespace and comments; a line whose first character is '#' (a '#' anywhere else is
 * part of KIND#N). Disassembly writes no comments, so a fixture with them does not round-trip
 * byte for byte. */
static void skip_ws(cursor* c) {
  while(c->pos < c->len) {
    char ch = c->buf[c->pos];
    if(ch == '#' && (c->pos == 0 || c->buf[c->pos - 1] == '\n')) {
      while(c->pos < c->len && c->buf[c->pos] != '\n') c->pos++;
      continue;
    }
    if(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      c->pos++;
      continue;
    }
    break;
  }
}

static int perr(cursor* c, const char* msg) {
  size_t line = 1, col = 1;
  for(size_t i = 0; i < c->pos && i < c->len; i++) {
    if(c->buf[i] == '\n') {
      line++;
      col = 1;
    } else {
      col++;
    }
  }
  fprintf(stderr, "%s:%zu:%zu: parse error: %s\n", c->filename, line, col, msg);
  return 0;
}

static int match_ch(cursor* c, char ch) {
  skip_ws(c);
  if(c->pos < c->len && c->buf[c->pos] == ch) {
    c->pos++;
    return 1;
  }
  return 0;
}

static int expect_ch(cursor* c, char ch) {
  if(match_ch(c, ch)) return 1;
  char msg[64];
  snprintf(msg, sizeof(msg), "expected '%c'", ch);
  return perr(c, msg);
}

static int read_ident(cursor* c, char* out, size_t outsz) {
  skip_ws(c);
  size_t start = c->pos;
  while(c->pos < c->len) {
    char ch = c->buf[c->pos];
    if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
       ch == '_') {
      c->pos++;
    } else break;
  }
  size_t n = c->pos - start;
  if(n == 0 || n >= outsz) return perr(c, "expected identifier");
  memcpy(out, c->buf + start, n);
  out[n] = '\0';
  return 1;
}

static int match_ident(cursor* c, const char* literal) {
  size_t save = c->pos;
  char buf[64];
  if(!read_ident(c, buf, sizeof(buf))) {
    c->pos = save;
    return 0;
  }
  if(strcmp(buf, literal) != 0) {
    c->pos = save;
    return 0;
  }
  return 1;
}

static int expect_ident(cursor* c, const char* literal) {
  if(match_ident(c, literal)) return 1;
  char msg[80];
  snprintf(msg, sizeof(msg), "expected '%s'", literal);
  return perr(c, msg);
}

static int read_uint(cursor* c, uint32_t* out) {
  skip_ws(c);
  size_t start = c->pos;
  while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  if(c->pos == start) return perr(c, "expected an unsigned integer");
  char buf[16];
  size_t n = c->pos - start;
  if(n >= sizeof(buf)) return perr(c, "integer literal too long");
  memcpy(buf, c->buf + start, n);
  buf[n] = '\0';
  *out = (uint32_t)strtoul(buf, NULL, 10);
  return 1;
}

static int read_int(cursor* c, int32_t* out) {
  skip_ws(c);
  int neg = 0;
  if(c->pos < c->len && c->buf[c->pos] == '-') {
    neg = 1;
    c->pos++;
  }
  uint32_t mag = 0;
  if(!read_uint(c, &mag)) return 0;
  // Negating unsigned keeps -2147483648, whose magnitude exceeds INT32_MAX, defined.
  uint32_t bits = neg ? (0u - mag) : mag;
  memcpy(out, &bits, sizeof bits);
  return 1;
}

// Literals for `const_i64:` and `const_f64:`, the only ones wider than `imm`.
static int read_int64(cursor* c, int64_t* out) {
  skip_ws(c);
  size_t start = c->pos;
  if(c->pos < c->len && c->buf[c->pos] == '-') c->pos++;
  size_t digits_start = c->pos;
  while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  if(c->pos == digits_start) return perr(c, "expected a 64-bit integer");
  char buf[32];
  size_t n = c->pos - start;
  if(n >= sizeof(buf)) return perr(c, "integer literal too long");
  memcpy(buf, c->buf + start, n);
  buf[n] = '\0';
  *out = (int64_t)strtoll(buf, NULL, 10);
  return 1;
}

static int read_double(cursor* c, double* out) {
  skip_ws(c);
  size_t start = c->pos;
  if(c->pos < c->len && (c->buf[c->pos] == '-' || c->buf[c->pos] == '+')) c->pos++;
  size_t digits_start = c->pos;
  while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  if(c->pos < c->len && c->buf[c->pos] == '.') {
    c->pos++;
    while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  }
  if(c->pos < c->len && (c->buf[c->pos] == 'e' || c->buf[c->pos] == 'E')) {
    size_t save = c->pos;
    c->pos++;
    if(c->pos < c->len && (c->buf[c->pos] == '-' || c->buf[c->pos] == '+')) c->pos++;
    size_t exp_digits = c->pos;
    while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
    if(c->pos == exp_digits) c->pos = save;
  }
  if(c->pos == digits_start) return perr(c, "expected a floating-point literal");
  char buf[64];
  size_t n = c->pos - start;
  if(n >= sizeof(buf)) return perr(c, "float literal too long");
  memcpy(buf, c->buf + start, n);
  buf[n] = '\0';
  *out = strtod(buf, NULL);
  return 1;
}

static int parse_features(cursor* c, uint32_t* out_features) {
  uint32_t features = 0;
  if(!expect_ch(c, '[')) return 0;
  skip_ws(c);
  if(match_ch(c, ']')) {
    *out_features = 0;
    return 1;
  }
  for(;;) {
    char name[32];
    if(!read_ident(c, name, sizeof(name))) return 0;
    const flag_name_entry* fe = flag_entry_from_name(name);
    if(!fe) {
      fprintf(stderr, "%s: unknown feature flag '%s'\n", c->filename, name);
      return 0;
    }
    if(!fe->supported) {
      fprintf(stderr, "%s: feature flag '%s' has no section producer/consumer in this tool yet\n",
              c->filename, name);
      return 0;
    }
    features |= (uint32_t)fe->bit;
    if(match_ch(c, ',')) continue;
    break;
  }
  if(!expect_ch(c, ']')) return 0;
  *out_features = features;
  return 1;
}

static int parse_type_entry(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                            asm_type_entry* out) {
  char name[32];
  if(!read_ident(c, name, sizeof(name))) return 0;
  jello_type_kind kind;
  if(!kind_from_name(name, &kind)) {
    fprintf(stderr, "%s: unknown or unsupported type kind '%s'\n", c->filename, name);
    return 0;
  }
  out->kind = kind;
  out->p0 = 0;
  if(kind == JELLO_T_LIST || kind == JELLO_T_ARRAY) {
    char elem[32];
    jello_type_kind elem_kind;
    if(!match_ch(c, '(')) return 1;
    if(!read_ident(c, elem, sizeof(elem))) return 0;
    if(!kind_from_name(elem, &elem_kind)) return perr(c, "unknown element type");
    if(match_ch(c, '#')) {
      uint32_t n;
      if(!read_uint(c, &n)) return 0;
      if(n >= ntypes || types[n].kind != elem_kind)
        return perr(c, "element type must be declared first");
      out->p0 = n;
      return expect_ch(c, ')');
    }
    if(!expect_ch(c, ')')) return 0;
    for(uint32_t i = 0; i < ntypes; i++) {
      if(types[i].kind == elem_kind) {
        out->p0 = i;
        return 1;
      }
    }
    return perr(c, "element type must be declared first");
  }
  if(kind == JELLO_T_STRUCT || kind == JELLO_T_FUNCTION || kind == JELLO_T_ABSTRACT ||
     kind == JELLO_T_ENUM) {
    if(kind == JELLO_T_ENUM && !match_ch(c, '(')) return 1;
    if(kind != JELLO_T_ENUM && !expect_ch(c, '(')) return 0;
    if(!read_uint(c, &out->p0)) return 0;
    if(!expect_ch(c, ')')) return 0;
  }
  return 1;
}

static int parse_types(cursor* c, asm_type_entry** out_types, uint32_t* out_n) {
  if(!expect_ident(c, "types")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  asm_type_entry* types = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      asm_type_entry ent;
      if(!parse_type_entry(c, types, n, &ent)) {
        free(types);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        types = (asm_type_entry*)realloc(types, cap * sizeof(asm_type_entry));
      }
      types[n++] = ent;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(types);
      return 0;
    }
  }
  *out_types = types;
  *out_n = n;
  return 1;
}

static int parse_atoms(cursor* c, char*** out_atoms, uint32_t* out_n) {
  if(!expect_ident(c, "atoms")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  char** atoms = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[64];
      if(!read_ident(c, name, sizeof(name))) {
        for(uint32_t i = 0; i < n; i++) free(atoms[i]);
        free(atoms);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        atoms = (char**)realloc(atoms, cap * sizeof(char*));
      }
      atoms[n] = (char*)malloc(strlen(name) + 1);
      strcpy(atoms[n], name);
      n++;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      for(uint32_t i = 0; i < n; i++) free(atoms[i]);
      free(atoms);
      return 0;
    }
  }
  *out_atoms = atoms;
  *out_n = n;
  return 1;
}

static int parse_const_i64(cursor* c, int64_t** out_vals, uint32_t* out_n) {
  if(!expect_ident(c, "const_i64")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  int64_t* vals = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      int64_t v = 0;
      if(!read_int64(c, &v)) {
        free(vals);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        vals = (int64_t*)realloc(vals, cap * sizeof(int64_t));
      }
      vals[n++] = v;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(vals);
      return 0;
    }
  }
  *out_vals = vals;
  *out_n = n;
  return 1;
}

static int parse_const_f64(cursor* c, double** out_vals, uint32_t* out_n) {
  if(!expect_ident(c, "const_f64")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  double* vals = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      double v;
      if(!read_double(c, &v)) {
        free(vals);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        vals = (double*)realloc(vals, cap * sizeof(double));
      }
      vals[n++] = v;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(vals);
      return 0;
    }
  }
  *out_vals = vals;
  *out_n = n;
  return 1;
}

static int find_type_index_by_kind_name(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                                        const char* name, uint32_t* out_idx) {
  jello_type_kind want;
  if(!kind_from_name(name, &want)) {
    fprintf(stderr, "%s: unknown type kind '%s'\n", c->filename, name);
    return 0;
  }
  /* KIND#N names type N, which must have that kind; a bare KIND names the first type of the
   * kind. */
  if(c->pos < c->len && c->buf[c->pos] == '#') {
    uint32_t n;
    c->pos++;
    if(!read_uint(c, &n)) return 0;
    if(n >= ntypes || types[n].kind != want)
      return perr(c, "KIND#N names a type of another kind or past the declared ones");
    *out_idx = n;
    return 1;
  }
  for(uint32_t i = 0; i < ntypes; i++) {
    if(types[i].kind == want) {
      *out_idx = i;
      return 1;
    }
  }
  fprintf(stderr, "%s: type kind '%s' used but not declared in 'types:'\n", c->filename, name);
  return 0;
}

static int parse_fun_sig_entry(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                               asm_fun_sig* out) {
  memset(out, 0, sizeof(*out));
  if(!expect_ch(c, '{')) return 0;
  if(!expect_ident(c, "ret")) return 0;
  if(!expect_ch(c, '=')) return 0;
  {
    char name[32];
    if(!read_ident(c, name, sizeof(name))) return 0;
    if(!find_type_index_by_kind_name(c, types, ntypes, name, &out->ret)) return 0;
  }
  if(!expect_ch(c, ',')) return 0;
  if(!expect_ident(c, "nargs")) return 0;
  if(!expect_ch(c, '=')) return 0;
  {
    uint32_t nargs = 0;
    if(!read_uint(c, &nargs)) return 0;
    out->nargs = (uint16_t)nargs;
  }
  if(!expect_ch(c, ',')) return 0;
  if(!expect_ident(c, "args")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!expect_ch(c, '[')) return 0;
  uint32_t* args = NULL;
  uint32_t got = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[32];
      uint32_t ti;
      if(!read_ident(c, name, sizeof(name))) {
        free(args);
        return 0;
      }
      if(!find_type_index_by_kind_name(c, types, ntypes, name, &ti)) {
        free(args);
        return 0;
      }
      args = (uint32_t*)realloc(args, (got + 1) * sizeof(uint32_t));
      args[got++] = ti;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(args);
      return 0;
    }
  }
  if(got != (uint32_t)out->nargs) {
    fprintf(stderr, "%s: sig nargs=%u but args has %u entries\n", c->filename, (unsigned)out->nargs,
            got);
    free(args);
    return 0;
  }
  out->args = args;
  if(!expect_ch(c, '}')) {
    free(args);
    return 0;
  }
  return 1;
}

static int parse_sigs(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                      asm_fun_sig** out_sigs, uint32_t* out_n) {
  if(!expect_ident(c, "sigs")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  asm_fun_sig* sigs = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      asm_fun_sig ent;
      if(!parse_fun_sig_entry(c, types, ntypes, &ent)) {
        for(uint32_t i = 0; i < n; i++) free(sigs[i].args);
        free(sigs);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        sigs = (asm_fun_sig*)realloc(sigs, cap * sizeof(asm_fun_sig));
      }
      sigs[n++] = ent;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      for(uint32_t i = 0; i < n; i++) free(sigs[i].args);
      free(sigs);
      return 0;
    }
  }
  *out_sigs = sigs;
  *out_n = n;
  return 1;
}

static int find_atom_index_by_name(cursor* c, char* const* atoms, uint32_t natoms, const char* name,
                                   uint32_t* out_idx) {
  for(uint32_t i = 0; i < natoms; i++) {
    if(strcmp(atoms[i], name) == 0) {
      *out_idx = i;
      return 1;
    }
  }
  fprintf(stderr, "%s: atom '%s' used but not declared in 'atoms:'\n", c->filename, name);
  return 0;
}

static int parse_struct_sig_entry(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                                  char* const* atoms, uint32_t natoms, asm_struct_sig* out) {
  memset(out, 0, sizeof(*out));
  if(!expect_ch(c, '{')) return 0;
  if(!expect_ident(c, "nfields")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t nfields;
  if(!read_uint(c, &nfields)) return 0;
  if(!expect_ident(c, "named")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t named;
  if(!read_uint(c, &named)) return 0;
  if(!expect_ident(c, "mutable")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t is_mutable;
  if(!read_uint(c, &is_mutable)) return 0;
  if(!expect_ident(c, "fields")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!expect_ch(c, '[')) return 0;

  out->nfields = (uint16_t)nfields;
  out->named = (uint8_t)(named ? 1 : 0);
  out->is_mutable = (uint8_t)(is_mutable ? 1 : 0);
  uint32_t* field_type = nfields ? (uint32_t*)calloc(nfields, sizeof(uint32_t)) : NULL;
  uint32_t got = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[32];
      if(!read_ident(c, name, sizeof(name))) {
        free(field_type);
        return 0;
      }
      uint32_t ti;
      if(!find_type_index_by_kind_name(c, types, ntypes, name, &ti)) {
        free(field_type);
        return 0;
      }
      if(got >= nfields) {
        fprintf(stderr, "%s: struct_sig has more fields than nfields\n", c->filename);
        free(field_type);
        return 0;
      }
      field_type[got++] = ti;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(field_type);
      return 0;
    }
  }
  if(got != nfields) {
    fprintf(stderr, "%s: struct_sig declares nfields=%u but fields=[...] has %u entries\n",
            c->filename, nfields, got);
    free(field_type);
    return 0;
  }
  out->field_type = field_type;

  if(out->named) {
    /* out->field_type already holds field_type, so a failure here frees both arrays and clears it.
     */
    uint32_t* field_name_atom = NULL;
    int names_ok = expect_ident(c, "names") && expect_ch(c, '=') && expect_ch(c, '[');
    if(names_ok) {
      field_name_atom = nfields ? (uint32_t*)calloc(nfields, sizeof(uint32_t)) : NULL;
      uint32_t ngot = 0;
      skip_ws(c);
      if(!match_ch(c, ']')) {
        for(;;) {
          char name[64];
          uint32_t ai;
          if(!read_ident(c, name, sizeof(name)) ||
             !find_atom_index_by_name(c, atoms, natoms, name, &ai)) {
            names_ok = 0;
            break;
          }
          if(ngot >= nfields) {
            fprintf(stderr, "%s: struct_sig has more names than nfields\n", c->filename);
            names_ok = 0;
            break;
          }
          field_name_atom[ngot++] = ai;
          if(match_ch(c, ',')) continue;
          break;
        }
        if(names_ok && !expect_ch(c, ']')) names_ok = 0;
      }
    }
    if(!names_ok) {
      free(field_name_atom);
      free(field_type);
      out->field_type = NULL;
      return 0;
    }
    out->field_name_atom = field_name_atom;
  }

  if(!expect_ch(c, '}')) return 0;
  return 1;
}

static int parse_struct_sigs(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                             char* const* atoms, uint32_t natoms, asm_struct_sig** out_sigs,
                             uint32_t* out_n) {
  if(!expect_ident(c, "struct_sigs")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  asm_struct_sig* sigs = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      asm_struct_sig sig;
      if(!parse_struct_sig_entry(c, types, ntypes, atoms, natoms, &sig)) {
        free(sigs);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        sigs = (asm_struct_sig*)realloc(sigs, cap * sizeof(asm_struct_sig));
      }
      sigs[n++] = sig;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(sigs);
      return 0;
    }
  }
  *out_sigs = sigs;
  *out_n = n;
  return 1;
}

static int parse_enum_sigs(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                           asm_enum_sig** out_sigs, uint32_t* out_n) {
  if(!expect_ident(c, "enum_sigs") || !expect_ch(c, ':') || !expect_ch(c, '[')) return 0;
  asm_enum_sig* sigs = NULL;
  uint32_t n = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      if(!expect_ch(c, '[')) goto fail;
      asm_enum_sig sig = {0};
      skip_ws(c);
      if(!match_ch(c, ']')) {
        for(;;) {
          if(!expect_ch(c, '[')) goto fail_entry;
          asm_enum_variant_sig variant = {0};
          uint32_t count = 0;
          skip_ws(c);
          if(!match_ch(c, ']')) {
            for(;;) {
              char name[32];
              uint32_t tid;
              if(!read_ident(c, name, sizeof(name)) ||
                 !find_type_index_by_kind_name(c, types, ntypes, name, &tid))
                goto fail_variant;
              variant.field_type =
                  (uint32_t*)realloc(variant.field_type, (count + 1u) * sizeof(uint32_t));
              variant.field_type[count++] = tid;
              if(match_ch(c, ',')) continue;
              if(!expect_ch(c, ']')) goto fail_variant;
              break;
            }
          }
          if(count > UINT16_MAX) goto fail_variant;
          variant.nfields = (uint16_t)count;
          sig.variants = (asm_enum_variant_sig*)realloc(sig.variants, ((size_t)sig.nvariants + 1u) *
                                                                          sizeof(*sig.variants));
          sig.variants[sig.nvariants++] = variant;
          if(match_ch(c, ',')) continue;
          if(!expect_ch(c, ']')) goto fail_entry;
          break;
        fail_variant:
          free(variant.field_type);
          goto fail_entry;
        }
      }
      sigs = (asm_enum_sig*)realloc(sigs, (n + 1u) * sizeof(*sigs));
      sigs[n++] = sig;
      if(match_ch(c, ',')) continue;
      if(!expect_ch(c, ']')) goto fail;
      break;
    fail_entry:
      for(uint16_t j = 0; j < sig.nvariants; j++) free(sig.variants[j].field_type);
      free(sig.variants);
      goto fail;
    }
  }
  *out_sigs = sigs;
  *out_n = n;
  return 1;
fail:
  for(uint32_t i = 0; i < n; i++) {
    for(uint16_t j = 0; j < sigs[i].nvariants; j++) free(sigs[i].variants[j].field_type);
    free(sigs[i].variants);
  }
  free(sigs);
  return 0;
}

/** Parses one `{ library=ATOM, export=ATOM, ret=KIND, args=[KIND, ...] }` of `imports:`, which
 * the text has only with IMPORTS. */
static int parse_import_entry(cursor* c, char* const* atoms, uint32_t natoms, asm_import_sig* out) {
  memset(out, 0, sizeof(*out));
  if(!expect_ch(c, '{')) return 0;
  if(!expect_ident(c, "library")) return 0;
  if(!expect_ch(c, '=')) return 0;
  char libname[64];
  if(!read_ident(c, libname, sizeof(libname))) return 0;
  if(!find_atom_index_by_name(c, atoms, natoms, libname, &out->library_atom)) return 0;
  match_ch(c, ',');

  if(!expect_ident(c, "export")) return 0;
  if(!expect_ch(c, '=')) return 0;
  char expname[64];
  if(!read_ident(c, expname, sizeof(expname))) return 0;
  if(!find_atom_index_by_name(c, atoms, natoms, expname, &out->export_atom)) return 0;
  match_ch(c, ',');

  if(!expect_ident(c, "ret")) return 0;
  if(!expect_ch(c, '=')) return 0;
  char retname[32];
  if(!read_ident(c, retname, sizeof(retname))) return 0;
  if(!kind_from_name(retname, &out->ret_kind)) {
    fprintf(stderr, "%s: unknown ret kind '%s' in import\n", c->filename, retname);
    return 0;
  }
  match_ch(c, ',');

  if(!expect_ident(c, "args")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!expect_ch(c, '[')) return 0;
  uint32_t got = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[32];
      if(!read_ident(c, name, sizeof(name))) return 0;
      jello_type_kind ak;
      if(!kind_from_name(name, &ak)) {
        fprintf(stderr, "%s: unknown arg kind '%s' in import\n", c->filename, name);
        return 0;
      }
      if(got >= 8) {
        fprintf(stderr, "%s: import arity exceeds 8 (v1 ceiling)\n", c->filename);
        return 0;
      }
      out->arg_kinds[got++] = ak;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) return 0;
  }
  out->arity = (uint8_t)got;
  if(!expect_ch(c, '}')) return 0;
  return 1;
}

static int parse_imports(cursor* c, char* const* atoms, uint32_t natoms,
                         asm_import_sig** out_imports, uint32_t* out_n) {
  if(!expect_ident(c, "imports")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  asm_import_sig* imports = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      asm_import_sig imp;
      if(!parse_import_entry(c, atoms, natoms, &imp)) {
        free(imports);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        imports = (asm_import_sig*)realloc(imports, cap * sizeof(asm_import_sig));
      }
      imports[n++] = imp;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(imports);
      return 0;
    }
  }
  *out_imports = imports;
  *out_n = n;
  return 1;
}

/** Parses LINE_TABLE's `source_files: [path, ...]`, each path a run of characters other than
 * spaces, newlines, ',' and ']', then `lines: [f pc file line col, ...]`. Nothing is checked
 * here, so a fixture can spell a malformed table. */
static int parse_line_table(cursor* c, module_t* m) {
  if(!expect_ident(c, "source_files") || !expect_ch(c, ':') || !expect_ch(c, '[')) return 0;
  uint32_t cap = 0;
  if(!match_ch(c, ']')) {
    for(;;) {
      skip_ws(c);
      size_t start = c->pos;
      while(c->pos < c->len && c->buf[c->pos] != ',' && c->buf[c->pos] != ']' &&
            c->buf[c->pos] != ' ' && c->buf[c->pos] != '\n')
        c->pos++;
      if(c->pos == start) return perr(c, "expected a source path");
      if(m->nsource_files == cap) {
        cap = cap ? cap * 2 : 4;
        m->source_files = (char**)realloc(m->source_files, cap * sizeof(char*));
      }
      char* path = (char*)malloc(c->pos - start + 1);
      memcpy(path, c->buf + start, c->pos - start);
      path[c->pos - start] = '\0';
      m->source_files[m->nsource_files++] = path;
      if(match_ch(c, ',')) continue;
      if(!expect_ch(c, ']')) return 0;
      break;
    }
  }
  if(!expect_ident(c, "lines") || !expect_ch(c, ':') || !expect_ch(c, '[')) return 0;
  cap = 0;
  if(!match_ch(c, ']')) {
    for(;;) {
      if(m->nline_runs == cap) {
        cap = cap ? cap * 2 : 4;
        m->line_runs = (uint32_t*)realloc(m->line_runs, cap * 5 * sizeof(uint32_t));
      }
      for(int k = 0; k < 5; k++)
        if(!read_uint(c, &m->line_runs[m->nline_runs * 5 + (uint32_t)k])) return 0;
      m->nline_runs++;
      if(match_ch(c, ',')) continue;
      if(!expect_ch(c, ']')) return 0;
      break;
    }
  }
  return 1;
}

/** Parses `label sep [KIND, ...]` into type-table indexes, as reg_types= and globals: spell them.
 */
static int parse_type_list(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                           const char* label, char sep, uint32_t** out_idx, uint32_t* out_n) {
  if(!expect_ident(c, label)) return 0;
  if(!expect_ch(c, sep)) return 0;
  if(!expect_ch(c, '[')) return 0;
  uint32_t* idx = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[32];
      if(!read_ident(c, name, sizeof(name))) {
        free(idx);
        return 0;
      }
      uint32_t ti;
      if(!find_type_index_by_kind_name(c, types, ntypes, name, &ti)) {
        free(idx);
        return 0;
      }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        idx = (uint32_t*)realloc(idx, cap * sizeof(uint32_t));
      }
      idx[n++] = ti;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      free(idx);
      return 0;
    }
  }
  *out_idx = idx;
  *out_n = n;
  return 1;
}

static int parse_reg_operand(cursor* c, uint8_t* out) {
  if(!expect_ch(c, '=')) return 0;
  if(!match_ch(c, 'r')) return perr(c, "expected register operand 'rN'");
  uint32_t v;
  if(!read_uint(c, &v)) return 0;
  if(v > 255) return perr(c, "register index exceeds 255, the bytecode's limit");
  *out = (uint8_t)v;
  return 1;
}

static int parse_insn(cursor* c, uint32_t expect_pc, jello_insn* out) {
  uint32_t pc_label;
  if(!read_uint(c, &pc_label)) return 0;
  if(pc_label != expect_pc) {
    fprintf(stderr,
            "%s: PC label %u out of sequence (expected %u, labels must be sequential from 0)\n",
            c->filename, pc_label, expect_pc);
    return 0;
  }
  if(!expect_ch(c, ':')) return 0;
  char opname[32];
  if(!read_ident(c, opname, sizeof(opname))) return 0;
  jello_op op;
  if(!op_from_name(opname, &op)) {
    fprintf(stderr, "%s: unknown opcode '%s'\n", c->filename, opname);
    return 0;
  }
  if(!expect_ident(c, "a")) return 0;
  uint8_t a;
  if(!parse_reg_operand(c, &a)) return 0;
  if(!expect_ident(c, "b")) return 0;
  uint8_t b;
  if(!parse_reg_operand(c, &b)) return 0;
  if(!expect_ident(c, "c")) return 0;
  uint8_t cc;
  if(!parse_reg_operand(c, &cc)) return 0;
  if(!expect_ident(c, "imm")) return 0;
  if(!expect_ch(c, '=')) return 0;
  int32_t imm;
  if(!read_int(c, &imm)) return 0;
  out->op = (uint8_t)op;
  out->a = a;
  out->b = b;
  out->c = cc;
  out->imm = (uint32_t)imm;
  return 1;
}

static int parse_func(cursor* c, const asm_type_entry* types, uint32_t ntypes, char* const* atoms,
                      uint32_t natoms, uint32_t* out_index, func_t* out_fn) {
  if(!expect_ident(c, "func")) return 0;
  if(!expect_ch(c, '[')) return 0;
  uint32_t idx;
  if(!read_uint(c, &idx)) return 0;
  if(!expect_ch(c, ']')) return 0;

  // Optional, like cap_start and nargs; name=ATOM, an atom spelt by name.
  uint32_t name_atom = JELLO_BC_NO_NAME;
  size_t name_save = c->pos;
  if(match_ident(c, "name")) {
    if(!expect_ch(c, '=')) return 0;
    char name[64];
    if(!read_ident(c, name, sizeof(name))) return 0;
    if(!find_atom_index_by_name(c, atoms, natoms, name, &name_atom)) return 0;
  } else {
    c->pos = name_save;
  }

  if(!expect_ident(c, "nregs")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t nregs;
  if(!read_uint(c, &nregs)) return 0;
  if(nregs > 256) return perr(c, "nregs exceeds the frozen 256-register ceiling");

  uint32_t cap_start = nregs; // Default: no captures.
  size_t save = c->pos;
  if(match_ident(c, "cap_start")) {
    if(!expect_ch(c, '=')) return 0;
    if(!read_uint(c, &cap_start)) return 0;
  } else {
    c->pos = save;
  }

  uint32_t nargs = nregs;
  save = c->pos;
  if(match_ident(c, "nargs")) {
    if(!expect_ch(c, '=')) return 0;
    if(!read_uint(c, &nargs)) return 0;
    if(nargs > nregs) return perr(c, "nargs exceeds nregs");
  } else {
    c->pos = save;
  }

  uint32_t* reg_type_idx = NULL;
  uint32_t nrt = 0;
  if(!parse_type_list(c, types, ntypes, "reg_types", '=', &reg_type_idx, &nrt)) return 0;
  if(nrt != nregs) {
    fprintf(stderr, "%s: reg_types has %u entries but nregs=%u\n", c->filename, nrt, nregs);
    free(reg_type_idx);
    return 0;
  }

  jello_insn* insns = NULL;
  uint32_t ninsns = 0, cap = 0;
  for(;;) {
    size_t before = c->pos;
    skip_ws(c);
    if(c->pos >= c->len) break;
    // An instruction starts with a decimal PC label and ':'; the next func[ does not.
    size_t probe = c->pos;
    while(probe < c->len && c->buf[probe] >= '0' && c->buf[probe] <= '9') probe++;
    if(probe == c->pos || probe >= c->len || c->buf[probe] != ':') {
      c->pos = before;
      break;
    }

    jello_insn insn;
    if(!parse_insn(c, ninsns, &insn)) {
      free(reg_type_idx);
      free(insns);
      return 0;
    }
    if(ninsns == cap) {
      cap = cap ? cap * 2 : 8;
      insns = (jello_insn*)realloc(insns, cap * sizeof(jello_insn));
    }
    insns[ninsns++] = insn;
  }

  out_fn->name_atom = name_atom;
  out_fn->nregs = nregs;
  out_fn->cap_start = cap_start;
  out_fn->nargs = nargs;
  out_fn->reg_type_idx = reg_type_idx;
  out_fn->ninsns = ninsns;
  out_fn->insns = insns;
  *out_index = idx;
  return 1;
}

static int parse_module(cursor* c, module_t* out) {
  memset(out, 0, sizeof(*out));
  if(!expect_ch(c, '.')) return 0;
  if(!expect_ident(c, "jlo")) return 0;
  if(!expect_ident(c, "module")) return 0;
  if(!expect_ident(c, "version")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!read_uint(c, &out->version)) return 0;
  if(!expect_ident(c, "features")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!parse_features(c, &out->features)) return 0;

  out->entry = 0;
  size_t save = c->pos;
  if(match_ident(c, "entry")) {
    if(!expect_ch(c, '=')) return 0;
    if(!read_uint(c, &out->entry)) return 0;
  } else {
    c->pos = save;
  }

  if(!parse_types(c, &out->types, &out->ntypes)) return 0;
  if(!parse_sigs(c, out->types, out->ntypes, &out->sigs, &out->nsigs)) {
    module_free(out);
    return 0;
  }
  if(!parse_atoms(c, &out->atoms, &out->natoms)) {
    module_free(out);
    return 0;
  }
  if(!parse_struct_sigs(c, out->types, out->ntypes, out->atoms, out->natoms, &out->struct_sigs,
                        &out->nstruct_sigs)) {
    module_free(out);
    return 0;
  }
  if(out->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA) {
    if(!parse_enum_sigs(c, out->types, out->ntypes, &out->enum_sigs, &out->nenum_sigs)) {
      module_free(out);
      return 0;
    }
  }
  // Each optional section is present exactly when its feature flag is.
  if(out->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    if(!parse_const_i64(c, &out->const_i64, &out->nconst_i64)) {
      module_free(out);
      return 0;
    }
    if(!parse_const_f64(c, &out->const_f64, &out->nconst_f64)) {
      module_free(out);
      return 0;
    }
  }
  if(out->features & (uint32_t)JELLO_BC_FEAT_IMPORTS) {
    if(!parse_imports(c, out->atoms, out->natoms, &out->imports, &out->nimports)) {
      module_free(out);
      return 0;
    }
  }
  if(out->features & (uint32_t)JELLO_BC_FEAT_GLOBALS) {
    if(!parse_type_list(c, out->types, out->ntypes, "globals", ':', &out->global_types,
                        &out->nglobals)) {
      module_free(out);
      return 0;
    }
  }
  if(out->features & (uint32_t)JELLO_BC_FEAT_LINE_TABLE) {
    if(!parse_line_table(c, out)) {
      module_free(out);
      return 0;
    }
  }

  func_t* funcs = NULL;
  uint32_t nfuncs = 0, cap = 0;
  for(;;) {
    skip_ws(c);
    if(c->pos >= c->len) break;
    uint32_t idx = 0;
    func_t fn;
    if(!parse_func(c, out->types, out->ntypes, out->atoms, out->natoms, &idx, &fn)) {
      module_free(out);
      free(funcs);
      return 0;
    }
    if(idx != nfuncs) {
      fprintf(stderr, "%s: func[%u] out of order; expected func[%u]\n", c->filename, idx, nfuncs);
      module_free(out);
      free(funcs);
      free(fn.reg_type_idx);
      free(fn.insns);
      return 0;
    }
    if(nfuncs == cap) {
      cap = cap ? cap * 2 : 4;
      funcs = (func_t*)realloc(funcs, cap * sizeof(func_t));
    }
    funcs[nfuncs++] = fn;
  }

  out->nfuncs = nfuncs;
  out->funcs = funcs;

  if(out->entry >= out->nfuncs) {
    fprintf(stderr, "%s: entry=%u out of range (nfuncs=%u)\n", c->filename, out->entry,
            out->nfuncs);
    module_free(out);
    return 0;
  }
  return 1;
}

// The `.jlo` writer; little-endian, in the order jello_bc_read reads.

typedef struct byte_buf {
  uint8_t* data;
  size_t len;
  size_t cap;
} byte_buf;

static void bb_push(byte_buf* bb, const void* p, size_t n) {
  if(bb->len + n > bb->cap) {
    size_t newcap = bb->cap ? bb->cap * 2 : 256;
    while(newcap < bb->len + n) newcap *= 2;
    bb->data = (uint8_t*)realloc(bb->data, newcap);
    bb->cap = newcap;
  }
  memcpy(bb->data + bb->len, p, n);
  bb->len += n;
}

static void bb_u8(byte_buf* bb, uint8_t v) { bb_push(bb, &v, 1); }
static void bb_u16(byte_buf* bb, uint16_t v) {
  uint8_t le[2] = {(uint8_t)(v), (uint8_t)(v >> 8)};
  bb_push(bb, le, 2);
}
static void bb_u32(byte_buf* bb, uint32_t v) {
  uint8_t le[4] = {(uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  bb_push(bb, le, 4);
}

// LEB128, signed and unsigned, as the line table stores its runs.
static void bb_sleb(byte_buf* bb, int64_t v) {
  for(;;) {
    uint8_t b = (uint8_t)(v & 0x7f);
    v >>= 7;
    if((v == 0 && !(b & 0x40u)) || (v == -1 && (b & 0x40u))) {
      bb_u8(bb, b);
      return;
    }
    bb_u8(bb, (uint8_t)(b | 0x80u));
  }
}
static void bb_uleb(byte_buf* bb, uint32_t v) {
  while(v >= 0x80u) {
    bb_u8(bb, (uint8_t)(v | 0x80u));
    v >>= 7;
  }
  bb_u8(bb, (uint8_t)v);
}

static void write_insn_canonical(byte_buf* bb, const jello_insn* in) {
  bb_u8(bb, in->op);
  bb_u8(bb, in->a);
  bb_u8(bb, in->b);
  bb_u8(bb, in->c);
  bb_u32(bb, in->imm);
}

static void write_insn_compressed(byte_buf* bb, const jello_insn* in) {
  if(in->imm == 0) {
    bb_u8(bb, 1); // COMPACT
    bb_u8(bb, in->op);
    bb_u8(bb, in->a);
    bb_u8(bb, in->b);
    bb_u8(bb, in->c);
  } else {
    bb_u8(bb, 0); // FULL
    bb_u8(bb, in->op);
    bb_u8(bb, in->a);
    bb_u8(bb, in->b);
    bb_u8(bb, in->c);
    bb_u32(bb, in->imm);
  }
}

static int write_module(const module_t* m, byte_buf* bb) {
  int compressed = (m->features & (uint32_t)JELLO_BC_FEAT_COMPRESSED_INSN) != 0;

  bb_u32(bb, JLO_MAGIC);
  bb_u32(bb, m->version);
  bb_u32(bb, m->features);
  bb_u32(bb, m->ntypes);
  bb_u32(bb, m->nsigs);
  bb_u32(bb, m->nstruct_sigs);
  bb_u32(bb, m->natoms);
  bb_u32(bb, m->nfuncs);
  bb_u32(bb, m->entry);
  bb_u32(bb, m->nimports); // Present with or without IMPORTS.
  if(m->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA) bb_u32(bb, m->nenum_sigs);

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    bb_u32(bb, m->nconst_i64);
    bb_u32(bb, m->nconst_f64);
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_CONSTBYTES) {
    bb_u32(bb, 0); // nconst_bytes
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_GLOBALS) bb_u32(bb, m->nglobals);

  for(uint32_t i = 0; i < m->ntypes; i++) {
    bb_u8(bb, (uint8_t)m->types[i].kind);
    bb_u8(bb, 0);  // pad
    bb_u16(bb, 0); // pad
    bb_u32(bb, m->types[i].p0);
    bb_u32(bb, 0); // p1
  }

  for(uint32_t i = 0; i < m->nsigs; i++) {
    const asm_fun_sig* sig = &m->sigs[i];
    bb_u32(bb, sig->ret);
    bb_u16(bb, sig->nargs);
    for(uint16_t j = 0; j < sig->nargs; j++) bb_u32(bb, sig->args[j]);
  }

  for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
    const asm_struct_sig* sig = &m->struct_sigs[i];
    bb_u16(bb, sig->nfields);
    bb_u8(bb, sig->named);
    bb_u8(bb, sig->is_mutable);
    for(uint32_t j = 0; j < sig->nfields; j++) bb_u32(bb, sig->field_type[j]);
    if(sig->named) {
      for(uint32_t j = 0; j < sig->nfields; j++) bb_u32(bb, sig->field_name_atom[j]);
    }
  }

  for(uint32_t i = 0; i < m->nenum_sigs; i++) {
    const asm_enum_sig* sig = &m->enum_sigs[i];
    bb_u16(bb, sig->nvariants);
    for(uint16_t j = 0; j < sig->nvariants; j++) {
      bb_u16(bb, sig->variants[j].nfields);
      for(uint16_t k = 0; k < sig->variants[j].nfields; k++)
        bb_u32(bb, sig->variants[j].field_type[k]);
    }
  }

  // Atoms; a u32 length, then the UTF-8 bytes.
  for(uint32_t i = 0; i < m->natoms; i++) {
    uint32_t len = (uint32_t)strlen(m->atoms[i]);
    bb_u32(bb, len);
    bb_push(bb, m->atoms[i], len);
  }

  // Imports follow the atoms, since the loader resolves their names as it reads them.
  for(uint32_t i = 0; i < m->nimports; i++) {
    const asm_import_sig* imp = &m->imports[i];
    bb_u32(bb, imp->library_atom);
    bb_u32(bb, imp->export_atom);
    bb_u8(bb, (uint8_t)imp->ret_kind);
    bb_u8(bb, imp->arity);
    for(int j = 0; j < 8; j++) bb_u8(bb, (uint8_t)imp->arg_kinds[j]);
  }

  // Each 64-bit constant as two u32 halves, low first.
  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    for(uint32_t i = 0; i < m->nconst_i64; i++) {
      uint64_t bits = (uint64_t)m->const_i64[i];
      bb_u32(bb, (uint32_t)bits);
      bb_u32(bb, (uint32_t)(bits >> 32));
    }
    for(uint32_t i = 0; i < m->nconst_f64; i++) {
      uint64_t bits;
      memcpy(&bits, &m->const_f64[i], sizeof bits);
      bb_u32(bb, (uint32_t)bits);
      bb_u32(bb, (uint32_t)(bits >> 32));
    }
  }

  for(uint32_t i = 0; i < m->nglobals; i++) bb_u32(bb, m->global_types[i]);

  for(uint32_t f = 0; f < m->nfuncs; f++) {
    const func_t* fn = &m->funcs[f];
    bb_u32(bb, fn->name_atom);
    bb_u32(bb, fn->nregs);
    bb_u32(bb, fn->cap_start);
    bb_u32(bb, fn->nargs);
    bb_u32(bb, fn->ninsns);
    for(uint32_t r = 0; r < fn->nregs; r++) bb_u32(bb, fn->reg_type_idx[r]);
    for(uint32_t k = 0; k < fn->ninsns; k++) {
      if(compressed) write_insn_compressed(bb, &fn->insns[k]);
      else write_insn_canonical(bb, &fn->insns[k]);
    }
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_LINE_TABLE) {
    bb_u32(bb, m->nsource_files);
    for(uint32_t i = 0; i < m->nsource_files; i++) {
      bb_u32(bb, (uint32_t)strlen(m->source_files[i]));
      bb_push(bb, m->source_files[i], strlen(m->source_files[i]));
    }
    for(uint32_t f = 0; f < m->nfuncs; f++) {
      uint32_t n = 0;
      for(uint32_t i = 0; i < m->nline_runs; i++) n += m->line_runs[i * 5] == f;
      bb_uleb(bb, n);
      uint32_t pc = 0, line = 0;
      for(uint32_t i = 0; i < m->nline_runs; i++) {
        const uint32_t* run = &m->line_runs[i * 5];
        if(run[0] != f) continue;
        // A pc below the last one wraps to a delta the loader rejects.
        bb_uleb(bb, run[1] - pc);
        bb_uleb(bb, run[2]);
        bb_sleb(bb, (int64_t)run[3] - (int64_t)line);
        bb_uleb(bb, run[4]);
        pc = run[1];
        line = run[3];
      }
    }
  }
  return 1;
}

/** Prints `m` in the text form `asm` reads, except const_bytes and imports, which print in a form
 * `asm` does not read. */
static void print_real_module(const jello_bc_module* m, FILE* out) {
  fprintf(out, ".jlo module version=%u features=[", m->version);
  int first = 1;
  for(size_t i = 0; i < NFLAG_NAMES; i++) {
    if(m->features & (uint32_t)FLAG_NAMES[i].bit) {
      if(!first) fprintf(out, ",");
      fprintf(out, "%s", FLAG_NAMES[i].name);
      first = 0;
    }
  }
  fprintf(out, "] entry=%u\n", m->entry);

  fprintf(out, "types: [");
  for(uint32_t i = 0; i < m->ntypes; i++) {
    if(i) fprintf(out, ", ");
    jello_type_kind kind = m->types[i].kind;
    fprintf(out, "%s", kind_name_of(kind));
    if(kind == JELLO_T_LIST || kind == JELLO_T_ARRAY) {
      fprintf(out, "(");
      print_type_ref(out, m, m->types[i].as.unary.elem);
      fprintf(out, ")");
    }
    if(kind == JELLO_T_STRUCT) fprintf(out, "(%u)", m->types[i].as.strct.struct_sig_id);
    if(kind == JELLO_T_FUNCTION) fprintf(out, "(%u)", m->types[i].as.fun.sig_id);
    if(kind == JELLO_T_ABSTRACT) fprintf(out, "(%u)", m->types[i].as.abs.abstract_id);
    if(kind == JELLO_T_ENUM && (m->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA))
      fprintf(out, "(%u)", m->types[i].as.enm.enum_sig_id);
  }
  fprintf(out, "]\n");
  fprintf(out, "sigs: [");
  for(uint32_t i = 0; i < m->nsigs; i++) {
    if(i) fprintf(out, ", ");
    const jello_fun_sig* sig = &m->sigs[i];
    fprintf(out, "{ ret=");
    print_type_ref(out, m, sig->ret);
    fprintf(out, ", nargs=%u, args=[", (unsigned)sig->nargs);
    for(uint16_t j = 0; j < sig->nargs; j++) {
      if(j) fprintf(out, ", ");
      print_type_ref(out, m, sig->args[j]);
    }
    fprintf(out, "] }");
  }
  fprintf(out, "]\n");

  fprintf(out, "atoms: [");
  for(uint32_t i = 0; i < m->natoms; i++) {
    if(i) fprintf(out, ", ");
    fprintf(out, "%s", m->atoms[i]);
  }
  fprintf(out, "]\n");

  fprintf(out, "struct_sigs: [");
  for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
    if(i) fprintf(out, ", ");
    const jello_struct_sig* sig = &m->struct_sigs[i];
    fprintf(out, "{ nfields=%u named=%u mutable=%u fields=[", sig->nfields, sig->named,
            sig->is_mutable);
    for(uint32_t j = 0; j < sig->nfields; j++) {
      if(j) fprintf(out, ", ");
      print_type_ref(out, m, sig->field_type[j]);
    }
    fprintf(out, "]");
    if(sig->named) {
      fprintf(out, " names=[");
      for(uint32_t j = 0; j < sig->nfields; j++) {
        if(j) fprintf(out, ", ");
        fprintf(out, "%s", m->atoms[sig->field_name_atom[j]]);
      }
      fprintf(out, "]");
    }
    fprintf(out, " }");
  }
  fprintf(out, "]\n");

  if(m->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA) {
    fprintf(out, "enum_sigs: [");
    for(uint32_t i = 0; i < m->nenum_sigs; i++) {
      if(i) fprintf(out, ", ");
      fprintf(out, "[");
      for(uint16_t j = 0; j < m->enum_sigs[i].nvariants; j++) {
        if(j) fprintf(out, ", ");
        fprintf(out, "[");
        for(uint16_t k = 0; k < m->enum_sigs[i].variants[j].nfields; k++) {
          if(k) fprintf(out, ", ");
          print_type_ref(out, m, m->enum_sigs[i].variants[j].field_type[k]);
        }
        fprintf(out, "]");
      }
      fprintf(out, "]");
    }
    fprintf(out, "]\n");
  }

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    fprintf(out, "const_i64: [");
    for(uint32_t i = 0; i < m->nconst_i64; i++) {
      if(i) fprintf(out, ", ");
      fprintf(out, "%lld", (long long)m->const_i64[i]);
    }
    fprintf(out, "]\n");
    fprintf(out, "const_f64: [");
    for(uint32_t i = 0; i < m->nconst_f64; i++) {
      if(i) fprintf(out, ", ");
      fprintf(out, "%g", m->const_f64[i]);
    }
    fprintf(out, "]\n");
  }

  // The Bytes pool, escaped as the compiler prints Bytes literals.
  if(m->nconst_bytes > 0) {
    fprintf(out, "const_bytes: [");
    for(uint32_t i = 0; i < m->nconst_bytes; i++) {
      fprintf(out, "%s\"", i ? ", " : "");
      const uint8_t* p = m->const_bytes_values[i]->data;
      for(uint32_t j = 0; j < m->const_bytes_values[i]->length; j++) {
        uint8_t ch = p[j];
        if(ch == '\n') fprintf(out, "\\n");
        else if(ch == '\r') fprintf(out, "\\r");
        else if(ch == '\t') fprintf(out, "\\t");
        else if(ch == '"' || ch == '\\') fprintf(out, "\\%c", ch);
        else if(ch >= 32 && ch <= 126) fputc(ch, out);
        else fprintf(out, "\\x%02x", ch);
      }
      fprintf(out, "\"");
    }
    fprintf(out, "]\n");
  }

  // Native imports, as library.export(arg kinds) -> result kind.
  if(m->nimports > 0) {
    fprintf(out, "imports: [");
    for(uint32_t i = 0; i < m->nimports; i++) {
      const jello_import_sig* imp = &m->imports[i];
      fprintf(out, "%s%s.%s(", i ? ", " : "", m->atoms[imp->library_atom],
              m->atoms[imp->export_atom]);
      for(uint8_t j = 0; j < imp->arity; j++)
        fprintf(out, "%s%s", j ? " " : "", kind_name_of((jello_type_kind)imp->arg_kinds[j]));
      fprintf(out, ") -> %s", kind_name_of((jello_type_kind)imp->ret_kind));
    }
    fprintf(out, "]\n");
  }

  if(m->features & (uint32_t)JELLO_BC_FEAT_GLOBALS) {
    fprintf(out, "globals: [");
    for(uint32_t i = 0; i < m->nglobals; i++) {
      if(i) fprintf(out, ", ");
      print_type_ref(out, m, m->global_types[i]);
    }
    fprintf(out, "]\n");
  }

  if(m->features & (uint32_t)JELLO_BC_FEAT_LINE_TABLE) {
    fprintf(out, "source_files: [");
    for(uint32_t i = 0; i < m->nsource_files; i++)
      fprintf(out, "%s%s", i ? ", " : "", m->source_files[i]);
    fprintf(out, "]\nlines: [");
    int any = 0;
    for(uint32_t f = 0; f < m->nfuncs; f++)
      for(uint32_t i = 0; i < m->funcs[f].nlocs; i++) {
        const jello_bc_location* l = &m->funcs[f].locs[i];
        fprintf(out, "%s%u %u %u %u %u", any ? ", " : "", f, l->pc, l->file, l->line, l->col);
        any = 1;
      }
    fprintf(out, "]\n");
  }

  for(uint32_t f = 0; f < m->nfuncs; f++) {
    const jello_bc_function* fn = &m->funcs[f];
    fprintf(out, "func[%u]", f);
    if(fn->name_atom != JELLO_BC_NO_NAME) fprintf(out, " name=%s", m->atoms[fn->name_atom]);
    fprintf(out, " nregs=%u", fn->nregs);
    if(fn->cap_start != fn->nregs) fprintf(out, " cap_start=%u", fn->cap_start);
    if(fn->nargs != fn->nregs) fprintf(out, " nargs=%u", fn->nargs);
    fprintf(out, " reg_types=[");
    for(uint32_t r = 0; r < fn->nregs; r++) {
      if(r) fprintf(out, ", ");
      print_type_ref(out, m, fn->reg_types[r]);
    }
    fprintf(out, "]\n");
    for(uint32_t k = 0; k < fn->ninsns; k++) {
      const jello_insn* in = &fn->insns[k];
      fprintf(out, "  %u: %s a=r%u b=r%u c=r%u imm=%d\n", k, op_name_of((jello_op)in->op), in->a,
              in->b, in->c, (int32_t)in->imm);
    }
  }
}

/** Prints `r` as a RESULT line. A heap pointer prints only as NULL or NON_NULL, since its address
 * differs from run to run. */
static void print_numeric_result(const jello_numeric_result* r, FILE* out) {
  switch(r->kind) {
    case JELLO_NUM_VOID: fprintf(out, "RESULT kind=VOID\n"); break;
    case JELLO_NUM_BOOL: fprintf(out, "RESULT kind=BOOL value=%d\n", r->as.b); break;
    case JELLO_NUM_I32: fprintf(out, "RESULT kind=I32 value=%d\n", r->as.i32); break;
    case JELLO_NUM_I64: fprintf(out, "RESULT kind=I64 value=%lld\n", (long long)r->as.i64); break;
    case JELLO_NUM_F32: fprintf(out, "RESULT kind=F32 value=%.9g\n", (double)r->as.f32); break;
    case JELLO_NUM_F64: fprintf(out, "RESULT kind=F64 value=%.17g\n", r->as.f64); break;
    case JELLO_NUM_PTR:
      fprintf(out, "RESULT kind=PTR value=%s\n", r->as.ptr ? "NON_NULL" : "NULL");
      break;
    case JELLO_NUM_DYN: {
      jello_value v = r->as.dyn;
      if(jello_is_null(v)) fprintf(out, "RESULT kind=DYN tag=NULL\n");
      else if(jello_is_i32(v)) fprintf(out, "RESULT kind=DYN tag=I32 value=%d\n", jello_as_i32(v));
      else if(jello_is_bool(v))
        fprintf(out, "RESULT kind=DYN tag=BOOL value=%d\n", jello_as_bool(v));
      else if(jello_is_atom(v))
        fprintf(out, "RESULT kind=DYN tag=ATOM value=%u\n", jello_as_atom(v));
      else
        fprintf(out, "RESULT kind=DYN tag=PTR value=%s\n", jello_as_ptr(v) ? "NON_NULL" : "NULL");
      break;
    }
  }
}

static int cmd_run(const char* in_path, int repeat, int gc_stats, uint64_t fuel,
                   uint32_t max_bytes_len, int trap_pc, int host_print) {
  FILE* f = fopen(in_path, "rb");
  if(!f) {
    fprintf(stderr, "%s: cannot open\n", in_path);
    return 1;
  }
  if(fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 1;
  }
  long size = ftell(f);
  if(size < 0) {
    fclose(f);
    return 1;
  }
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
  size_t n = fread(data, 1, (size_t)size, f);
  fclose(f);

  jello_jdll_search_beside(in_path, NULL);
  jello_bc_module* m = NULL;
  jello_bc_result rr = jello_bc_read(data, n, &m);
  free(data);
  if(rr.err) {
    fprintf(stderr, "%s: %s (offset %zu)\n", in_path, rr.msg, rr.offset);
    return 1;
  }

  jello_vm* vm = jello_vm_create();
  if(!vm) {
    fprintf(stderr, "%s: out of memory creating VM\n", in_path);
    jello_bc_free(m);
    return 1;
  }
  if(fuel) jello_vm_set_fuel(vm, fuel);
  if(max_bytes_len) jello_vm_set_max_bytes_len(vm, max_bytes_len);

  jello_jit_module* jm = jello_jit_module_create(m);
  jello_numeric_result result;
  jello_exec_status st = JELLO_EXEC_OK;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for(int i = 0; i < repeat; i++) {
    st = jello_vm_exec_numeric_with_jit(vm, jm, m, &result);
    if(st != JELLO_EXEC_OK) break;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  jello_jit_module_destroy(jm);

  if(st == JELLO_EXEC_OK) {
    if(host_print) jello_print_host_result(m, &result, stdout, 1);
    else print_numeric_result(&result, stdout);
  } else {
    if(host_print) {
      fprintf(stderr, "TRAP code=%s\n", jello_trap_code_name(jello_vm_last_trap_code(vm)));
      fprintf(stderr, "TRAP_PC func_index=%u pc=%u\n", jello_vm_last_trap_func_index(vm),
              jello_vm_last_trap_pc(vm));
    } else {
      printf("TRAP code=%s\n", jello_trap_code_name(jello_vm_last_trap_code(vm)));
      if(trap_pc) {
        printf("TRAP_PC func_index=%u pc=%u\n", jello_vm_last_trap_func_index(vm),
               jello_vm_last_trap_pc(vm));
      }
    }
  }
  if(repeat > 1) {
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("ELAPSED_MS %.3f\n", ms);
  }
  if(gc_stats) {
    uint64_t live_bytes = 0;
    uint32_t collections = 0;
    double pause_ms = 0.0;
    jello_vm_gc_stats(vm, &live_bytes, &collections, &pause_ms);
    printf("GC_LIVE_BYTES %llu\n", (unsigned long long)live_bytes);
    printf("GC_COLLECTIONS %u\n", collections);
    printf("GC_PAUSE_MS %.3f\n", pause_ms);
    printf("GC_FINALIZED_HANDLES %u\n", vm->space->finalized_handles);
  }

  jello_vm_destroy(vm);
  jello_bc_free(m);
  if(host_print && st != JELLO_EXEC_OK) return 1;
  return 0;
}

/* dumpir prints each function's JIT IR as `jir fn[N] nregs=.. flags=[..] nslow=..` and then a
 * `bc_pc=.. OPNAME operands..` line per instruction, showing only the operands the opcode uses. */

static const char* jir_op_name(jello_jit_ir_op op) {
  switch(op) {
    case JIR_NOP: return "NOP";
    case JIR_RET: return "RET";
    case JIR_JMP: return "JMP";
    case JIR_JMP_IF: return "JMP_IF";
    case JIR_MOV: return "MOV";
    case JIR_SAFEPOINT: return "SAFEPOINT";
    case JIR_SLOW: return "SLOW";
    case JIR_CALL_SELF: return "CALL_SELF";
    case JIR_CALL_DIRECT: return "CALL_DIRECT";
    case JIR_CALL_NATIVE: return "CALL_NATIVE";
    case JIR_CALLR: return "CALLR";
    case JIR_TRY: return "TRY";
    case JIR_ENDTRY: return "ENDTRY";
    case JIR_THROW: return "THROW";
    case JIR_CONST_I8_IMM: return "CONST_I8_IMM";
    case JIR_CONST_I32: return "CONST_I32";
    case JIR_CONST_I64: return "CONST_I64";
    case JIR_CONST_F16: return "CONST_F16";
    case JIR_CONST_F32: return "CONST_F32";
    case JIR_CONST_F64: return "CONST_F64";
    case JIR_CONST_BOOL: return "CONST_BOOL";
    case JIR_CONST_NULL: return "CONST_NULL";
    case JIR_CONST_ATOM: return "CONST_ATOM";
    case JIR_BIN_I32: return "BIN_I32";
    case JIR_BIN_I64: return "BIN_I64";
    case JIR_BIN_F16: return "BIN_F16";
    case JIR_BIN_F32: return "BIN_F32";
    case JIR_BIN_F64: return "BIN_F64";
    case JIR_ADD_I32_IMM: return "ADD_I32_IMM";
    case JIR_SUB_I32_IMM: return "SUB_I32_IMM";
    case JIR_MUL_I32_IMM: return "MUL_I32_IMM";
    case JIR_NEG_I32: return "NEG_I32";
    case JIR_NEG_I64: return "NEG_I64";
    case JIR_NEG_F32: return "NEG_F32";
    case JIR_NEG_F64: return "NEG_F64";
    case JIR_NOT_BOOL: return "NOT_BOOL";
    case JIR_CMP_I32: return "CMP_I32";
    case JIR_CMP_I64: return "CMP_I64";
    case JIR_CMP_F32: return "CMP_F32";
    case JIR_CMP_F64: return "CMP_F64";
    case JIR_EQ_I32_IMM: return "EQ_I32_IMM";
    case JIR_LT_I32_IMM: return "LT_I32_IMM";
    case JIR_CONV: return "CONV";
    case JIR_TO_DYN: return "TO_DYN";
    case JIR_FROM_DYN: return "FROM_DYN";
    case JIR_KINDOF: return "KINDOF";
    case JIR_IS_NULL: return "IS_NULL";
    case JIR_IS_ENUM: return "IS_ENUM";
    case JIR_NULL_CHECK: return "NULL_CHECK";
    case JIR_PHYSEQ: return "PHYSEQ";
    case JIR_BYTES_NEW: return "BYTES_NEW";
    case JIR_BYTES_LEN: return "BYTES_LEN";
    case JIR_BYTES_GET_U8: return "BYTES_GET_U8";
    case JIR_BYTES_SET_U8: return "BYTES_SET_U8";
    case JIR_BYTES_EQ: return "BYTES_EQ";
    case JIR_BYTES_CONCAT2: return "BYTES_CONCAT2";
    case JIR_BYTES_BITOP: return "BYTES_BITOP";
    case JIR_BYTES_READ: return "BYTES_READ";
    case JIR_BYTES_WRITE: return "BYTES_WRITE";
    case JIR_BYTES_BLIT: return "BYTES_BLIT";
    case JIR_BITS_WRITE: return "BITS_WRITE";
    case JIR_BITS_READ: return "BITS_READ";
    case JIR_BYTES_FREEZE: return "BYTES_FREEZE";
    case JIR_STRUCT_NEW: return "STRUCT_NEW";
    case JIR_STRUCT_GET: return "STRUCT_GET";
    case JIR_STRUCT_SET: return "STRUCT_SET";
    case JIR_OBJ_NEW: return "OBJ_NEW";
    case JIR_OBJ_HAS_ATOM: return "OBJ_HAS_ATOM";
    case JIR_OBJ_GET_ATOM: return "OBJ_GET_ATOM";
    case JIR_OBJ_SET_ATOM: return "OBJ_SET_ATOM";
    case JIR_OBJ_GET: return "OBJ_GET";
    case JIR_OBJ_SET: return "OBJ_SET";
    case JIR_OBJ_HAS: return "OBJ_HAS";
    case JIR_OBJ_REMOVE: return "OBJ_REMOVE";
    case JIR_OBJ_CLEAR: return "OBJ_CLEAR";
    case JIR_OBJ_COPY: return "OBJ_COPY";
    case JIR_ARRAY_LEN: return "ARRAY_LEN";
    case JIR_ARRAY_GET: return "ARRAY_GET";
    case JIR_ARRAY_SET: return "ARRAY_SET";
    case JIR_GLOBAL_GET: return "GLOBAL_GET";
    case JIR_GLOBAL_SET: return "GLOBAL_SET";
    default: return "?";
  }
}

static const char* jir_bin_name(uint32_t v) {
  switch((jello_jir_bin)v) {
    case JIR_BIN_ADD: return "ADD";
    case JIR_BIN_SUB: return "SUB";
    case JIR_BIN_MUL: return "MUL";
    case JIR_BIN_DIV: return "DIV";
    case JIR_BIN_MOD: return "MOD";
    case JIR_BIN_SHL: return "SHL";
    case JIR_BIN_SHR: return "SHR";
    case JIR_BIN_AND: return "AND";
    case JIR_BIN_OR: return "OR";
    case JIR_BIN_XOR: return "XOR";
    default: return "?";
  }
}
static const char* jir_cmp_name(uint32_t v) {
  switch((jello_jir_cmp)v) {
    case JIR_CMP_EQ: return "EQ";
    case JIR_CMP_LT: return "LT";
    default: return "?";
  }
}
static const char* jir_conv_name(uint32_t v) {
  switch((jello_jir_conv)v) {
    case JIR_CONV_SEXT_I64: return "SEXT_I64";
    case JIR_CONV_SEXT_I16: return "SEXT_I16";
    case JIR_CONV_TRUNC_I8: return "TRUNC_I8";
    case JIR_CONV_TRUNC_I16: return "TRUNC_I16";
    case JIR_CONV_I32_FROM_I64: return "I32_FROM_I64";
    case JIR_CONV_F64_FROM_I32: return "F64_FROM_I32";
    case JIR_CONV_I32_FROM_F64: return "I32_FROM_F64";
    case JIR_CONV_F64_FROM_I64: return "F64_FROM_I64";
    case JIR_CONV_I64_FROM_F64: return "I64_FROM_F64";
    case JIR_CONV_F32_FROM_I32: return "F32_FROM_I32";
    case JIR_CONV_I32_FROM_F32: return "I32_FROM_F32";
    case JIR_CONV_F64_FROM_F32: return "F64_FROM_F32";
    case JIR_CONV_F32_FROM_F64: return "F32_FROM_F64";
    case JIR_CONV_F32_FROM_I64: return "F32_FROM_I64";
    case JIR_CONV_I64_FROM_F32: return "I64_FROM_F32";
    case JIR_CONV_F16_FROM_F32: return "F16_FROM_F32";
    case JIR_CONV_F32_FROM_F16: return "F32_FROM_F16";
    case JIR_CONV_F16_FROM_I32: return "F16_FROM_I32";
    case JIR_CONV_I32_FROM_F16: return "I32_FROM_F16";
    default: return "?";
  }
}
static const char* jir_brw_name(uint32_t v) {
  switch((jello_jir_bytes_rw)v) {
    case JIR_BRW_U16_LE: return "U16_LE";
    case JIR_BRW_U16_BE: return "U16_BE";
    case JIR_BRW_U32_LE: return "U32_LE";
    case JIR_BRW_U32_BE: return "U32_BE";
    case JIR_BRW_I32_LE: return "I32_LE";
    case JIR_BRW_I32_BE: return "I32_BE";
    case JIR_BRW_F32_LE: return "F32_LE";
    case JIR_BRW_F32_BE: return "F32_BE";
    case JIR_BRW_I64_LE: return "I64_LE";
    case JIR_BRW_I64_BE: return "I64_BE";
    case JIR_BRW_F64_LE: return "F64_LE";
    case JIR_BRW_F64_BE: return "F64_BE";
    default: return "?";
  }
}

// Which operands of a JIR instruction the dump shows, and how it names the immediate.
typedef enum jir_imm_kind {
  JIRSHAPE_NONE,
  JIRSHAPE_BIN,
  JIRSHAPE_CMP,
  JIRSHAPE_CONV,
  JIRSHAPE_BRW,
  JIRSHAPE_KIND,
  JIRSHAPE_RAW
} jir_imm_kind;
typedef struct jir_shape {
  int has_a, has_b, has_c, has_imm;
  jir_imm_kind imm_kind;
  int has_c_literal; /**< `c` is a bit width, not a register (BITS_WRITE, BITS_READ). */
} jir_shape;

/** The register operands come from jello_jit_ir_op_reg_mask, which jello_jit_ir_verify also
 * checks, so the dump and the verifier agree on them. */
static jir_shape jir_shape_of(jello_jit_ir_op op) {
  unsigned mask = jello_jit_ir_op_reg_mask(op);
  jir_shape s;
  s.has_a = (mask & JELLO_JIR_REG_A) != 0;
  s.has_b = (mask & JELLO_JIR_REG_B) != 0;
  s.has_c = (mask & JELLO_JIR_REG_C) != 0;
  s.has_imm = 0;
  s.imm_kind = JIRSHAPE_NONE;
  s.has_c_literal = (op == JIR_BITS_WRITE || op == JIR_BITS_READ);

  switch(op) {
    case JIR_JMP:
    case JIR_JMP_IF:
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32:
    case JIR_CONST_I64:
    case JIR_CONST_F16:
    case JIR_CONST_F32:
    case JIR_CONST_F64:
    case JIR_CONST_BOOL:
    case JIR_CONST_ATOM:
    case JIR_ADD_I32_IMM:
    case JIR_SUB_I32_IMM:
    case JIR_MUL_I32_IMM:
    case JIR_EQ_I32_IMM:
    case JIR_LT_I32_IMM:
    case JIR_STRUCT_GET:
    case JIR_STRUCT_SET:
    case JIR_OBJ_HAS_ATOM:
    case JIR_OBJ_GET_ATOM:
    case JIR_OBJ_SET_ATOM:
    case JIR_BITS_WRITE:
    case JIR_BITS_READ:
    case JIR_CALL_DIRECT:
    case JIR_CALL_NATIVE:
    case JIR_CALLR:
    case JIR_SLOW:
    case JIR_TRY:
    case JIR_GLOBAL_GET:
    case JIR_GLOBAL_SET:
      s.has_imm = 1;
      s.imm_kind = JIRSHAPE_RAW;
      break;
    case JIR_BIN_I32:
    case JIR_BIN_I64:
    case JIR_BIN_F16:
    case JIR_BIN_F32:
    case JIR_BIN_F64:
    case JIR_BYTES_BITOP:
      s.has_imm = 1;
      s.imm_kind = JIRSHAPE_BIN;
      break;
    case JIR_CMP_I32:
    case JIR_CMP_I64:
    case JIR_CMP_F32:
    case JIR_CMP_F64:
      s.has_imm = 1;
      s.imm_kind = JIRSHAPE_CMP;
      break;
    case JIR_CONV:
      s.has_imm = 1;
      s.imm_kind = JIRSHAPE_CONV;
      break;
    case JIR_FROM_DYN:
      s.has_imm = 1;
      s.imm_kind = JIRSHAPE_KIND;
      break;
    case JIR_BYTES_READ:
    case JIR_BYTES_WRITE:
      s.has_imm = 1;
      s.imm_kind = JIRSHAPE_BRW;
      break;
    default: break;
  }
  return s;
}

static void print_jir_imm(FILE* out, jir_imm_kind imm_kind, uint32_t imm) {
  switch(imm_kind) {
    case JIRSHAPE_BIN: fprintf(out, "imm=%s", jir_bin_name(imm)); break;
    case JIRSHAPE_CMP: fprintf(out, "imm=%s", jir_cmp_name(imm)); break;
    case JIRSHAPE_CONV: fprintf(out, "imm=%s", jir_conv_name(imm)); break;
    case JIRSHAPE_BRW: fprintf(out, "imm=%s", jir_brw_name(imm)); break;
    case JIRSHAPE_KIND: fprintf(out, "imm=%s", kind_name_of((jello_type_kind)imm)); break;
    // Signed, as disasm prints it, so a backward jump's delta reads as negative.
    default: fprintf(out, "imm=%d", (int32_t)imm); break;
  }
}

static void print_jir_function(const jello_jit_ir_function* jf, uint32_t fi, FILE* out) {
  fprintf(out, "jir fn[%u] nregs=%u flags=[%s] nslow=%u\n", fi, jf->nregs,
          (jf->flags & JELLO_JIR_FLAG_NUMERIC_FAST) ? "numeric_fast" : "", jf->nslow);
  for(uint32_t i = 0; i < jf->ninsns; i++) {
    const jello_jit_ir_insn* ins = &jf->insns[i];
    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    jir_shape shape = jir_shape_of(op);
    fprintf(out, "  bc_pc=%u  %s", ins->bc_pc, jir_op_name(op));
    if(shape.has_a) fprintf(out, " a=r%u", ins->a);
    if(shape.has_b) fprintf(out, " b=r%u", ins->b);
    if(shape.has_c_literal) fprintf(out, " width=%u", ins->c);
    if(shape.has_c) fprintf(out, " c=r%u", ins->c);
    if(shape.has_imm) {
      fprintf(out, " ");
      print_jir_imm(out, shape.imm_kind, ins->imm);
    }
    fprintf(out, "\n");
  }
}

static int cmd_dumpir(const char* in_path, int func_filter, int has_func_filter) {
  FILE* f = fopen(in_path, "rb");
  if(!f) {
    fprintf(stderr, "%s: cannot open\n", in_path);
    return 1;
  }
  if(fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 1;
  }
  long size = ftell(f);
  if(size < 0) {
    fclose(f);
    return 1;
  }
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
  size_t n = fread(data, 1, (size_t)size, f);
  fclose(f);

  jello_jdll_search_beside(in_path, NULL);
  jello_bc_module* m = NULL;
  jello_bc_result rr = jello_bc_read(data, n, &m);
  free(data);
  if(rr.err) {
    fprintf(stderr, "%s: %s (offset %zu)\n", in_path, rr.msg, rr.offset);
    return 1;
  }

  uint32_t nfuncs = m->nfuncs;
  for(uint32_t fi = 0; fi < nfuncs; fi++) {
    if(has_func_filter && (uint32_t)func_filter != fi) continue;
    jello_jit_ir_function* jf = jello_jit_ir_build(m, fi);
    if(!jf) {
      fprintf(stderr, "%s: jello_jit_ir_build failed for fn[%u]\n", in_path, fi);
      jello_bc_free(m);
      return 1;
    }
    if(!jello_jit_ir_verify(jf, &m->funcs[fi])) {
      fprintf(stderr, "%s: jello_jit_ir_verify rejected fn[%u]\n", in_path, fi);
      jello_jit_ir_free(jf);
      jello_bc_free(m);
      return 1;
    }
    print_jir_function(jf, fi, stdout);
    jello_jit_ir_free(jf);
  }

  jello_bc_free(m);
  return 0;
}

static char* read_whole_file(const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if(!f) {
    fprintf(stderr, "%s: cannot open: %s\n", path, strerror(errno));
    return NULL;
  }
  if(fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if(size < 0) {
    fclose(f);
    return NULL;
  }
  if(fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char* buf = (char*)malloc((size_t)size + 1);
  if(!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[n] = '\0';
  *out_len = n;
  return buf;
}

static void usage(const char* prog) {
  fprintf(stderr, "usage: %s asm <in.jlo.txt> -o <out.jlo>\n", prog);
  fprintf(stderr, "       %s disasm <in.jlo> [-o <out.jlo.txt>]\n", prog);
  fprintf(stderr,
          "       %s run [--print] <in.jlo> [--print] [--repeat N] [--gc-stats] [--fuel N] "
          "[--max-bytes-len N] [--trap-pc]\n",
          prog);
  fprintf(stderr, "       %s dumpir <in.jlo> [--func N]\n", prog);
  fprintf(stderr, "       %s init [DIR] [--name NAME]\n", prog);
  fprintf(stderr, "       %s get-deps [DIR]\n", prog);
}

static int cmd_asm(const char* in_path, const char* out_path) {
  size_t len = 0;
  char* text = read_whole_file(in_path, &len);
  if(!text) return 1;

  cursor c = {text, len, 0, in_path};
  module_t m;
  if(!parse_module(&c, &m)) {
    free(text);
    return 1;
  }
  free(text);

  byte_buf bb = {0};
  write_module(&m, &bb);
  module_free(&m);

  FILE* out = fopen(out_path, "wb");
  if(!out) {
    fprintf(stderr, "%s: cannot open for writing\n", out_path);
    free(bb.data);
    return 1;
  }
  fwrite(bb.data, 1, bb.len, out);
  fclose(out);
  free(bb.data);
  return 0;
}

static int cmd_disasm(const char* in_path, const char* out_path) {
  FILE* f = fopen(in_path, "rb");
  if(!f) {
    fprintf(stderr, "%s: cannot open\n", in_path);
    return 1;
  }
  if(fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 1;
  }
  long size = ftell(f);
  if(size < 0) {
    fclose(f);
    return 1;
  }
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
  size_t n = fread(data, 1, (size_t)size, f);
  fclose(f);

  jello_jdll_search_beside(in_path, NULL);
  jello_bc_module* m = NULL;
  jello_bc_result rr = jello_bc_read(data, n, &m);
  free(data);
  if(rr.err) {
    fprintf(stderr, "%s: %s (offset %zu)\n", in_path, rr.msg, rr.offset);
    return 1;
  }

  FILE* out = out_path ? fopen(out_path, "w") : stdout;
  if(!out) {
    fprintf(stderr, "%s: cannot open for writing\n", out_path);
    jello_bc_free(m);
    return 1;
  }
  print_real_module(m, out);
  if(out_path) fclose(out);
  jello_bc_free(m);
  return 0;
}

int main(int argc, char** argv) {
  jello_host_start();
  if(argc < 2) {
    usage(argv[0]);
    return 1;
  }
  // The project commands take their own arguments (deps.c).
  if(strcmp(argv[1], "init") == 0) return jlo_init_main(argc - 2, argv + 2);
  if(strcmp(argv[1], "get-deps") == 0) return jlo_get_deps_main(argc - 2, argv + 2);

  const char* subcmd = argv[1];
  const char* in_path = NULL;
  const char* out_path = NULL;
  int repeat = 1;
  int gc_stats = 0;
  int func_filter = 0;
  int has_func_filter = 0;
  uint64_t fuel = 0;
  uint32_t max_bytes_len = 0;
  int trap_pc = 0;
  int host_print = 0;
  for(int i = 2; i < argc; i++) {
    if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out_path = argv[i + 1];
      i++;
    } else if(strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
      repeat = atoi(argv[i + 1]);
      i++;
    } else if(strcmp(argv[i], "--gc-stats") == 0) {
      gc_stats = 1;
    } else if(strcmp(argv[i], "--func") == 0 && i + 1 < argc) {
      func_filter = atoi(argv[i + 1]);
      has_func_filter = 1;
      i++;
    } else if(strcmp(argv[i], "--fuel") == 0 && i + 1 < argc) {
      fuel = strtoull(argv[i + 1], NULL, 10);
      i++;
    } else if(strcmp(argv[i], "--max-bytes-len") == 0 && i + 1 < argc) {
      max_bytes_len = (uint32_t)strtoul(argv[i + 1], NULL, 10);
      i++;
    } else if(strcmp(argv[i], "--trap-pc") == 0) {
      trap_pc = 1;
    } else if(strcmp(argv[i], "--print") == 0) {
      host_print = 1;
    } else if(!in_path) {
      in_path = argv[i];
    } else {
      usage(argv[0]);
      return 1;
    }
  }
  if(!in_path) {
    usage(argv[0]);
    return 1;
  }

  if(strcmp(subcmd, "asm") == 0) {
    if(!out_path) {
      usage(argv[0]);
      return 1;
    }
    return cmd_asm(in_path, out_path);
  }
  if(strcmp(subcmd, "disasm") == 0) { return cmd_disasm(in_path, out_path); }
  if(strcmp(subcmd, "run") == 0) {
    return cmd_run(in_path, repeat > 0 ? repeat : 1, gc_stats, fuel, max_bytes_len, trap_pc,
                   host_print);
  }
  if(strcmp(subcmd, "dumpir") == 0) { return cmd_dumpir(in_path, func_filter, has_func_filter); }
  usage(argv[0]);
  return 1;
}
