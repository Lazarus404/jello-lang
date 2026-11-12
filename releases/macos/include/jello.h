// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The embedding API; load a `.jlo` module, run it on a VM, and build or inspect the values and
 * heap objects it exchanges with C. Native-library authors also include jello/jdll.h. */

#ifndef JELLO_H
#define JELLO_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t jello_type_id;
typedef enum jello_type_kind {
  JELLO_T_VOID = 1,
  JELLO_T_BOOL = 2,
  JELLO_T_ATOM = 3,
  JELLO_T_I8 = 4,
  JELLO_T_I16 = 5,
  JELLO_T_I32 = 6,
  JELLO_T_I64 = 7,
  JELLO_T_F16 = 8,
  JELLO_T_F32 = 9,
  JELLO_T_F64 = 10,
  JELLO_T_BYTES = 11,
  JELLO_T_LIST = 12,
  JELLO_T_ARRAY = 13,
  JELLO_T_STRUCT = 14,
  JELLO_T_OBJECT = 15,
  JELLO_T_FUNCTION = 16,
  JELLO_T_ABSTRACT = 17,
  JELLO_T_DYNAMIC = 18,
  JELLO_T_ENUM = 19,
} jello_type_kind;

#define JELLO_TYPE_KIND_COUNT 19

size_t jello_slot_size(jello_type_kind k);

typedef struct jello_fun_sig {
  jello_type_id ret;
  uint16_t nargs;
  const jello_type_id* args;
} jello_fun_sig;

typedef struct jello_struct_sig {
  uint16_t nfields;
  uint8_t named;
  uint8_t is_mutable;
  const uint32_t* field_type;
  const uint32_t* field_name_atom; /* NULL when named == 0 */
  const uint32_t* field_offset;
} jello_struct_sig;

typedef struct jello_enum_variant_sig {
  uint16_t nfields;
  const jello_type_id* field_type;
} jello_enum_variant_sig;

typedef struct jello_enum_sig {
  uint16_t nvariants;
  const jello_enum_variant_sig* variants;
} jello_enum_sig;

typedef struct jello_type_entry {
  jello_type_kind kind;
  union {
    struct { jello_type_id elem; } unary;          /* LIST, ARRAY */
    struct { uint32_t sig_id; } fun;               /* FUNCTION */
    struct { uint32_t struct_sig_id; } strct;      /* STRUCT */
    struct {
      uint32_t enum_sig_id;
    } enm; /**< ENUM, with ENUM_SCHEMA: its variants. */
    struct { uint32_t abstract_id; } abs;          /* ABSTRACT */
  } as;
} jello_type_entry;

struct jello_bc_module;
typedef struct jello_bc_module jello_bc_module;

typedef enum jello_bc_error {
  JELLO_BC_OK = 0,
  JELLO_BC_EOF,
  JELLO_BC_BAD_MAGIC,
  JELLO_BC_UNSUPPORTED_VERSION,
  JELLO_BC_BAD_FORMAT,
  JELLO_BC_OUT_OF_MEMORY,
} jello_bc_error;

typedef struct jello_bc_result {
  jello_bc_error err;
  const char* msg;
  size_t offset;
} jello_bc_result;

jello_bc_result jello_bc_read(const uint8_t* data, size_t size, jello_bc_module** out);
void jello_bc_free(jello_bc_module* m);
uint32_t jello_bc_get_entry(const jello_bc_module* m);

struct jello_vm;
typedef struct jello_vm jello_vm;

jello_vm* jello_vm_create(void);
void jello_vm_destroy(jello_vm* vm);

typedef enum jello_exec_status {
  JELLO_EXEC_OK = 0,
  JELLO_EXEC_TRAP = 1,
} jello_exec_status;

typedef enum jello_trap_code {
  JELLO_TRAP_NONE = 0,
  JELLO_TRAP_ARITHMETIC = 1,
  JELLO_TRAP_STACK_OVERFLOW = 2,
  JELLO_TRAP_NULL_DEREF = 3,
  JELLO_TRAP_BOUNDS = 4,
  JELLO_TRAP_TYPE_MISMATCH = 5,
  JELLO_TRAP_FUEL = 6,
  JELLO_TRAP_LIMIT = 7,
  JELLO_TRAP_UNCAUGHT_THROW = 8,
  JELLO_TRAP_STOPPED = 9,        /**< The process was asked to stop. */
  JELLO_TRAP_DEADLOCK = 10,      /**< The process waited for what no process could ever do. */
  JELLO_TRAP_UNSENDABLE = 11,
  JELLO_TRAP_SEALED = 12,        /**< A write would change a sealed method (jello_object_seal). */
} jello_trap_code;

jello_trap_code jello_vm_last_trap_code(const jello_vm* vm);
const char* jello_trap_code_name(jello_trap_code tc);
void jello_vm_set_fuel(jello_vm* vm, uint64_t fuel);
void jello_vm_set_max_bytes_len(jello_vm* vm, uint32_t max_len);
void jello_vm_set_program_args(jello_vm* vm, int32_t argc, const char* const* argv);
void jello_vm_set_image(jello_vm* vm, const uint8_t* data, size_t size);
void jello_vm_keep_processes(jello_vm* vm, int keep);
int jello_run_for(jello_vm* vm, uint32_t ms);
uint32_t jello_vm_last_trap_pc(const jello_vm* vm);
uint32_t jello_vm_last_trap_func_index(const jello_vm* vm);
#define JELLO_TRAP_CALLERS_MAX 16u
uint32_t jello_vm_last_trap_callers(const jello_vm* vm, uint32_t* func_indexes, uint32_t* pcs,
                                    uint8_t* repeated, uint32_t max);

typedef uintptr_t jello_value;

typedef enum jello_value_tag {
  JELLO_TAG_PTR = 0x0u,
  JELLO_TAG_I32 = 0x1u,
  JELLO_TAG_ATOM = 0x2u,
  JELLO_TAG_BOOL = 0x3u,
  JELLO_TAG_NULL = 0x4u,
} jello_value_tag;

static inline jello_value jello_make_null(void) {
  return (jello_value)JELLO_TAG_NULL;
}
static inline int jello_is_null(jello_value v) {
  return (unsigned)(v & 0x7u) == (unsigned)JELLO_TAG_NULL;
}
static inline jello_value jello_make_bool(int b) {
  return ((jello_value)((b != 0) ? 1u : 0u) << 3) | (jello_value)JELLO_TAG_BOOL;
}
static inline int jello_is_bool(jello_value v) {
  return (unsigned)(v & 0x7u) == (unsigned)JELLO_TAG_BOOL;
}
static inline int jello_as_bool(jello_value v) {
  return (int)((v >> 3) & 1u);
}
static inline jello_value jello_make_atom(uint32_t atom_id) {
  return ((jello_value)atom_id << 3) | (jello_value)JELLO_TAG_ATOM;
}
static inline int jello_is_atom(jello_value v) {
  return (unsigned)(v & 0x7u) == (unsigned)JELLO_TAG_ATOM;
}
static inline uint32_t jello_as_atom(jello_value v) {
  return (uint32_t)(v >> 3);
}
static inline jello_value jello_make_i32(int32_t x) {
  return ((jello_value)(uintptr_t)(uint32_t)x << 3) | (jello_value)JELLO_TAG_I32;
}
static inline int jello_is_i32(jello_value v) {
  return (unsigned)(v & 0x7u) == (unsigned)JELLO_TAG_I32;
}
static inline int32_t jello_as_i32(jello_value v) {
  return (int32_t)(uint32_t)(v >> 3);
}
static inline int jello_is_ptr(jello_value v) {
  return (unsigned)(v & 0x7u) == (unsigned)JELLO_TAG_PTR;
}
static inline void* jello_as_ptr(jello_value v) {
  return (void*)(uintptr_t)v;
}
static inline jello_value jello_from_ptr(const void* p) {
  return (jello_value)(uintptr_t)p;
}

jello_value jello_vm_last_thrown(const jello_vm* vm);

const char* jello_vm_last_trap_detail(const jello_vm* vm);

typedef enum jello_numeric_kind {
  JELLO_NUM_VOID = 0,
  JELLO_NUM_BOOL,
  JELLO_NUM_I32,
  JELLO_NUM_I64,
  JELLO_NUM_F32,
  JELLO_NUM_F64,
  JELLO_NUM_PTR,
  JELLO_NUM_DYN,
} jello_numeric_kind;

typedef struct jello_numeric_result {
  jello_numeric_kind kind;
  union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    int b; /* 0/1, for JELLO_NUM_BOOL */
    void* ptr; /* JELLO_NUM_PTR -- Bytes/Struct/Object, NULL is legal */
    jello_value dyn; /* JELLO_NUM_DYN -- the raw tagged word */
  } as;
} jello_numeric_result;

jello_exec_status jello_vm_exec_numeric(jello_vm* vm, const jello_bc_module* m, jello_numeric_result* out);

typedef struct jello_jit_module jello_jit_module;

jello_jit_module* jello_jit_module_create(const jello_bc_module* m);
void jello_jit_module_destroy(jello_jit_module* jm);
int jello_jit_available(void);

jello_exec_status jello_vm_exec_numeric_with_jit(jello_vm* vm, jello_jit_module* jm,
    const jello_bc_module* m, jello_numeric_result* out);

void jello_gc_init(jello_vm* vm);
void jello_gc_shutdown(jello_vm* vm);
void* jello_gc_alloc(jello_vm* vm, size_t size);
void jello_gc_collect(jello_vm* vm);
void jello_gc_push_root(jello_vm* vm, void* obj);
void jello_gc_pop_roots(jello_vm* vm, uint32_t n);
void jello_vm_gc_stats(const jello_vm* vm, uint64_t* out_live_bytes, uint32_t* out_collections,
                       double* out_pause_ms);

typedef enum jello_obj_kind {
  JELLO_OBJ_BYTES = 1,
  JELLO_OBJ_STRUCT = 2,
  JELLO_OBJ_OBJECT = 3,
  JELLO_OBJ_BOX_I64 = 4,
  JELLO_OBJ_BOX_F64 = 5,
  JELLO_OBJ_BOX_F32 = 6,
  JELLO_OBJ_BOX_F16 = 7,
  JELLO_OBJ_ARRAY = 8,
  JELLO_OBJ_FUNCTION = 9,
  JELLO_OBJ_LIST = 10,
  JELLO_OBJ_ENUM = 11,
  JELLO_OBJ_HANDLE = 12, /**< A native library's object, reachable only through Dynamic. */
} jello_obj_kind;

typedef struct jello_obj_header {
  uint8_t kind;
  uint32_t type_id;
} jello_obj_header;

typedef struct jello_bytes {
  jello_obj_header h;
  uint32_t length;
  uint32_t shared; /**< 1 when `owner` is a shared binary it holds a reference to, else 0. */
  uint8_t* data;
  struct jello_bytes* owner;
  uint8_t storage[];
} jello_bytes;

jello_bytes* jello_bytes_new(jello_vm* vm, uint32_t length);
jello_bytes* jello_bytes_new_zeroed(jello_vm* vm, uint32_t length);
jello_bytes* jello_bytes_slice(jello_vm* vm, jello_bytes* src, uint32_t start, uint32_t length);

/* -- numeric boxing -- */
typedef struct jello_box_i64 { jello_obj_header h; int64_t value; } jello_box_i64;
typedef struct jello_box_f64 { jello_obj_header h; double value; } jello_box_f64;
typedef struct jello_box_f32 { jello_obj_header h; float value; } jello_box_f32;
typedef struct jello_box_f16 { jello_obj_header h; uint16_t value; } jello_box_f16;

jello_box_i64* jello_box_i64_new(jello_vm* vm, int64_t v);
jello_box_f64* jello_box_f64_new(jello_vm* vm, double v);
jello_box_f32* jello_box_f32_new(jello_vm* vm, float v);
jello_box_f16* jello_box_f16_new(jello_vm* vm, uint16_t bits);

static inline int jello_is_box_i64(jello_value v) {
  return jello_is_ptr(v) && v != 0 && ((const jello_obj_header*)jello_as_ptr(v))->kind == (uint8_t)JELLO_OBJ_BOX_I64;
}
static inline int jello_is_box_f64(jello_value v) {
  return jello_is_ptr(v) && v != 0 && ((const jello_obj_header*)jello_as_ptr(v))->kind == (uint8_t)JELLO_OBJ_BOX_F64;
}
static inline int jello_is_box_f32(jello_value v) {
  return jello_is_ptr(v) && v != 0 && ((const jello_obj_header*)jello_as_ptr(v))->kind == (uint8_t)JELLO_OBJ_BOX_F32;
}
static inline int jello_is_box_f16(jello_value v) {
  return jello_is_ptr(v) && v != 0 && ((const jello_obj_header*)jello_as_ptr(v))->kind == (uint8_t)JELLO_OBJ_BOX_F16;
}
static inline int64_t jello_as_box_i64(jello_value v) { return ((const jello_box_i64*)jello_as_ptr(v))->value; }
static inline double jello_as_box_f64(jello_value v) { return ((const jello_box_f64*)jello_as_ptr(v))->value; }
static inline float jello_as_box_f32(jello_value v) { return ((const jello_box_f32*)jello_as_ptr(v))->value; }
static inline uint16_t jello_as_box_f16(jello_value v) { return ((const jello_box_f16*)jello_as_ptr(v))->value; }

typedef struct jello_struct {
  jello_obj_header h;
  uint8_t fields[];
} jello_struct;

jello_struct* jello_struct_new(jello_vm* vm, const jello_bc_module* m, uint32_t struct_sig_id);

typedef struct jello_object {
  jello_obj_header h;
  struct jello_object* proto;
  uint32_t cap;
  uint32_t len;
  uint32_t tombstones; /**< Removed slots; they count towards the load factor so a probe always
                        * ends at an empty slot. */
  uint32_t sealed;     /**< Sealed slots (jello_object_seal). */
  uint32_t* keys;
  jello_value* vals;
  uint8_t* states;
} jello_object;

typedef struct jello_array {
  jello_obj_header h;
  uint32_t length;
  uint32_t cap;
  void* data;
} jello_array;

jello_array* jello_array_new(jello_vm* vm, const jello_bc_module* m, uint32_t array_type_id, uint32_t length);
void jello_array_resize(jello_vm* vm, const jello_bc_module* m, jello_array* a, uint32_t new_length);

typedef struct jello_list {
  jello_obj_header h;
  jello_value head;
  struct jello_list* tail;
} jello_list;

jello_list* jello_list_cons(jello_vm* vm, uint32_t type_id, jello_value head, jello_list* tail);

typedef struct jello_enum {
  jello_obj_header h;
  uint32_t tag;
  uint32_t nfields;
  jello_value fields[];
} jello_enum;

jello_enum* jello_enum_new(jello_vm* vm, uint32_t type_id, uint32_t tag, uint32_t nfields,
                           const jello_value* fields);

typedef struct jello_function {
  jello_obj_header h;
  uint32_t func_index;
  uint32_t ncaps;
  uint64_t captures[];
} jello_function;

jello_function* jello_function_new(jello_vm* vm, uint32_t func_index, uint32_t ncaps);
jello_object* jello_object_new(jello_vm* vm, uint32_t type_id);
int jello_object_has(const jello_object* o, uint32_t atom_id);
jello_value jello_object_get(const jello_object* o, uint32_t atom_id);
void jello_object_set(jello_object* o, uint32_t atom_id, jello_value v);
void jello_object_seal(jello_object* o, uint32_t atom_id, jello_value v);
int jello_object_remove(jello_object* o, uint32_t atom_id);
void jello_object_clear(jello_object* o);
jello_object* jello_object_copy(jello_vm* vm, const jello_object* src);
uint32_t jello_object_keys(const jello_object* o, int skip_proto, uint32_t* out);
void jello_vm_blocking(jello_vm* vm, int is_blocking);

#endif /* JELLO_H */
