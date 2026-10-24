// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The embedding API; load a `.jlo` module, run it on a VM, and build or inspect the values and
 * heap objects it exchanges with C. Native-library authors also include jello/jdll.h. */

#ifndef JELLO_H
#define JELLO_H

#include <stddef.h>
#include <stdint.h>

/** Index into a module's type table. */
typedef uint32_t jello_type_id;

/** The kind of a type-table entry, and so of every register, field and value of that type.
 * The values are the kind bytes of a `.jlo` type table. */
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

/** The number of jello_type_kind values; they run from 1 to this. */
#define JELLO_TYPE_KIND_COUNT 19

/** The bytes a value of kind `k` occupies as a struct field or array element; 4 for the narrow
 * scalars, 8 for I64 and F64, pointer size for Dynamic and every heap kind, 0 for an unknown
 * kind. */
size_t jello_slot_size(jello_type_kind k);

/** A function signature; type ids of the result and of the `nargs` arguments. */
typedef struct jello_fun_sig {
  jello_type_id ret;
  uint16_t nargs;
  const jello_type_id* args;
} jello_fun_sig;

/** A struct layout, from the module's struct-layout table. */
typedef struct jello_struct_sig {
  uint16_t nfields;
  uint8_t named;
  uint8_t is_mutable;
  const uint32_t* field_type;      /**< Type id of each field. */
  const uint32_t* field_name_atom; /**< Atom naming each field; NULL when `named` is 0. */
  const uint32_t* field_offset;    /**< Byte offset of each field in jello_struct::fields,
                                     * computed by the loader with each field aligned to its own
                                     * slot size; not stored in the `.jlo`. */
} jello_struct_sig;

/** One enum variant; the type ids of its `nfields` payload fields. */
typedef struct jello_enum_variant_sig {
  uint16_t nfields;
  const jello_type_id* field_type;
} jello_enum_variant_sig;

/** An enum's variants, indexed by tag. */
typedef struct jello_enum_sig {
  uint16_t nvariants;
  const jello_enum_variant_sig* variants;
} jello_enum_sig;

/** A type-table entry; its kind and, for the kinds that have one, the table it refers to. */
typedef struct jello_type_entry {
  jello_type_kind kind;
  union {
    struct { jello_type_id elem; } unary;         /**< LIST, ARRAY: the element type. */
    struct { uint32_t sig_id; } fun;               /**< FUNCTION: its signature. */
    struct { uint32_t struct_sig_id; } strct;      /**< STRUCT: its layout. */
    struct { uint32_t enum_sig_id; } enm;          /**< ENUM, with ENUM_SCHEMA: its variants. */
    struct { uint32_t abstract_id; } abs;          /**< ABSTRACT: an opaque identifier. */
  } as;
} jello_type_entry;

struct jello_bc_module;
/** A loaded, validated module. Opaque; create it with jello_bc_read. */
typedef struct jello_bc_module jello_bc_module;

/** Why jello_bc_read failed. JELLO_BC_EOF means the input ended early; JELLO_BC_BAD_FORMAT covers
 * malformed or invalid content and failed native imports. */
typedef enum jello_bc_error {
  JELLO_BC_OK = 0,
  JELLO_BC_EOF,
  JELLO_BC_BAD_MAGIC,
  JELLO_BC_UNSUPPORTED_VERSION,
  JELLO_BC_BAD_FORMAT,
  JELLO_BC_OUT_OF_MEMORY,
} jello_bc_error;

/** The outcome of jello_bc_read. */
typedef struct jello_bc_result {
  jello_bc_error err;
  const char* msg; /**< Text describing the error; not to be freed. An instruction-validation
                    * or native-library message lives in a per-thread buffer that the next such
                    * failure on the same thread overwrites. */
  size_t offset;   /**< Byte offset at or near the error; 0 for most validation failures. */
} jello_bc_result;

/** Parses and validates the `.jlo` image `data` of `size` bytes into `*out`.
 * The module copies what it needs, so `data` may be freed on return. Loading also opens every
 * native library the module imports and checks each import's signature against the library's
 * export; a missing library or mismatched signature is JELLO_BC_BAD_FORMAT.
 * @return JELLO_BC_OK with `*out` set, or an error with `*out` NULL and nothing to free. */
jello_bc_result jello_bc_read(const uint8_t* data, size_t size, jello_bc_module** out);
/** Frees `m` and unloads its native libraries, once no VM running it remains, as its function
 * values go with it. `m` may be NULL. */
void jello_bc_free(jello_bc_module* m);
/** The index of `m`'s entry function, or 0 when `m` is NULL. */
uint32_t jello_bc_get_entry(const jello_bc_module* m);

struct jello_vm;
/** One thread of execution and the heap it allocates from. Opaque. */
typedef struct jello_vm jello_vm;

/** Creates a VM with its own heap. Returns NULL when the VM cannot be allocated, and aborts when
 * the heap cannot. */
jello_vm* jello_vm_create(void);
/** Destroys `vm`. The last VM using a heap frees it and every object on it. `vm` may be NULL. */
void jello_vm_destroy(jello_vm* vm);

typedef enum jello_exec_status {
  JELLO_EXEC_OK = 0,
  JELLO_EXEC_TRAP = 1,
} jello_exec_status;

/** Why a run trapped, as jello_vm_last_trap_code reports it. */
typedef enum jello_trap_code {
  JELLO_TRAP_NONE = 0,
  JELLO_TRAP_ARITHMETIC = 1,
  JELLO_TRAP_STACK_OVERFLOW = 2,
  JELLO_TRAP_NULL_DEREF = 3,   /**< A null reference was used, or FROM_DYN_* unboxed null. */
  JELLO_TRAP_BOUNDS = 4,
  JELLO_TRAP_TYPE_MISMATCH = 5, /**< A value had the wrong kind, or a native call failed. */
  JELLO_TRAP_FUEL = 6,          /**< The jello_vm_set_fuel budget ran out. */
  JELLO_TRAP_LIMIT = 7,         /**< BYTES_NEW asked for more than jello_vm_set_max_bytes_len. */
  JELLO_TRAP_UNCAUGHT_THROW = 8, /**< A THROW found no enclosing try scope. */
  JELLO_TRAP_STOPPED = 9,        /**< The process was asked to stop. */
  JELLO_TRAP_DEADLOCK = 10,      /**< The process waited for what no process could ever do. */
  JELLO_TRAP_UNSENDABLE = 11,    /**< A value leaving its process held a native handle whose
                                  * class cannot share it. */
  JELLO_TRAP_SEALED = 12,        /**< A write would change a sealed method (jello_object_seal). */
} jello_trap_code;

/** The trap code of `vm`'s last run; JELLO_TRAP_NONE when it did not trap. `vm` must not be
 * NULL. */
jello_trap_code jello_vm_last_trap_code(const jello_vm* vm);
/** The name of trap code `tc`, such as "BOUNDS". */
const char* jello_trap_code_name(jello_trap_code tc);

/** Gives `vm` a budget of `fuel` units. A unit is spent on each backward JMP or JMP_IF (taken or
 * not), each CALL, CALLR, TAILCALL and TAILCALLR, and each THROW, interpreted or compiled; the run
 * traps with JELLO_TRAP_FUEL when none is left. The budget carries over between runs until set
 * again. 0, the default, means unlimited. */
void jello_vm_set_fuel(jello_vm* vm, uint64_t fuel);
/** Makes BYTES_NEW trap with JELLO_TRAP_LIMIT when asked for more than `max_len` bytes.
 * 0, the default, means no limit. */
void jello_vm_set_max_bytes_len(jello_vm* vm, uint32_t max_len);
/** Sets the program arguments natives read through jdl_program_arg. They belong to `vm`'s heap,
 * so every VM sharing it sees them; `argv` is not copied and must outlive every run. */
void jello_vm_set_program_args(jello_vm* vm, int32_t argc, const char* const* argv);
/** Sets the module image `vm` runs, `size` bytes at `data`, which the `vm` library's image_run runs
 * again when given none. It belongs to `vm`'s heap; `data` is not copied and must outlive every
 * run. */
void jello_vm_set_image(jello_vm* vm, const uint8_t* data, size_t size);
/** When `keep` is set, leaves the processes a run of `vm` spawns running after it returns, on
 * their schedulers, with the JIT code they run, until the next run or jello_vm_destroy stops
 * them, so that a host can drive them with jello_run_for. */
void jello_vm_keep_processes(jello_vm* vm, int keep);
/** Runs, on the calling thread, the processes waiting for `vm`'s main thread, those spawned on
 * main among them, and fires the timers due, until `ms` milliseconds have passed or none waits;
 * the other schedulers run theirs meanwhile, as they do between calls. A host calls it from the
 * thread that ran `vm`, between runs, for example once a frame; during a run it does nothing
 * and returns 1. Returns whether any of `vm`'s processes has yet to finish. */
int jello_run_for(jello_vm* vm, uint32_t ms);
/** The instruction index, within its function, where `vm`'s last run trapped; the calls that led
 * there are given by jello_vm_last_trap_callers. Meaningful only after a trap; 0 when `vm` is
 * NULL. */
uint32_t jello_vm_last_trap_pc(const jello_vm* vm);
/** The index of the function in which `vm`'s last run trapped; see jello_vm_last_trap_pc. */
uint32_t jello_vm_last_trap_func_index(const jello_vm* vm);
/** The most calls jello_vm_last_trap_callers gives. */
#define JELLO_TRAP_CALLERS_MAX 16u
/** Copies into `func_indexes` and `pcs` up to `max` of the calls through which `vm`'s last run
 * reached its trap, innermost first: each is a call instruction's function and pc. A call passed
 * through several times in a row, as in deep recursion, is given once, with its `repeated` entry
 * set; `repeated` may be NULL. Returns how many it copied; 0 when `vm` is NULL. */
uint32_t jello_vm_last_trap_callers(const jello_vm* vm, uint32_t* func_indexes, uint32_t* pcs,
                                    uint8_t* repeated, uint32_t max);

/** A Dynamic value; a pointer-sized word whose low 3 bits are a jello_value_tag.
 * Null, Bool, I32 and Atom live in the word itself. Any other value is a pointer to a heap object
 * (tag 0, since heap objects are 8-byte aligned); I64 and the floats are boxed. A PTR-tagged NULL
 * pointer, jello_from_ptr(NULL), is not the same value as jello_make_null(). */
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

/** The value an uncaught throw threw, when jello_vm_last_trap_code is JELLO_TRAP_UNCAUGHT_THROW.
 * It is not rooted, so it is valid only until the next allocation on the VM's heap. */
jello_value jello_vm_last_thrown(const jello_vm* vm);

/** What the site of `vm`'s last trap said about it, such as "native open failed: no such file"
 * when a native call failed, or "" when it said nothing. */
const char* jello_vm_last_trap_detail(const jello_vm* vm);

/** The kind of value a run returned in a jello_numeric_result. */
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

/** The entry function's result, returned without allocating.
 * Bool, I32, I64, F32 and F64 arrive as C values; Bytes, Struct and Object as a pointer
 * (JELLO_NUM_PTR, possibly NULL); Dynamic as its tagged word (JELLO_NUM_DYN). Any other result
 * kind reads as JELLO_NUM_VOID. A heap object in the result is not rooted, so it is valid
 * only until the next allocation on the VM's heap. */
typedef struct jello_numeric_result {
  jello_numeric_kind kind;
  union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    int b; /**< 0 or 1. */
    void* ptr;
    jello_value dyn;
  } as;
} jello_numeric_result;

/** Runs `m`'s entry function, with no arguments, on `vm` and stores its result in `*out`.
 * In a JIT build the module is compiled for this call alone, unless the environment variable
 * JELLO_JIT_DISABLE is set and non-empty, in which case it is interpreted. One run at a time per
 * VM.
 * @return JELLO_EXEC_OK, or JELLO_EXEC_TRAP with the cause in jello_vm_last_trap_code. */
jello_exec_status jello_vm_exec_numeric(jello_vm* vm, const jello_bc_module* m, jello_numeric_result* out);

/** A module's JIT-compiled code, kept across runs so that a host calling the same module many
 * times compiles it once. Opaque. */
typedef struct jello_jit_module jello_jit_module;

/** Prepares `m` for the JIT. On x64 every function is compiled up front, and the call fails if any
 * cannot be; on arm64 each function is compiled when first called, and one the JIT cannot compile
 * runs interpreted. The handle changes as code runs, so use it from one thread at a time. `m` must
 * outlive it.
 * @return The handle, or NULL on failure, including when `m` has no functions or the library
 * was built without the JIT. */
jello_jit_module* jello_jit_module_create(const jello_bc_module* m);
/** Frees `jm`, which may be NULL. */
void jello_jit_module_destroy(jello_jit_module* jm);
/** Returns 1 when the library was built with the JIT, 0 when every run is interpreted. */
int jello_jit_available(void);

/** Like jello_vm_exec_numeric, but runs on `jm`, which must have been created from `m`.
 * With `jm` NULL it is exactly jello_vm_exec_numeric; when JELLO_JIT_DISABLE is set and
 * non-empty, `m` is interpreted and `jm` is unused. */
jello_exec_status jello_vm_exec_numeric_with_jit(jello_vm* vm, jello_jit_module* jm,
    const jello_bc_module* m, jello_numeric_result* out);

/** Called by jello_vm_create; embedders do not call it. */
void jello_gc_init(jello_vm* vm);
/** Called by jello_vm_destroy for the last VM on a heap; frees every object on it. */
void jello_gc_shutdown(jello_vm* vm);
/** Allocates `size` uninitialised bytes on `vm`'s heap, first collecting if the heap has grown
 * past its threshold. The caller must fill in the jello_obj_header and make the object reachable
 * before the next allocation, or a collection may free it. Aborts when out of memory. */
void* jello_gc_alloc(jello_vm* vm, size_t size);
/** Collects `vm`'s heap, stopping every VM sharing it. Returns without collecting when
 * another thread's collection was already running. */
void jello_gc_collect(jello_vm* vm);
/** Pushes `obj` onto `vm`'s root stack, keeping it and what it references alive while `vm` is
 * attached to its heap. Native code holding an object across an allocation must root it first. */
void jello_gc_push_root(jello_vm* vm, void* obj);
/** Pops the `n` roots most recently pushed onto `vm`'s stack. Aborts when fewer than `n` are
 * pushed. */
void jello_gc_pop_roots(jello_vm* vm, uint32_t n);

/** Statistics of the space `vm` allocates in, for tests and benchmarks; the bytes the collector
 * counts as live (native handles' estimated sizes and the shared binaries its Bytes hold
 * included), the number of collections, and the total milliseconds spent collecting. A
 * process's space is its own, so a main VM's statistics leave out its processes'. Any out
 * pointer may be NULL. */
void jello_vm_gc_stats(const jello_vm* vm, uint64_t* out_live_bytes, uint32_t* out_collections,
                        double* out_pause_ms);

/** The kind of a heap object, from its jello_obj_header. */
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

/** The first field of every heap object, so any object pointer can be inspected as one.
 * `type_id` is the struct sig index for a Struct (the collector reads its field kinds from it),
 * the Array type's type id for an Array, the List, Enum, Object or Function type's id for those
 * kinds, and 0 for boxes, handles and Bytes the VM never writes. */
typedef struct jello_obj_header {
  uint8_t kind;
  uint32_t type_id;
} jello_obj_header;

/** A Bytes value, read through `data` in every case, which points:
 * - for one of under 64 bytes that is no slice, into its own `storage`, `owner` being NULL;
 * - for one of 64 bytes or more that is no slice, into a shared binary, in no heap and reference
 *   counted, which processes share: its `owner`, of which it holds a reference, `shared` being 1;
 * - for a slice, into its `owner`, which it keeps alive: either of those, never another slice. */
typedef struct jello_bytes {
  jello_obj_header h;
  uint32_t length;
  uint32_t shared; /**< 1 when `owner` is a shared binary it holds a reference to, else 0. */
  uint8_t* data;
  struct jello_bytes* owner;
  uint8_t storage[];
} jello_bytes;

/** Allocates a binary of `length` uninitialised bytes, for a caller that writes them all through
 * `data`. May collect; aborts when out of memory. */
jello_bytes* jello_bytes_new(jello_vm* vm, uint32_t length);
/** Allocates a binary of `length` zero bytes. May collect; aborts when out of memory. */
jello_bytes* jello_bytes_new_zeroed(jello_vm* vm, uint32_t length);
/** Allocates a slice of the `length` bytes of `src` from `start`, sharing its storage, or a copy
 * of them when `src` is a buffer the VM may write. The caller checks the range lies within `src`,
 * and keeps `src` reachable, since the allocation may collect. */
jello_bytes* jello_bytes_slice(jello_vm* vm, jello_bytes* src, uint32_t start, uint32_t length);

/** Boxes for the Dynamic values that do not fit in a jello_value's word. F16 holds IEEE 754
 * half-precision bits. */
typedef struct jello_box_i64 { jello_obj_header h; int64_t value; } jello_box_i64;
typedef struct jello_box_f64 { jello_obj_header h; double value; } jello_box_f64;
typedef struct jello_box_f32 { jello_obj_header h; float value; } jello_box_f32;
typedef struct jello_box_f16 { jello_obj_header h; uint16_t value; } jello_box_f16;

/** Each may collect, and aborts when out of memory. */
jello_box_i64* jello_box_i64_new(jello_vm* vm, int64_t v);
jello_box_f64* jello_box_f64_new(jello_vm* vm, double v);
jello_box_f32* jello_box_f32_new(jello_vm* vm, float v);
jello_box_f16* jello_box_f16_new(jello_vm* vm, uint16_t bits);

/** jello_is_box_* test for a non-NULL pointer to that box; jello_as_box_* do not check. */
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

/** A fixed-layout record. `h.type_id` is its struct sig index, and field `i` sits at
 * `fields + field_offset[i]` of that sig; STRUCT_GET and STRUCT_SET are fixed-offset accesses. */
typedef struct jello_struct {
  jello_obj_header h;
  uint8_t fields[];
} jello_struct;

/** Allocates a struct of `m`'s struct sig `struct_sig_id`, its fields zero and its Dynamic fields
 * null. May collect; aborts when out of memory. */
jello_struct* jello_struct_new(jello_vm* vm, const jello_bc_module* m, uint32_t struct_sig_id);

/** A dynamically shaped record keyed by atom; an open-addressing hash table with an optional
 * prototype. `keys`, `vals` and `states` share one malloc block, owned by the object and freed
 * when it is collected. */
typedef struct jello_object {
  jello_obj_header h;
  struct jello_object* proto;
  uint32_t cap; /**< Slots in the table; a power of two. */
  uint32_t len; /**< Occupied slots. */
  uint32_t tombstones; /**< Removed slots; they count towards the load factor so a probe always
                        * ends at an empty slot. */
  uint32_t sealed; /**< Sealed slots (jello_object_seal). */
  uint32_t* keys;
  jello_value* vals;
  uint8_t* states;
} jello_object;

/** A homogeneous, growable sequence. `h.type_id` is the type id of its Array type, whose element
 * kind gives each element's jello_slot_size. `data` is a separate malloc buffer of `cap` elements,
 * owned by the array, so resizing never moves the header; `length` elements are in use. */
typedef struct jello_array {
  jello_obj_header h;
  uint32_t length;
  uint32_t cap;
  void* data;
} jello_array;

/** Allocates an array of `m`'s Array type `array_type_id` with `length` elements, zero or, for a
 * Dynamic element type, null. May collect; aborts when out of memory. */
jello_array* jello_array_new(jello_vm* vm, const jello_bc_module* m, uint32_t array_type_id, uint32_t length);
/** Sets `a`'s length to `new_length`. Growing may move `data`, so pointers into it go stale;
 * added elements are zero or null as in jello_array_new. Does not collect; aborts when out of
 * memory. */
void jello_array_resize(jello_vm* vm, const jello_bc_module* m, jello_array* a, uint32_t new_length);

/** A list cell. The empty list is NULL. */
typedef struct jello_list {
  jello_obj_header h;
  jello_value head;
  struct jello_list* tail;
} jello_list;

/** Allocates the cell `head` followed by `tail`, of List type `type_id`. `head` and `tail` must
 * stay reachable from a root during the call, since the allocation may collect. */
jello_list* jello_list_cons(jello_vm* vm, uint32_t type_id, jello_value head, jello_list* tail);

/** An enum value; variant `tag` of the Enum type `h.type_id`. Payload fields are Dynamic words,
 * so recursive enums are ordinary object graphs. */
typedef struct jello_enum {
  jello_obj_header h;
  uint32_t tag;
  uint32_t nfields;
  jello_value fields[];
} jello_enum;

/** Allocates an enum value with `nfields` fields, copied from `fields`, or null when `fields` is
 * NULL. Objects in `fields` must stay reachable during the call, since it may collect. Aborts when
 * out of memory. */
jello_enum* jello_enum_new(jello_vm* vm, uint32_t type_id, uint32_t tag,
                            uint32_t nfields, const jello_value* fields);

/** A function value, with or without captures; CONST_FUN and CLOSURE both make one, so an
 * indirect call never distinguishes the two. Each capture is an 8-byte slot, copied into the
 * callee's registers from its `cap_start` on every call. */
typedef struct jello_function {
  jello_obj_header h;
  uint32_t func_index;
  uint32_t ncaps;
  uint64_t captures[];
} jello_function;

/** Allocates a function value for function `func_index` with `ncaps` zeroed captures and a
 * `type_id` of 0 for the caller to set. May collect; aborts when out of memory. */
jello_function* jello_function_new(jello_vm* vm, uint32_t func_index, uint32_t ncaps);

/** Allocates an empty object of Object type `type_id`. May collect; aborts when out of memory. */
jello_object* jello_object_new(jello_vm* vm, uint32_t type_id);
/** Whether `o` itself, not its prototype, has key `atom_id`. */
int jello_object_has(const jello_object* o, uint32_t atom_id);
/** `o`'s own value for `atom_id`, or null when absent. */
jello_value jello_object_get(const jello_object* o, uint32_t atom_id);
/** Sets `o`'s `atom_id` to `v`, adding the key if absent. Does not collect; aborts when out of
 * memory. */
void jello_object_set(jello_object* o, uint32_t atom_id, jello_value v);
/** Sets `o`'s `atom_id` to `v` and seals the slot, as a linked prototype's methods are: Object
 * writes then trap with JELLO_TRAP_SEALED rather than set or remove it, shadow it in an Object
 * that delegates to `o`, or relink such an Object. Does not collect; aborts when out of memory. */
void jello_object_seal(jello_object* o, uint32_t atom_id, jello_value v);
/** Removes `atom_id` from `o`; whether it was there. */
int jello_object_remove(jello_object* o, uint32_t atom_id);
void jello_object_clear(jello_object* o);
/** Allocates a shallow copy of `src`, sharing its prototype. May collect, so `src` must stay
 * reachable; aborts when out of memory. */
jello_object* jello_object_copy(jello_vm* vm, const jello_object* src);
/** Writes `o`'s own keys to `out`, which has room for `o->len`, in ascending atom order; with
 * `skip_proto`, atom 0 (__proto__ when prototypes are enabled) is left out.
 * @return the number of keys written */
uint32_t jello_object_keys(const jello_object* o, int skip_proto, uint32_t* out);

/** Marks the calling thread as blocked (`is_blocking` nonzero) or running again (zero), so a
 * collection on another thread need not wait for it. Calls nest and must balance; leaving waits
 * for a running collection to finish and aborts if not inside. While blocked, the thread must not
 * touch heap objects or call other VM functions. */
void jello_vm_blocking(jello_vm* vm, int is_blocking);

#endif /* JELLO_H */
