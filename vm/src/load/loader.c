// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The `.jlo` reader; parses a module into memory it owns, loads and checks its native imports,
 * then runs check.c's validation over every function. */

#include <jello/internal.h>
#include <jello/jdll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JELLO_JLO_MAGIC 0x4A4C5942u
#define JELLO_JLO_VERSION 1u

/** A bounds-checked cursor over the `.jlo` bytes; every multi-byte read is little-endian. */
typedef struct rd {
  const uint8_t* p;
  size_t n;
  size_t off;
} rd;

static jello_bc_result ok(void) {
  jello_bc_result r = {JELLO_BC_OK, "ok", 0};
  return r;
}

static jello_bc_result err(jello_bc_error e, const char* msg, size_t off) {
  jello_bc_result r = {e, msg, off};
  return r;
}

/** The last validation failure's message on this thread, overwritten by its next one. */
static _Thread_local char g_validate_err[512];

/** The Jello name of a native argument or result kind, as a binding of the native writes it. */
static const char* native_kind_name(uint8_t kind) {
  static const char* const names[] = {
      [JELLO_T_VOID] = "Void",         [JELLO_T_BOOL] = "Bool",
      [JELLO_T_ATOM] = "Atom",         [JELLO_T_I8] = "I8",
      [JELLO_T_I16] = "I16",           [JELLO_T_I32] = "I32",
      [JELLO_T_I64] = "I64",           [JELLO_T_F16] = "F16",
      [JELLO_T_F32] = "F32",           [JELLO_T_F64] = "F64",
      [JELLO_T_BYTES] = "Bytes",       [JELLO_T_LIST] = "List",
      [JELLO_T_ARRAY] = "Array",       [JELLO_T_STRUCT] = "Struct",
      [JELLO_T_OBJECT] = "Object",     [JELLO_T_FUNCTION] = "Function",
      [JELLO_T_ABSTRACT] = "Abstract", [JELLO_T_DYNAMIC] = "Dynamic",
      [JELLO_T_ENUM] = "Enum",
  };
  return kind < sizeof names / sizeof names[0] && names[kind] ? names[kind] : "?";
}

/** Appends `text` to the string `out`, of `size` bytes, as far as it fits. */
static void append_text(char* out, size_t size, const char* text) {
  size_t at = strlen(out);
  if(at + 1 < size) snprintf(out + at, size - at, "%s", text);
}

/** Appends to the string `out`, of `size` bytes, the function type `(A, B) -> R` of `arity`
 * arguments of kinds `args` and a result of kind `ret`. */
static void append_signature(char* out, size_t size, uint8_t ret, uint8_t arity,
                             const uint8_t* args) {
  append_text(out, size, "(");
  for(uint8_t j = 0; j < arity; j++) {
    if(j) append_text(out, size, ", ");
    append_text(out, size, native_kind_name(args[j]));
  }
  append_text(out, size, ") -> ");
  append_text(out, size, native_kind_name(ret));
}

/** Validates instruction `pc` of function `fi`. A failure's message (in g_validate_err) adds
 * the position, the opcode, its operands and the types and kinds of registers a and b. */
static jello_bc_result validate_insn_ctx(const jello_bc_module* m, const jello_bc_function* f,
                                         uint32_t fi, uint32_t pc) {
  const jello_insn* ins = &f->insns[pc];
  jello_bc_result vr =
      jello_bc_validate_insn(m, f->reg_types, ins, f->nregs, pc, f->ninsns, m->nfuncs);
  if(!vr.err) return ok();
  {
    const char* opn = "?";
    if((uint32_t)ins->op < (uint32_t)JOP_COUNT) opn = jello_op_table[ins->op].name;
    uint32_t ta = 0;
    uint32_t tb = 0;
    unsigned ka = 0;
    unsigned kb = 0;
    if(ins->a < f->nregs) {
      ta = f->reg_types[ins->a];
      if(ta < m->ntypes) ka = (unsigned)m->types[ta].kind;
    }
    if(ins->b < f->nregs) {
      tb = f->reg_types[ins->b];
      if(tb < m->ntypes) kb = (unsigned)m->types[tb].kind;
    }
    snprintf(g_validate_err, sizeof g_validate_err,
             "%s at func[%u] pc=%u %s a=r%u b=r%u c=r%u tid_a=%u kind_a=%u tid_b=%u kind_b=%u",
             vr.msg ? vr.msg : "validate failed", fi, pc, opn, ins->a, ins->b, ins->c, ta, ka, tb,
             kb);
    return err(JELLO_BC_BAD_FORMAT, g_validate_err, 0);
  }
}

static int rd_need(rd* r, size_t k) { return r->off + k <= r->n; }

static jello_bc_result rd_u8(rd* r, uint8_t* out) {
  if(!rd_need(r, 1)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  *out = r->p[r->off++];
  return ok();
}

static jello_bc_result rd_u16(rd* r, uint16_t* out) {
  if(!rd_need(r, 2)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  *out = (uint16_t)((uint16_t)r->p[r->off] | ((uint16_t)r->p[r->off + 1] << 8));
  r->off += 2;
  return ok();
}

static jello_bc_result rd_u32(rd* r, uint32_t* out) {
  if(!rd_need(r, 4)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  *out = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
         ((uint32_t)r->p[r->off + 2] << 16) | ((uint32_t)r->p[r->off + 3] << 24);
  r->off += 4;
  return ok();
}

static jello_bc_result rd_bytes(rd* r, uint8_t* out, size_t n) {
  if(!rd_need(r, n)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  memcpy(out, r->p + r->off, n);
  r->off += n;
  return ok();
}

/** Rounds `x` up to a multiple of `a`, a power of two; 0 leaves `x` unchanged. */
static uint32_t align_up(uint32_t x, uint32_t a) {
  return (a == 0u) ? x : (x + (a - 1u)) & ~(a - 1u);
}

static jello_bc_result read_insn_canonical(rd* r, jello_insn* out) {
  jello_bc_result rr;
  uint8_t op, a, b, c;
  uint32_t imm;
  if((rr = rd_u8(r, &op)).err) return rr;
  if((rr = rd_u8(r, &a)).err) return rr;
  if((rr = rd_u8(r, &b)).err) return rr;
  if((rr = rd_u8(r, &c)).err) return rr;
  if((rr = rd_u32(r, &imm)).err) return rr;
  out->op = op;
  out->a = a;
  out->b = b;
  out->c = c;
  out->imm = imm;
  return ok();
}

/** Reads a compressed instruction; a tag byte, then op, a, b and c, then a u32 imm only when
 * the tag is 0 (tag 1 means imm 0). */
static jello_bc_result read_insn_compressed(rd* r, jello_insn* out) {
  jello_bc_result rr;
  uint8_t tag, op, a, b, c;
  if((rr = rd_u8(r, &tag)).err) return rr;
  if(tag != 0 && tag != 1) return err(JELLO_BC_BAD_FORMAT, "bad compressed insn tag", r->off - 1);
  if((rr = rd_u8(r, &op)).err) return rr;
  if((rr = rd_u8(r, &a)).err) return rr;
  if((rr = rd_u8(r, &b)).err) return rr;
  if((rr = rd_u8(r, &c)).err) return rr;
  uint32_t imm = 0;
  if(tag == 0) {
    if((rr = rd_u32(r, &imm)).err) return rr;
  }
  out->op = op;
  out->a = a;
  out->b = b;
  out->c = c;
  out->imm = imm;
  return ok();
}

void jello_bc_free(jello_bc_module* m) {
  if(!m) return;
  if(m->jdll_handles) {
    jello_jdll_handle** handles = (jello_jdll_handle**)m->jdll_handles;
    for(uint32_t i = 0; i < m->njdll_handles; i++) jello_jdll_unload(handles[i]);
    free(handles);
  }
  free((void*)m->imports);
  free((void*)m->types);
  if(m->sigs) {
    for(uint32_t i = 0; i < m->nsigs; i++) free((void*)m->sigs[i].args);
    free((void*)m->sigs);
  }
  if(m->struct_sigs) {
    for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
      free((void*)m->struct_sigs[i].field_type);
      free((void*)m->struct_sigs[i].field_name_atom);
      free((void*)m->struct_sigs[i].field_offset);
    }
    free((void*)m->struct_sigs);
  }
  if(m->enum_sigs) {
    for(uint32_t i = 0; i < m->nenum_sigs; i++) {
      for(uint16_t j = 0; j < m->enum_sigs[i].nvariants; j++)
        free((void*)m->enum_sigs[i].variants[j].field_type);
      free((void*)m->enum_sigs[i].variants);
    }
    free((void*)m->enum_sigs);
  }
  free((void*)m->atoms_data);
  free((void*)m->atoms);
  free((void*)m->const_i64);
  free((void*)m->const_f64);
  free((void*)m->const_bytes_values);
  free((void*)m->global_types);
  free(m->globals);
  free((void*)m->ptr_globals);
  for(uint32_t i = 0; m->const_funs && i < m->nfuncs; i++) vm_immortal_free(m->const_funs[i]);
  free((void*)m->const_funs);
  for(uint32_t i = 0; m->const_enums && i < m->nconst_enums; i++)
    vm_immortal_free(m->const_enums[i]);
  free((void*)m->const_enums);
  free((void*)m->const_enum_base);
  if(m->funcs) {
    for(uint32_t i = 0; i < m->nfuncs; i++) {
      free((void*)m->funcs[i].reg_types);
      free((void*)m->funcs[i].insns);
      free((void*)m->funcs[i].locs);
    }
    free((void*)m->funcs);
  }
  if(m->source_files) {
    for(uint32_t i = 0; i < m->nsource_files; i++) free((void*)m->source_files[i]);
    free((void*)m->source_files);
  }
  free(m);
}

/** Reads a LEB128 varint of at most five bytes; unsigned up to UINT32_MAX, or with `is_signed`
 * signed within int32. */
static jello_bc_result rd_leb(rd* r, int is_signed, int64_t* out) {
  size_t at = r->off;
  uint64_t v = 0;
  for(unsigned shift = 0;; shift += 7u) {
    if(shift == 35u) return err(JELLO_BC_BAD_FORMAT, "varint longer than 5 bytes", at);
    if(!rd_need(r, 1)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
    uint8_t b = r->p[r->off++];
    v |= (uint64_t)(b & 0x7fu) << shift;
    if(b & 0x80u) continue;
    if(is_signed) {
      if(b & 0x40u) v |= ~(uint64_t)0 << (shift + 7u);
      int64_t sv = (int64_t)v;
      if(sv < INT32_MIN || sv > INT32_MAX)
        return err(JELLO_BC_BAD_FORMAT, "varint out of range", at);
      *out = sv;
    } else {
      if(v > UINT32_MAX) return err(JELLO_BC_BAD_FORMAT, "varint out of range", at);
      *out = (int64_t)v;
    }
    return ok();
  }
}

/** Reads the line table, which follows the functions (JELLO_BC_FEAT_LINE_TABLE); a u32 file
 * count, each file as a u32 length and its bytes; then per function a ULEB128 run count and
 * runs of ULEB128 pc delta (the first absolute), ULEB128 file, SLEB128 line delta (the first
 * from 0) and ULEB128 column. It is checked even with JELLO_BC_SKIP_VALIDATE, since trap
 * reporting indexes with it. */
static jello_bc_result read_line_table(rd* r, jello_bc_module* m) {
  jello_bc_result rr;
  uint32_t nfiles = 0;
  if((rr = rd_u32(r, &nfiles)).err) return rr;
  if(nfiles > (uint32_t)((r->n - r->off) / 4u))
    return err(JELLO_BC_BAD_FORMAT, "line table file count too large", r->off - 4);
  char** files = nfiles ? (char**)calloc(nfiles, sizeof(char*)) : NULL;
  if(nfiles && !files) return err(JELLO_BC_OUT_OF_MEMORY, "oom source files", r->off);
  m->source_files = (const char* const*)files;
  m->nsource_files = nfiles;
  for(uint32_t i = 0; i < nfiles; i++) {
    uint32_t len = 0;
    if((rr = rd_u32(r, &len)).err) return rr;
    if(len == 0 || len > r->n - r->off)
      return err(JELLO_BC_BAD_FORMAT, "line table path length out of range", r->off - 4);
    if(memchr(r->p + r->off, 0, len))
      return err(JELLO_BC_BAD_FORMAT, "line table path contains NUL", r->off);
    files[i] = (char*)malloc((size_t)len + 1u);
    if(!files[i]) return err(JELLO_BC_OUT_OF_MEMORY, "oom source file", r->off);
    memcpy(files[i], r->p + r->off, len);
    files[i][len] = 0;
    r->off += len;
  }
  for(uint32_t fi = 0; fi < m->nfuncs; fi++) {
    jello_bc_function* f = (jello_bc_function*)&m->funcs[fi];
    int64_t nlocs = 0;
    size_t at = r->off;
    if((rr = rd_leb(r, 0, &nlocs)).err) return rr;
    if(nlocs > (int64_t)f->ninsns)
      return err(JELLO_BC_BAD_FORMAT, "line table has more runs than instructions", at);
    if(nlocs == 0) continue;
    jello_bc_location* locs = (jello_bc_location*)calloc((size_t)nlocs, sizeof(jello_bc_location));
    if(!locs) return err(JELLO_BC_OUT_OF_MEMORY, "oom line table", r->off);
    f->locs = locs;
    f->nlocs = (uint32_t)nlocs;
    int64_t pc = 0, line = 0;
    for(uint32_t j = 0; j < f->nlocs; j++) {
      jello_bc_location* l = &locs[j];
      int64_t dpc = 0, file = 0, dline = 0, col = 0;
      at = r->off;
      if((rr = rd_leb(r, 0, &dpc)).err || (rr = rd_leb(r, 0, &file)).err ||
         (rr = rd_leb(r, 1, &dline)).err || (rr = rd_leb(r, 0, &col)).err)
        return rr;
      if(j > 0 && dpc == 0) return err(JELLO_BC_BAD_FORMAT, "line table pcs not increasing", at);
      pc += dpc;
      line += dline;
      if(pc >= (int64_t)f->ninsns)
        return err(JELLO_BC_BAD_FORMAT, "line table pc out of range", at);
      if(file >= (int64_t)nfiles)
        return err(JELLO_BC_BAD_FORMAT, "line table file out of range", at);
      if(line < 1 || line > UINT32_MAX || col == 0)
        return err(JELLO_BC_BAD_FORMAT, "line table line or column out of range", at);
      l->pc = (uint32_t)pc;
      l->file = (uint32_t)file;
      l->line = (uint32_t)line;
      l->col = (uint32_t)col;
    }
  }
  return ok();
}

int jello_bc_source_location(const jello_bc_module* m, uint32_t fi, uint32_t pc, const char** file,
                             uint32_t* line, uint32_t* col) {
  if(!m || fi >= m->nfuncs) return 0;
  const jello_bc_function* f = &m->funcs[fi];
  if(f->nlocs == 0 || pc < f->locs[0].pc) return 0;
  /* The last run starting at or before pc. */
  uint32_t lo = 0, hi = f->nlocs - 1u;
  while(lo < hi) {
    uint32_t mid = lo + (hi - lo + 1u) / 2u;
    if(f->locs[mid].pc <= pc) lo = mid;
    else hi = mid - 1u;
  }
  *file = m->source_files[f->locs[lo].file];
  *line = f->locs[lo].line;
  *col = f->locs[lo].col;
  return 1;
}

uint32_t jello_bc_get_entry(const jello_bc_module* m) { return m ? m->entry : 0u; }

jello_bc_result jello_bc_read(const uint8_t* data, size_t size, jello_bc_module** out) {
  if(!out) return err(JELLO_BC_BAD_FORMAT, "out is NULL", 0);
  *out = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  if(!*out) return err(JELLO_BC_OUT_OF_MEMORY, "oom module", 0);
  jello_bc_module* m = *out;

  rd r = {data, size, 0};
  jello_bc_result rr;

  uint32_t magic = 0;
  if((rr = rd_u32(&r, &magic)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if(magic != JELLO_JLO_MAGIC) {
    jello_bc_free(m);
    *out = NULL;
    return err(JELLO_BC_BAD_MAGIC, "bad magic", r.off - 4);
  }

  if((rr = rd_u32(&r, &m->version)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if(m->version != JELLO_JLO_VERSION) {
    jello_bc_free(m);
    *out = NULL;
    return err(JELLO_BC_UNSUPPORTED_VERSION, "unsupported version", r.off - 4);
  }

  if((rr = rd_u32(&r, &m->features)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if((rr = rd_u32(&r, &m->ntypes)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if((rr = rd_u32(&r, &m->nsigs)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if((rr = rd_u32(&r, &m->nstruct_sigs)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if((rr = rd_u32(&r, &m->natoms)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if((rr = rd_u32(&r, &m->nfuncs)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if((rr = rd_u32(&r, &m->entry)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if(m->entry >= m->nfuncs) {
    jello_bc_free(m);
    *out = NULL;
    return err(JELLO_BC_BAD_FORMAT, "entry out of range", r.off - 4);
  }
  /* nimports is always present; a module without native imports stores 0. */
  if((rr = rd_u32(&r, &m->nimports)).err) {
    jello_bc_free(m);
    *out = NULL;
    return rr;
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA) {
    if((rr = rd_u32(&r, &m->nenum_sigs)).err) {
      jello_bc_free(m);
      *out = NULL;
      return rr;
    }
  }

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    if((rr = rd_u32(&r, &m->nconst_i64)).err) {
      jello_bc_free(m);
      *out = NULL;
      return rr;
    }
    if((rr = rd_u32(&r, &m->nconst_f64)).err) {
      jello_bc_free(m);
      *out = NULL;
      return rr;
    }
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_CONSTBYTES) {
    if((rr = rd_u32(&r, &m->nconst_bytes)).err) {
      jello_bc_free(m);
      *out = NULL;
      return rr;
    }
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_GLOBALS) {
    if((rr = rd_u32(&r, &m->nglobals)).err) {
      jello_bc_free(m);
      *out = NULL;
      return rr;
    }
  }

  if(m->ntypes > 0) {
    jello_type_entry* types = (jello_type_entry*)calloc(m->ntypes, sizeof(jello_type_entry));
    if(!types) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom types", r.off);
    }
    m->types = types;
    for(uint32_t i = 0; i < m->ntypes; i++) {
      uint8_t kind_u8 = 0, pad1 = 0;
      uint16_t pad2 = 0;
      uint32_t p0 = 0, p1 = 0;
      if((rr = rd_u8(&r, &kind_u8)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u8(&r, &pad1)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u16(&r, &pad2)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u32(&r, &p0)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u32(&r, &p1)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      (void)pad1;
      (void)pad2;
      (void)p1;
      if(!jello_bc_type_kind_is_valid(kind_u8)) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "invalid type kind", r.off - 12);
      }
      types[i].kind = (jello_type_kind)kind_u8;
      switch(types[i].kind) {
        case JELLO_T_LIST:
        case JELLO_T_ARRAY:
          if(p0 >= m->ntypes) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "elem type out of range", r.off - 8);
          }
          types[i].as.unary.elem = (jello_type_id)p0;
          break;
        case JELLO_T_FUNCTION:
          if(p0 >= m->nsigs) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "sig id out of range", r.off - 8);
          }
          types[i].as.fun.sig_id = p0;
          break;
        case JELLO_T_STRUCT:
          if(p0 >= m->nstruct_sigs) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "struct sig id out of range", r.off - 8);
          }
          types[i].as.strct.struct_sig_id = p0;
          break;
        case JELLO_T_ABSTRACT:
          types[i].as.abs.abstract_id = p0; /* No table to check an abstract id against. */
          break;
        case JELLO_T_ENUM:
          if(m->features & (uint32_t)JELLO_BC_FEAT_ENUM_SCHEMA) {
            if(p0 >= m->nenum_sigs) {
              jello_bc_free(m);
              *out = NULL;
              return err(JELLO_BC_BAD_FORMAT, "enum sig id out of range", r.off - 8);
            }
            types[i].as.enm.enum_sig_id = p0;
          }
          break;
        default: break;
      }
    }
  }

  if(m->nsigs > 0) {
    jello_fun_sig* sigs = (jello_fun_sig*)calloc(m->nsigs, sizeof(jello_fun_sig));
    if(!sigs) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom sigs", r.off);
    }
    m->sigs = sigs;
    for(uint32_t i = 0; i < m->nsigs; i++) {
      uint32_t ret = 0;
      uint16_t nargs = 0;
      if((rr = rd_u32(&r, &ret)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(ret >= m->ntypes) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "sig ret type out of range", r.off - 4);
      }
      if((rr = rd_u16(&r, &nargs)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      sigs[i].ret = (jello_type_id)ret;
      sigs[i].nargs = nargs;
      if(nargs > 0) {
        jello_type_id* args = (jello_type_id*)calloc(nargs, sizeof(jello_type_id));
        if(!args) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom sig args", r.off);
        }
        sigs[i].args = args;
        for(uint16_t j = 0; j < nargs; j++) {
          uint32_t at = 0;
          if((rr = rd_u32(&r, &at)).err) {
            jello_bc_free(m);
            *out = NULL;
            return rr;
          }
          if(at >= m->ntypes) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "sig arg type out of range", r.off - 4);
          }
          ((jello_type_id*)args)[j] = (jello_type_id)at;
        }
      }
    }
  }

  if(m->nstruct_sigs > 0) {
    jello_struct_sig* sigs = (jello_struct_sig*)calloc(m->nstruct_sigs, sizeof(jello_struct_sig));
    if(!sigs) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom struct_sigs", r.off);
    }
    m->struct_sigs = sigs;
    for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
      uint16_t nfields = 0;
      uint8_t named = 0, is_mutable = 0;
      if((rr = rd_u16(&r, &nfields)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u8(&r, &named)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u8(&r, &is_mutable)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      sigs[i].nfields = nfields;
      sigs[i].named = named;
      sigs[i].is_mutable = is_mutable;
      if(nfields > 0) {
        uint32_t* field_type = (uint32_t*)calloc(nfields, sizeof(uint32_t));
        if(!field_type) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom struct field_type", r.off);
        }
        sigs[i].field_type = field_type;
        for(uint16_t j = 0; j < nfields; j++) {
          uint32_t ft = 0;
          if((rr = rd_u32(&r, &ft)).err) {
            jello_bc_free(m);
            *out = NULL;
            return rr;
          }
          if(ft >= m->ntypes) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "struct field type out of range", r.off - 4);
          }
          ((uint32_t*)field_type)[j] = ft;
        }
        /* field_offset is not in the file; each field follows the previous one, aligned to its
         * jello_slot_size. */
        uint32_t* field_offset = (uint32_t*)calloc(nfields, sizeof(uint32_t));
        if(!field_offset) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom struct field_offset", r.off);
        }
        uint32_t cursor = 0;
        for(uint16_t j = 0; j < nfields; j++) {
          jello_type_kind fk = m->types[field_type[j]].kind;
          uint32_t sz = (uint32_t)jello_slot_size(fk);
          cursor = align_up(cursor, sz);
          field_offset[j] = cursor;
          cursor += sz;
        }
        sigs[i].field_offset = field_offset;
        if(named) {
          uint32_t* field_name_atom = (uint32_t*)calloc(nfields, sizeof(uint32_t));
          if(!field_name_atom) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_OUT_OF_MEMORY, "oom struct field_name_atom", r.off);
          }
          sigs[i].field_name_atom = field_name_atom;
          for(uint16_t j = 0; j < nfields; j++) {
            uint32_t fa = 0;
            if((rr = rd_u32(&r, &fa)).err) {
              jello_bc_free(m);
              *out = NULL;
              return rr;
            }
            if(fa >= m->natoms) {
              jello_bc_free(m);
              *out = NULL;
              return err(JELLO_BC_BAD_FORMAT, "struct field name atom out of range", r.off - 4);
            }
            for(uint16_t k = 0; k < j; k++) {
              if(((uint32_t*)field_name_atom)[k] == fa) {
                jello_bc_free(m);
                *out = NULL;
                return err(JELLO_BC_BAD_FORMAT, "duplicate struct field name", r.off - 4);
              }
            }
            ((uint32_t*)field_name_atom)[j] = fa;
          }
        }
      }
    }
  }

  if(m->nenum_sigs > 0) {
    jello_enum_sig* sigs = (jello_enum_sig*)calloc(m->nenum_sigs, sizeof(jello_enum_sig));
    if(!sigs) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom enum_sigs", r.off);
    }
    m->enum_sigs = sigs;
    for(uint32_t i = 0; i < m->nenum_sigs; i++) {
      uint16_t nvariants = 0;
      if((rr = rd_u16(&r, &nvariants)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      sigs[i].nvariants = nvariants;
      jello_enum_variant_sig* variants =
          nvariants ? (jello_enum_variant_sig*)calloc(nvariants, sizeof(*variants)) : NULL;
      if(nvariants && !variants) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_OUT_OF_MEMORY, "oom enum variants", r.off);
      }
      sigs[i].variants = variants;
      for(uint16_t j = 0; j < nvariants; j++) {
        uint16_t nfields = 0;
        if((rr = rd_u16(&r, &nfields)).err) {
          jello_bc_free(m);
          *out = NULL;
          return rr;
        }
        variants[j].nfields = nfields;
        jello_type_id* fields = nfields ? (jello_type_id*)calloc(nfields, sizeof(*fields)) : NULL;
        if(nfields && !fields) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom enum fields", r.off);
        }
        variants[j].field_type = fields;
        for(uint16_t k = 0; k < nfields; k++) {
          uint32_t tid = 0;
          if((rr = rd_u32(&r, &tid)).err) {
            jello_bc_free(m);
            *out = NULL;
            return rr;
          }
          if(tid >= m->ntypes) {
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "enum field type out of range", r.off - 4);
          }
          fields[k] = tid;
        }
      }
    }

    /* A slot for each variant of each enum type, which ENUM_NEW fills with the one value of a
     * variant without fields. */
    uint32_t* base = (uint32_t*)calloc(m->ntypes ? m->ntypes : 1, sizeof(uint32_t));
    if(!base) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom const_enum_base", r.off);
    }
    m->const_enum_base = base;
    uint64_t slots = 0;
    for(uint32_t t = 0; t < m->ntypes; t++) {
      if(m->types[t].kind != JELLO_T_ENUM) continue;
      base[t] = (uint32_t)slots;
      slots += sigs[m->types[t].as.enm.enum_sig_id].nvariants;
    }
    if(slots > UINT32_MAX) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_BAD_FORMAT, "too many enum variants", r.off);
    }
    m->nconst_enums = (uint32_t)slots;
    m->const_enums =
        (_Atomic(struct jello_enum*)*)calloc(slots ? slots : 1, sizeof(*m->const_enums));
    if(!m->const_enums) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom const_enums", r.off);
    }
  }

  /* Atoms are one blob of NUL-terminated strings (atoms_data) plus pointers into it (atoms).
   * Offsets stand in for the pointers until the blob stops growing, since realloc may move it. */
  if(m->natoms > 0) {
    uint32_t* atom_offsets = (uint32_t*)calloc(m->natoms, sizeof(uint32_t));
    if(!atom_offsets) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom atom offsets", r.off);
    }
    size_t cap = 256, used = 0;
    char* blob = (char*)malloc(cap);
    if(!blob) {
      free(atom_offsets);
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom atoms_data", r.off);
    }
    for(uint32_t i = 0; i < m->natoms; i++) {
      uint32_t len = 0;
      if((rr = rd_u32(&r, &len)).err) {
        free(blob);
        free(atom_offsets);
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      size_t need = (size_t)len + 1u;
      if(used + need > cap) {
        while(used + need > cap) cap *= 2;
        char* grown = (char*)realloc(blob, cap);
        if(!grown) {
          free(blob);
          free(atom_offsets);
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom atoms_data grow", r.off);
        }
        blob = grown;
      }
      if((rr = rd_bytes(&r, (uint8_t*)(blob + used), len)).err) {
        free(blob);
        free(atom_offsets);
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      blob[used + len] = '\0';
      atom_offsets[i] = (uint32_t)used;
      used += need;
    }
    m->atoms_data = blob;
    const char** atoms = (const char**)calloc(m->natoms, sizeof(const char*));
    if(!atoms) {
      free(atom_offsets);
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom atoms", r.off);
    }
    for(uint32_t i = 0; i < m->natoms; i++) atoms[i] = m->atoms_data + atom_offsets[i];
    m->atoms = atoms;
    free(atom_offsets);
  }

  /* Each import names a `.jdll` and one of its exports by atom. The library is loaded here and the
   * module rejected unless the export's signature matches the declared one exactly; check.c
   * then checks CALL_NATIVE instructions against the declarations. */
  if(m->nimports > 0) {
    jello_import_sig* imports = (jello_import_sig*)calloc(m->nimports, sizeof(jello_import_sig));
    jello_jdll_handle** handles =
        (jello_jdll_handle**)calloc(m->nimports, sizeof(jello_jdll_handle*));
    if(!imports || !handles) {
      free(imports);
      free(handles);
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom imports", r.off);
    }
    m->imports = imports;
    m->jdll_handles = handles;
    for(uint32_t i = 0; i < m->nimports; i++) {
      uint32_t lib_atom = 0, exp_atom = 0;
      uint8_t ret_kind = 0, arity = 0;
      if((rr = rd_u32(&r, &lib_atom)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(lib_atom >= m->natoms) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "import library atom out of range", r.off - 4);
      }
      if((rr = rd_u32(&r, &exp_atom)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(exp_atom >= m->natoms) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "import export atom out of range", r.off - 4);
      }
      if((rr = rd_u8(&r, &ret_kind)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(!jello_bc_type_kind_is_valid(ret_kind)) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "import ret kind invalid", r.off - 1);
      }
      if((rr = rd_u8(&r, &arity)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(arity > 8) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "import arity exceeds 8 (v1 ceiling)", r.off - 1);
      }
      ((jello_import_sig*)imports)[i].library_atom = lib_atom;
      ((jello_import_sig*)imports)[i].export_atom = exp_atom;
      ((jello_import_sig*)imports)[i].ret_kind = ret_kind;
      ((jello_import_sig*)imports)[i].arity = arity;
      for(uint8_t j = 0; j < 8; j++) {
        uint8_t ak = 0;
        if((rr = rd_u8(&r, &ak)).err) {
          jello_bc_free(m);
          *out = NULL;
          return rr;
        }
        if(j < arity && !jello_bc_type_kind_is_valid(ak)) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_BAD_FORMAT, "import arg kind invalid", r.off - 1);
        }
        ((jello_import_sig*)imports)[i].arg_kinds[j] = ak;
      }

      const char* library_name = m->atoms[lib_atom];
      const char* export_name = m->atoms[exp_atom];
      jello_jdll_handle* h = jello_jdll_load(library_name);
      if(!h) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, jello_jdll_last_error(), r.off);
      }
      handles[i] = h;
      m->njdll_handles = i + 1u;

      const jello_jdll_export* exp = jello_jdll_find_export(h, export_name);
      if(!exp) {
        snprintf(g_validate_err, sizeof g_validate_err, "%s.jdll exports no function %s",
                 library_name, export_name);
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, g_validate_err, r.off);
      }
      int sig_matches = (exp->ret_kind == ret_kind) && (exp->arity == arity);
      for(uint8_t j = 0; sig_matches && j < arity; j++)
        sig_matches = (exp->arg_kinds[j] == imports[i].arg_kinds[j]);
      if(!sig_matches) {
        snprintf(g_validate_err, sizeof g_validate_err, "native %s from %s is bound as ",
                 export_name, library_name);
        append_signature(g_validate_err, sizeof g_validate_err, ret_kind, arity,
                         imports[i].arg_kinds);
        append_text(g_validate_err, sizeof g_validate_err, ", but the library exports ");
        append_signature(g_validate_err, sizeof g_validate_err, exp->ret_kind, exp->arity,
                         exp->arg_kinds);
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, g_validate_err, r.off);
      }
      ((jello_import_sig*)imports)[i].resolved = exp;
    }
  }

  /* Prototype lookup applies only when atom 0 (JELLO_ATOM___PROTO__) is "__proto__";
   * proto_enabled is derived here, not stored in the file. */
  m->proto_enabled = (uint8_t)(m->natoms > JELLO_ATOM___PROTO__ &&
                               strcmp(m->atoms[JELLO_ATOM___PROTO__], "__proto__") == 0);

  if(m->nconst_i64 > 0) {
    int64_t* pool = (int64_t*)calloc(m->nconst_i64, sizeof(int64_t));
    if(!pool) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom const_i64", r.off);
    }
    m->const_i64 = pool;
    for(uint32_t i = 0; i < m->nconst_i64; i++) {
      uint32_t lo = 0, hi = 0;
      if((rr = rd_u32(&r, &lo)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u32(&r, &hi)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      ((int64_t*)pool)[i] = (int64_t)(((uint64_t)hi << 32) | (uint64_t)lo);
    }
  }
  if(m->nconst_f64 > 0) {
    double* pool = (double*)calloc(m->nconst_f64, sizeof(double));
    if(!pool) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom const_f64", r.off);
    }
    m->const_f64 = pool;
    for(uint32_t i = 0; i < m->nconst_f64; i++) {
      uint32_t lo = 0, hi = 0;
      if((rr = rd_u32(&r, &lo)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if((rr = rd_u32(&r, &hi)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
      memcpy(&((double*)pool)[i], &bits, sizeof(double));
    }
  }
  if(m->nconst_bytes > 0) {
    /* The values follow their table in one block: a first pass sizes it, a second fills it. */
    size_t table = sizeof(jello_bytes*) * m->nconst_bytes, total = table;
    rd scan = r;
    for(uint32_t i = 0; i < m->nconst_bytes; i++) {
      uint32_t len = 0;
      if((rr = rd_u32(&scan, &len)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(!rd_need(&scan, len)) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_EOF, "truncated const_bytes", scan.off);
      }
      scan.off += len;
      total += vm_bytes_immortal_size(len);
    }
    uint8_t* block = (uint8_t*)calloc(1, total);
    if(!block) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom const_bytes", r.off);
    }
    jello_bytes** values = (jello_bytes**)block;
    m->const_bytes_values = values;
    size_t at = 0;
    for(uint32_t i = 0; i < m->nconst_bytes; i++) {
      uint32_t len = 0;
      if((rr = rd_u32(&r, &len)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      values[i] = vm_bytes_immortal_at(block + table + at, len);
      if((rr = rd_bytes(&r, values[i]->storage, len)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      at += vm_bytes_immortal_size(len);
    }
  }

  // Globals start zeroed, and a Dynamic one as null.
  if(m->nglobals > 0) {
    if(!rd_need(&r, (size_t)m->nglobals * 4u)) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_EOF, "truncated globals", r.off);
    }
    jello_type_id* types = (jello_type_id*)calloc(m->nglobals, sizeof(jello_type_id));
    uint64_t* slots = (uint64_t*)calloc(m->nglobals, sizeof(uint64_t));
    uint32_t* ptrs = (uint32_t*)malloc(sizeof(uint32_t) * m->nglobals);
    m->global_types = types;
    m->globals = slots;
    m->ptr_globals = ptrs;
    if(!types || !slots || !ptrs) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom globals", r.off);
    }
    for(uint32_t i = 0; i < m->nglobals; i++) {
      uint32_t t = 0;
      if((rr = rd_u32(&r, &t)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(t >= m->ntypes) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "global type out of range", r.off - 4);
      }
      types[i] = t;
      if(m->types[t].kind == JELLO_T_DYNAMIC) slots[i] = (uint64_t)jello_make_null();
      if(vm_kind_needs_gc_scan(m->types[t].kind)) ptrs[m->nptr_globals++] = i;
    }
  }

  if(m->nfuncs > 0) {
    jello_bc_function* funcs = (jello_bc_function*)calloc(m->nfuncs, sizeof(jello_bc_function));
    m->funcs = funcs;
    m->const_funs = (_Atomic(struct jello_function*)*)calloc(m->nfuncs, sizeof(*m->const_funs));
    if(!funcs || !m->const_funs) {
      jello_bc_free(m);
      *out = NULL;
      return err(JELLO_BC_OUT_OF_MEMORY, "oom funcs", r.off);
    }
    for(uint32_t i = 0; i < m->nfuncs; i++) {
      uint32_t name_atom = JELLO_BC_NO_NAME, nregs = 0, cap_start = 0, ninsns = 0;
      /* The loader reads only the current format, so a change to it means rebuilding every
       * `.jlo`, the committed seed included. */
      if((rr = rd_u32(&r, &name_atom)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(name_atom != JELLO_BC_NO_NAME && name_atom >= m->natoms) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "function name atom out of range", r.off - 4);
      }
      if((rr = rd_u32(&r, &nregs)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(nregs > 256) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "nregs > 256", r.off - 4);
      }
      if((rr = rd_u32(&r, &cap_start)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      uint32_t nargs = nregs;
      if((rr = rd_u32(&r, &nargs)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }
      if(nargs > nregs) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "nargs > nregs", r.off - 4);
      }
      if((rr = rd_u32(&r, &ninsns)).err) {
        jello_bc_free(m);
        *out = NULL;
        return rr;
      }

      jello_type_id* reg_types = NULL;
      if(nregs > 0) {
        reg_types = (jello_type_id*)calloc(nregs, sizeof(jello_type_id));
        if(!reg_types) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom reg_types", r.off);
        }
        for(uint32_t j = 0; j < nregs; j++) {
          uint32_t tid = 0;
          if((rr = rd_u32(&r, &tid)).err) {
            free(reg_types);
            jello_bc_free(m);
            *out = NULL;
            return rr;
          }
          if(tid >= m->ntypes) {
            free(reg_types);
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "reg type out of range", r.off - 4);
          }
          ((jello_type_id*)reg_types)[j] = (jello_type_id)tid;
        }
      }

      jello_insn* insns = NULL;
      if(ninsns > 0) {
        insns = (jello_insn*)calloc(ninsns, sizeof(jello_insn));
        if(!insns) {
          free(reg_types);
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_OUT_OF_MEMORY, "oom insns", r.off);
        }
        int compressed = (m->features & (uint32_t)JELLO_BC_FEAT_COMPRESSED_INSN) != 0;
        for(uint32_t j = 0; j < ninsns; j++) {
          rr = compressed ? read_insn_compressed(&r, &((jello_insn*)insns)[j])
                          : read_insn_canonical(&r, &((jello_insn*)insns)[j]);
          if(rr.err) {
            free(insns);
            free(reg_types);
            jello_bc_free(m);
            *out = NULL;
            return rr;
          }
          if(!jello_bc_op_is_valid(insns[j].op)) {
            free(insns);
            free(reg_types);
            jello_bc_free(m);
            *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "invalid opcode", r.off);
          }
        }
      }

      funcs[i].name_atom = name_atom;
      funcs[i].nargs = nargs;
      funcs[i].nregs = nregs;
      funcs[i].cap_start = cap_start;
      funcs[i].reg_types = reg_types;
      funcs[i].ninsns = ninsns;
      funcs[i].insns = insns;
    }

    /* Validation waits until every function is read; CALL checks its arguments against the
     * callee's reg_types, and the callee may come later in the table. JELLO_BC_SKIP_VALIDATE
     * skips it. */
    if(getenv("JELLO_BC_SKIP_VALIDATE") == NULL) {
      for(uint32_t i = 0; i < m->nfuncs; i++) {
        const jello_bc_function* f = &funcs[i];
        for(uint32_t j = 0; j < f->ninsns; j++) {
          jello_bc_result vr = validate_insn_ctx(m, f, i, j);
          if(vr.err) {
            jello_bc_free(m);
            *out = NULL;
            return vr;
          }
          if(f->insns[j].op == (uint8_t)JOP_SWITCH_KIND) j += f->insns[j].b;
        }

        /* A function must end in RET, JMP, THROW or a tail call, or the interpreter's pc would
         * run past `insns`. */
        if(f->ninsns == 0 || (f->insns[f->ninsns - 1u].op != (uint8_t)JOP_RET &&
                              f->insns[f->ninsns - 1u].op != (uint8_t)JOP_JMP &&
                              f->insns[f->ninsns - 1u].op != (uint8_t)JOP_THROW &&
                              f->insns[f->ninsns - 1u].op != (uint8_t)JOP_TAILCALL &&
                              f->insns[f->ninsns - 1u].op != (uint8_t)JOP_TAILCALLR)) {
          jello_bc_free(m);
          *out = NULL;
          return err(JELLO_BC_BAD_FORMAT, "function does not end in a terminator", r.off);
        }
      }
      {
        jello_bc_result vr = jello_bc_validate_results(m);
        if(vr.err) {
          jello_bc_free(m);
          *out = NULL;
          return vr;
        }
        vr = jello_bc_validate_try_regions(m);
        if(vr.err) {
          jello_bc_free(m);
          *out = NULL;
          return vr;
        }
      }
    }
  }

  if(m->features & (uint32_t)JELLO_BC_FEAT_LINE_TABLE) {
    rr = read_line_table(&r, m);
    if(rr.err) {
      jello_bc_free(m);
      *out = NULL;
      return rr;
    }
  }

  /* Nothing may follow the functions or the line table. */
  if(r.off != r.n) {
    jello_bc_free(m);
    *out = NULL;
    return err(JELLO_BC_BAD_FORMAT, "trailing bytes after functions", r.off);
  }

  return ok();
}
