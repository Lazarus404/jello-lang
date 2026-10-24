// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * VM internals shared by load, interp, jit, gc, jdll and thread; the concrete module, register
 * frames, call frames, the shared heap and the per-thread VM, plus the internal entry points
 * between those parts. */

#ifndef JELLO_VM_INTERNAL_H
#define JELLO_VM_INTERNAL_H

#include <jello.h>
#include <jello/internal/bytecode.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/** One entry of a module's native import table, which JOP_CALL_NATIVE indexes. It names a
 * `.jdll` and an export within it, both by atom; the loader rejects the module unless the
 * declared signature matches the export exactly. */
typedef struct jello_import_sig {
  uint32_t library_atom;
  uint32_t export_atom;
  uint8_t ret_kind;
  uint8_t arity;
  uint8_t arg_kinds[8];
  const struct jello_jdll_export* resolved; /**< Set by the loader once the signature matches;
                                             * never read from the file. */
} jello_import_sig;

/** A loaded module; opaque in jello.h. */
struct jello_bc_module {
  uint32_t version;
  uint32_t features;
  uint32_t ntypes;
  const jello_type_entry* types;
  uint32_t nsigs;
  const jello_fun_sig* sigs;
  uint32_t nstruct_sigs;
  const jello_struct_sig* struct_sigs;
  uint32_t nenum_sigs;
  const jello_enum_sig* enum_sigs;
  uint32_t natoms;
  const char* atoms_data;
  const char* const* atoms;
  uint32_t nconst_i64;
  const int64_t* const_i64;
  uint32_t nconst_f64;
  const double* const_f64;
  uint32_t nconst_bytes;
  /** Each Bytes constant's one value, immortal, every process sharing it. The values follow the
   * table in its one block, which is freed with the module. */
  struct jello_bytes* const* const_bytes_values;
  /* One 8-byte slot per global in its declared slot representation; zero at load, or null for
   * a Dynamic global. The main VM's globals, and a GC root while the module runs; each process
   * has a table of its own. */
  uint32_t nglobals;
  const jello_type_id* global_types;
  uint64_t* globals;
  const uint32_t* ptr_globals; /**< The globals whose kind may hold a pointer, by index. */
  uint32_t nptr_globals;
  uint32_t nfuncs;
  const jello_bc_function* funcs;
  /** The one Function value of each function without captures, by index, once CONST_FUN has
   * made it, so that it equals itself; else NULL. Each is immortal, every process sharing it,
   * and freed with the module. */
  _Atomic(struct jello_function*)* const_funs;
  /** The one value of each enum variant without fields, at const_enum_base[type] + tag, once
   * ENUM_NEW has made it; else NULL. Each is immortal, every process sharing it, and freed with
   * the module. Both tables are NULL in a module without enums. */
  _Atomic(struct jello_enum*)* const_enums;
  uint32_t nconst_enums;
  const uint32_t* const_enum_base; /**< By type; meaningful for an enum type only. */
  uint32_t entry;
  uint8_t proto_enabled; /**< Derived at load; atom 0 exists and is "__proto__". */
  uint32_t nimports;
  const jello_import_sig* imports;
  uint32_t nsource_files;
  const char* const* source_files; /**< Line-table source paths (JELLO_BC_FEAT_LINE_TABLE). */
  void* jdll_handles; /**< One jello_jdll_handle* per import, in import order; closed by
                       * jello_bc_free. */
  uint32_t njdll_handles;
};

/** The atom a compiler interns "__proto__" at when it uses prototype chains. */
#define JELLO_ATOM___PROTO__ 0u

/** Slot states of a jello_object's hash table, shared by gc/object.c, the collector and JIT code.
 * A slot holds a value when its state is odd. An owned slot's value is an F64 box that nothing
 * else references, so a typed F64 store writes into it rather than allocating; a read that lets
 * the box escape makes the slot plain occupied first. */
enum {
  JELLO_OBJ_SLOT_EMPTY = 0,
  JELLO_OBJ_SLOT_OCCUPIED = 1,
  JELLO_OBJ_SLOT_TOMB = 2,
  JELLO_OBJ_SLOT_OWNED = 3,
  JELLO_OBJ_SLOT_SEALED = 5, /**< Holds a method that may not change (jello_object_seal). */
};
/** Whether a slot in `state` holds a value. */
static inline int vm_obj_slot_live(uint8_t state) { return state & 1u; }
/** The hash that places `atom_id` in an object's table; JIT code folds it for a constant atom. */
static inline uint32_t vm_atom_hash(uint32_t atom_id) {
  uint32_t x = atom_id;
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
#define JELLO_OBJECT_INIT_CAP 8u
#define JELLO_OBJECT_LOAD_NUM 10u
#define JELLO_OBJECT_LOAD_DEN 7u

/** Checks one instruction's operands, types and jump targets. `ins` is `pc` into the function's
 * `ninsns` instructions. The loader calls it once all functions are read, for every instruction
 * but the rows of a switch_kind table, which their SWITCH_KIND checks. */
jello_bc_result jello_bc_validate_insn(const jello_bc_module* m, const jello_type_id* reg_types,
                                       const jello_insn* ins, uint32_t nregs, uint32_t pc,
                                       uint32_t ninsns, uint32_t nfuncs);

/** Checks every call site's destination, and every Function value's signature, against its
 * callee's result type; the type its RETs agree on or, for a function without RET, the result of
 * what it tail calls. RET copies a raw slot, so a destination of another kind would reinterpret
 * the bits; a call through a Void signature needs a Void destination. Run after every instruction
 * is validated, so it indexes with operands already checked. */
jello_bc_result jello_bc_validate_results(const jello_bc_module* m);

/** Checks that try regions nest consistently, within JELLO_TRY_STACK_MAX, at every pc. */
jello_bc_result jello_bc_validate_try_regions(const jello_bc_module* m);

/** Looks up instruction `pc` of function `fi` in the line table. Returns 1 and sets `*file`,
 * `*line` and `*col`, or returns 0 when the module has no location for it. */
int jello_bc_source_location(const jello_bc_module* m, uint32_t fi, uint32_t pc, const char** file,
                             uint32_t* line, uint32_t* col);

/** Bytes per register. Every register takes one 8-byte slot whatever its kind, so register `r`
 * is at `mem + r * REG_SLOT_BYTES` with no offset table; a 4-byte kind uses the slot's first four
 * bytes, which stay naturally aligned. */
#define REG_SLOT_BYTES 8u

/** One frame's registers; a slice of the VM's frame stack plus its function's cached layout. */
typedef struct reg_frame {
  uint8_t* mem;
  const uint8_t* kind; /**< Declared jello_type_kind per register, from the frame_layout. */
  const uint8_t* wide; /**< Per register; 1 when its slot value is 8 bytes, 0 when 4. */
  uint32_t size;
} reg_frame;

static inline uint8_t* vm_reg_ptr(reg_frame* rf, uint32_t r) {
  return rf->mem + (size_t)r * REG_SLOT_BYTES;
}

/** Fast form of vm_reg_kind, from the frame's cached layout. */
static inline jello_type_kind vm_reg_kind_fast(const reg_frame* rf, uint32_t r) {
  return (jello_type_kind)rf->kind[r];
}
static inline int vm_reg_is_wide(const reg_frame* rf, uint32_t r) { return rf->wide[r] != 0; }

/** Whether kind `k` holds a raw heap pointer. Dynamic holds a tagged word and is excluded. */
static inline int vm_kind_is_gc_pointer(jello_type_kind k) {
  switch(k) {
    case JELLO_T_BYTES:
    case JELLO_T_LIST:
    case JELLO_T_ARRAY:
    case JELLO_T_STRUCT:
    case JELLO_T_OBJECT:
    case JELLO_T_FUNCTION:
    case JELLO_T_ABSTRACT:
    case JELLO_T_ENUM: return 1;
    default: return 0;
  }
}

static inline int vm_kind_needs_gc_scan(jello_type_kind k) {
  return k == JELLO_T_DYNAMIC || vm_kind_is_gc_pointer(k);
}

/** A native library's object (a jdll.h handle class instance), reachable only through Dynamic.
 * The collector finalises the payload and counts `native_bytes` as live heap, so native memory
 * paces collection. */
typedef struct jello_handle {
  jello_obj_header h;
  const struct jello_jdll_handle_class* cls;
  void* payload;
  uint32_t native_bytes;
} jello_handle;

/** Allocates a handle of class `cls` holding `payload`, and adds `native_bytes` to the heap's
 * live count. */
jello_handle* jello_handle_new(jello_vm* vm, const struct jello_jdll_handle_class* cls,
                               void* payload, uint32_t native_bytes);

/** Allocates a binary of the bytes of the `n` non-null `parts`, in order, with the bytes of
 * `separator` between each two unless it is NULL. May collect, which leaves the parts in place.
 * @return the binary, or NULL with a BOUNDS trap set when it would pass UINT32_MAX bytes */
jello_bytes* jello_bytes_join(jello_vm* vm, jello_bytes* const* parts, uint32_t n,
                              const jello_bytes* separator);

/** Try scopes per frame. The validator rejects deeper nesting, and TRY traps STACK_OVERFLOW
 * rather than overrun the fixed array. */
#define JELLO_TRY_STACK_MAX 16u

/** SPILL_PUSH/SPILL_POP slots per frame; the validator rejects a slot index at or past this. */
#define JELLO_SPILL_SLOTS_MAX 65536u

typedef struct try_scope {
  uint32_t catch_pc;
  uint32_t payload_reg;
} try_scope;

/** One activation. Frames are strictly LIFO, so RET reclaims a frame's registers by rewinding
 * the frame stack. */
typedef struct call_frame {
  const jello_bc_function* f;
  uint32_t func_index;
  uint32_t pc; /**< The next instruction to run. */
  reg_frame rf;
  uint8_t has_caller;
  uint8_t has_pointer; /**< Copied from the frame_layout so root scanning can skip the frame. */
  uint32_t caller_dst; /**< Register in the caller's frame that RET writes into. */
  /** What popping it takes beyond rewinding the frame stack, read at once as pop_work by the
   * JIT's inline pop, which leaves a frame with either to vm_pop_frame. */
  union {
    struct {
      uint16_t try_stack_len;  /**< Its open try scopes, the top ones of the VM's try_scopes. */
      uint16_t starts_segment; /**< 1 when it is the first frame of a frame stack segment, whose
                                * pop returns to the segment below. */
    };
    uint32_t pop_work;
  };
  jello_value* spill_slots; /**< SPILL_PUSH/SPILL_POP storage, grown on demand and freed on pop.
                             * Values are boxed, so a slot round-trips whatever kind the spilled
                             * register has; unwritten slots hold null. */
  uint32_t spill_cap;
} call_frame;

/** A run of frame stack. A VM's first is the stack it was attached to; above that it maps one
 * when its frames outgrow the segment below, which keeps its frames where they are. Frames never
 * move, so a segment is entered by its first frame's push and left by that frame's pop. While a
 * segment is current, the VM's frame_stack fields hold its place; its own hold it otherwise. */
typedef struct frame_segment {
  uint8_t* mem;
  uint32_t bytes;              /**< Reserved. */
  uint32_t limit;              /**< Committed. */
  uint32_t top;                /**< In use. */
  struct frame_segment* below; /**< The segment it was entered from; NULL for the first. */
  struct frame_segment* above; /**< The segment mapped above it, kept once left; or NULL. */
} frame_segment;

/** Per-function register metadata, cached for one top-level call; built on a function's first
 * call, or for every function when the JIT warms the cache, and freed by vm_frame_reset. */
typedef struct frame_layout {
  const jello_bc_function* f; /**< Cache key. */
  uint8_t* kind;
  uint8_t* wide;
  uint8_t* init;   /**< When has_pointer, the `total` bytes a pushed frame starts as: zero, with
                    * null in each Dynamic register (a zero word is not null). */
  uint32_t total;  /**< `f->nregs * REG_SLOT_BYTES`. */
  int has_pointer; /**< Some register is a pointer or Dynamic kind; push copies `init` into the
                    * frame and the collector scans it. */
} frame_layout;

/** Allocator bookkeeping at the start of each heap block, immediately before the object, whose
 * own jello_obj_header comes first; only gc.c, and vm_is_immortal, read or write it, and a
 * vm_parcel links its blocks by it. */
typedef struct jello_gc_hdr {
  struct jello_gc_hdr* next; /**< Live-object list, or a free list once swept. */
  uint32_t total_size;       /**< Header plus payload, rounded up to the size class. */
  /** 1 once a collection has reached it, else 0; or GC_IMMORTAL or GC_SHARED for an object in no
   * space, which the collector skips as already marked, so neither traces nor frees it. Such an
   * object must therefore reference no object in a space. */
  uint8_t marked;
} jello_gc_hdr;
/** The `marked` of an immortal object. */
#define GC_IMMORTAL 2u
/** The `marked` of a shared binary: the bytes of a large Bytes, in no space and reference counted,
 * which each Bytes holding a reference names as its `owner`, its `shared` set. */
#define GC_SHARED 3u

/** Whether heap object `p` is immortal: in no space, shared by every process and heap. */
static inline int vm_is_immortal(const void* p) {
  return ((const jello_gc_hdr*)p - 1)->marked == GC_IMMORTAL;
}

/** The least length of a Bytes whose bytes live in a shared binary, so that a copy for another
 * process shares them rather than copying them. */
#define VM_BYTES_SHARED_MIN 64u

/** Power-of-two allocation size classes, 32 to 4096 bytes, each with its own free list. Larger
 * blocks are allocated and freed individually. */
#define GC_NUM_SIZE_CLASSES 8
#define GC_MAX_SIZE_CLASS_BYTES 4096u

/** Objects that are allocated into and collected together, with their counters. */
typedef struct jello_space {
  jello_gc_hdr* objects; /**< Every live object, singly linked. */
  jello_gc_hdr* free_classes[GC_NUM_SIZE_CLASSES];
  uint64_t bytes_live;   /**< Its objects' blocks, its handles' native bytes, and the shared
                          * binaries its Bytes hold. */
  uint64_t next_collect; /**< Allocation collects once bytes_live reaches this. */
  uint32_t collections;
  uint32_t freed_objects;
  uint32_t finalized_handles;
  double pause_ms; /**< Cumulative wall-clock time inside jello_gc_collect. */
} jello_space;

/** The heap shared by every jello_vm attached to it. jello_vm_create makes a private heap;
 * jello_vm_create_joined attaches another thread's VM to an existing one. */
typedef struct jello_heap {
  jello_space space; /**< The objects of its first VM and the VMs joined to it; a process's VM
                      * has a space of its own. */

  pthread_mutex_t alloc_lock; /**< Guards `space`'s objects and free lists, and threads[];
                               * a stop-the-world holds it for its whole stopped window. */
  atomic_int stop_requested;  /**< Polled with one atomic load at every allocation and safepoint. */
  pthread_mutex_t stw_lock;   /**< With stw_resume_cv, parks threads until stop_requested clears. */
  pthread_cond_t stw_resume_cv;
  pthread_mutex_t proc_lock;    /**< Guards the schedulers of processes on the heap. */
  _Atomic uint32_t collections; /**< Of every space on it. */

  struct jello_vm** threads;
  uint32_t threads_len;
  uint32_t threads_cap;
  uint32_t refcount; /**< VMs attached; the last jello_vm_destroy frees the heap. */

  int32_t program_argc; /**< From jello_vm_set_program_args. */
  const char* const* program_argv;
  const uint8_t* image; /**< From jello_vm_set_image; NULL when unset. */
  size_t image_size;
} jello_heap;

/** The call sites a trap unwound through, innermost first, so that a report can say how the
 * program reached the trapping instruction. Each frame records its site as it is left: JIT code
 * as its trap exit pops it, and the interpreter from its frames, whose pcs are current. A run of
 * one site, as in deep recursion, is one entry, so the calls that began it are kept too. */
typedef struct jello_trap_trace {
  uint32_t len;
  /** 1 + the pc of the call through which a callee's trap is passing in the top frame, which is
   * then a caller to record; 0 in the frame where the trap happened. */
  uint32_t site;
  uint32_t func_index[JELLO_TRAP_CALLERS_MAX];
  uint32_t pc[JELLO_TRAP_CALLERS_MAX];
  uint8_t repeated[JELLO_TRAP_CALLERS_MAX]; /**< Whether the site was passed more than once. */
  uint8_t full; /**< A site was dropped for want of room, so no later one is recorded. */
} jello_trap_trace;

/** One thread's VM; its own frame stack and call frames, and a pointer to the shared heap. */
struct jello_vm {
  jello_heap* heap;
  jello_space* space; /**< Where it allocates, and what its collections sweep: its heap's, or a
                       * process's own. */

  uint8_t* frame_stack; /**< The current segment's frame_stack_max bytes; never moves. */
  uint32_t frame_stack_top;
  uint32_t frame_stack_limit;  /**< The bytes of frame_stack committed; a push past them grows
                                * them, through jit_try_grow_push. */
  uint32_t frame_stack_max;    /**< Bytes of frame_stack; a push past them enters a segment above,
                                * through jit_try_grow_push. */
  frame_segment* segment;      /**< The current segment; first_segment, or one mapped above it. */
  frame_segment first_segment; /**< The frame stack it was attached to. */
  uint32_t segment_bytes;      /**< The bytes of every segment mapped above the first. */
  uint32_t call_frames_limit;  /**< Most call frames at once; one more traps STACK_OVERFLOW. */

  call_frame* call_frames;
  uint32_t call_frames_len;
  uint32_t call_frames_cap;

  frame_layout* frame_layouts; /**< Layout cache. vm_frame_reset clears it at every top-level
                                * call, because a freed module's function addresses can be reused
                                * by an unrelated module. */
  uint32_t frame_layouts_len;
  uint32_t frame_layouts_cap;

  jello_trap_code trap_code;
  uint32_t trap_pc; /**< With trap_func_index, where the last trap happened; valid while
                     * trap_code != JELLO_TRAP_NONE. JIT code stores both before its trap exit,
                     * since a JIT frame's pc is not kept current. */
  uint32_t trap_func_index;
  jello_value thrown;    /**< What an uncaught throw threw; see jello_vm_last_thrown. */
  char trap_detail[160]; /**< What the last trap's site said about it, or empty; see
                          * jello_vm_last_trap_detail. */
  const jello_bc_module* running_module; /**< Set for a top-level call; root scanning reads
                                          * register kinds from it. */
  uint64_t* globals; /**< The globals it reads and writes: running_module's own, or, for a
                      * process, a table of its own, which it owns when `owns_globals`. */
  uint8_t owns_globals;
  uint8_t keep_processes;  /**< Set by jello_vm_keep_processes. */
  int32_t net_error;       /**< Why its last failing socket call failed; see net.c. */
  void* gc_transient_root; /**< An object still being populated, held as a root. */
  void** gc_roots;         /**< The roots jello_gc_push_root pushed, a stack. */
  uint32_t gc_roots_len;
  uint32_t gc_roots_cap;
  struct vm_parcel* parcel; /**< Where it allocates while vm_parcel_pack copies, rather than its
                             * space. */

  struct jello_jit_module* active_jit; /**< The running call's JIT module; NULL when interpreted. */
  /** The JIT module a run prepared for itself alone, kept for the processes it left running
   * until they end; NULL when there is none. */
  struct jello_jit_module* kept_jit;
  uintptr_t jit_stack_floor;    /**< Lowest C stack address an interpreted call may enter JIT code
                                 * from. */
  uintptr_t native_stack_limit; /**< Lowest native stack address JIT code may call from, leaving
                                 * room below for what it calls; 0 for no limit. */
  /** The open try scopes of every frame, innermost last; a frame's are above those of the frames
   * below it, as only the top frame opens one and a pop closes the popped frame's. */
  try_scope* try_scopes;
  uint32_t try_scopes_cap;
  uint32_t live_try_scopes; /**< Try scopes on every frame; zero means a throw is uncaught. */
  uint32_t unwind_len;      /**< Frame count of a caught throw's catcher, while trap_code is
                             * JELLO_TRAP_INTERNAL_UNWIND; each JIT or nested-interpreter activation
                             * pops itself until the catcher's loop resumes at its handler. */
  uint64_t jit_enters;      /**< For --jit-report. */
  uint64_t jit_bridges;     /**< For --jit-report. */

  int32_t reductions;        /**< Charges left in the window, 0 when made; the last one ends it. */
  uint32_t reduction_window; /**< The charges the current window started with. */
  uint64_t fuel_limit;       /**< 0 disables fuel. */
  uint64_t fuel_remaining;   /**< Fuel before the current window; settled when it ends. */
  uint32_t max_bytes_len;    /**< BYTES_NEW length cap; 0 disables it. */

  atomic_int quiescent_depth; /**< Nonzero while the thread does not touch Jello-visible memory;
                               * jello_vm_blocking, a native call, or parking at a safepoint. The
                               * collector reads it from other threads without a lock. */
  atomic_int in_exec; /**< Set for the whole of jello_vm_exec_numeric, before frames are reset,
                       * and cleared only after the loop returns; or VM_RUNNING_OUTSIDE between
                       * jello_vm_running calls. The collector's spin checks this rather than
                       * call_frames_len, which reads 0 while a call is starting. */

  frame_layout** jit_layout_by_func; /**< Layout per function index, from vm_warm_jit_layouts. */
  uint32_t jit_layout_by_func_len;
  uint8_t layouts_shared;      /**< The layouts are another VM's, from vm_share_layouts. */
  uint32_t heap_index;         /**< Its index in heap->threads. */
  struct jello_process* proc;  /**< The process it runs as, once its scheduler has started. */
  jello_trap_trace trap_trace; /**< How the last trap was reached; last, being cold. */
};

/** Whether `space` is `heap`'s own, which VMs on many threads may share, not a process's. */
static inline int vm_space_shared(const jello_heap* heap, const jello_space* space) {
  return space == &heap->space;
}

/** Whether `vm`'s space is its own, a process's, freed with it, rather than its heap's. */
static inline int vm_owns_space(const jello_vm* vm) {
  return !vm_space_shared(vm->heap, vm->space);
}

/** The kind global `g` of `m` is declared with. */
static inline jello_type_kind vm_global_kind(const jello_bc_module* m, uint32_t g) {
  return m->types[m->global_types[g]].kind;
}

/** The object global `g` of `globals`, a table laid out as `m`'s, holds, or NULL; `g` is one of
 * `m`'s ptr_globals. */
static inline void* vm_global_ptr(const jello_bc_module* m, const uint64_t* globals, uint32_t g) {
  if(vm_global_kind(m, g) == JELLO_T_DYNAMIC) {
    jello_value v;
    memcpy(&v, &globals[g], sizeof v);
    return jello_is_ptr(v) ? jello_as_ptr(v) : NULL;
  }
  void* p;
  memcpy(&p, &globals[g], sizeof p);
  return p;
}

/** `r`'s Bytes, whether a Bytes result or a Dynamic holding one; NULL otherwise. */
static inline const jello_bytes* vm_result_bytes(const jello_numeric_result* r) {
  const void* p = r->kind == JELLO_NUM_PTR                              ? r->as.ptr
                  : r->kind == JELLO_NUM_DYN && jello_is_ptr(r->as.dyn) ? jello_as_ptr(r->as.dyn)
                                                                        : NULL;
  return p && ((const jello_obj_header*)p)->kind == (uint8_t)JELLO_OBJ_BYTES ? (const jello_bytes*)p
                                                                             : NULL;
}

/** Makes `vm` run `m` with `m`'s own globals, as a top-level call does. */
static inline void vm_enter_module(jello_vm* vm, const jello_bc_module* m) {
  vm->running_module = m;
  vm->globals = m->globals;
}

/** The native stack JIT code leaves below native_stack_limit for what it calls: natives (PCRE2
 * matching alone takes 20 KiB), compiling a function lazily, and the collector. */
#define JIT_NATIVE_RESERVE (64u * 1024u)

/** The frame stack jello_vm_create maps, and the call frames it allows. */
#define VM_DEFAULT_FRAME_STACK_BYTES (64u * 1024u * 1024u)
#define VM_DEFAULT_CALL_FRAMES 1000000u
/** The most bytes of frame stack segments a VM maps above its first. */
#define VM_MAX_SEGMENT_BYTES (1024u * 1024u * 1024u)
/** The call frames a VM's first push makes room for, doubled as it needs more: few, as each takes
 * over 200 bytes and a parked process seldom has many. */
#define VM_FIRST_CALL_FRAMES 4u

/** The calling convention of JIT code's entry points and of the helpers it calls. x64 JIT code
 * keeps the System V convention on Windows too, so there JELLO_JIT_SYSV_THUNKS is defined and
 * these are sysv_abi; the other C functions JIT code calls keep the system's convention, as the
 * rest of the VM calls them, and JIT code reaches them through JIT_C. */
#if defined(_WIN32) && defined(__x86_64__)
#define JELLO_JIT_SYSV_THUNKS 1
#define JELLO_JIT_ABI __attribute__((sysv_abi))
#else
#define JELLO_JIT_ABI
#endif

/* Frame helpers that JIT code calls (frame.c). */

/** Builds the layout of every function in `m` and indexes it by function index. */
void vm_warm_jit_layouts(jello_vm* vm, const jello_bc_module* m);
/** Ensures room for a frame ending `top` bytes into the frame stack, committing more of it as
 * needed, and a free call_frame slot; returns 0 when `top` is past frame_stack_max or
 * committing or allocation fails. */
JELLO_JIT_ABI int jit_try_grow_push(jello_vm* vm, uint64_t top);
int jit_call_push_numeric(jello_vm* vm, uint32_t callee_fi, uint32_t caller_dst, uint32_t arg_first,
                          uint32_t nargs);

/** The top frame's register memory, or the frame stack's base when no frame is pushed. */
JELLO_JIT_ABI uint8_t* jit_current_frame_mem(jello_vm* vm);

/* Register loads and stores. Slots are 8-byte aligned, so plain casts are safe. */
static inline uint32_t vm_load_u32(reg_frame* rf, uint32_t r) {
  return *(const uint32_t*)vm_reg_ptr(rf, r);
}
static inline void vm_store_u32(reg_frame* rf, uint32_t r, uint32_t v) {
  *(uint32_t*)vm_reg_ptr(rf, r) = v;
}
static inline int64_t vm_load_i64(reg_frame* rf, uint32_t r) {
  return *(const int64_t*)vm_reg_ptr(rf, r);
}
static inline void vm_store_i64(reg_frame* rf, uint32_t r, int64_t v) {
  *(int64_t*)vm_reg_ptr(rf, r) = v;
}
static inline float vm_load_f32(reg_frame* rf, uint32_t r) {
  return *(const float*)vm_reg_ptr(rf, r);
}
static inline void vm_store_f32(reg_frame* rf, uint32_t r, float v) {
  *(float*)vm_reg_ptr(rf, r) = v;
}
static inline double vm_load_f64(reg_frame* rf, uint32_t r) {
  return *(const double*)vm_reg_ptr(rf, r);
}
static inline void vm_store_f64(reg_frame* rf, uint32_t r, double v) {
  *(double*)vm_reg_ptr(rf, r) = v;
}
static inline void* vm_load_ptr(reg_frame* rf, uint32_t r) {
  void* p;
  memcpy(&p, vm_reg_ptr(rf, r), sizeof(void*));
  return p;
}
static inline void vm_store_ptr(reg_frame* rf, uint32_t r, void* p) {
  memcpy(vm_reg_ptr(rf, r), &p, sizeof(void*));
}
static inline jello_value vm_load_val(reg_frame* rf, uint32_t r) {
  jello_value v;
  memcpy(&v, vm_reg_ptr(rf, r), sizeof(jello_value));
  return v;
}
static inline void vm_store_val(reg_frame* rf, uint32_t r, jello_value v) {
  memcpy(vm_reg_ptr(rf, r), &v, sizeof(jello_value));
}
static inline float vm_f32_from_bits(uint32_t bits) {
  float f;
  memcpy(&f, &bits, sizeof f);
  return f;
}
/** Stores `v` truncated to the width of kind `k`, so I8 and I16 registers can share I32 ops. */
static inline void vm_store_u32_masked(reg_frame* rf, uint32_t r, uint32_t v, jello_type_kind k) {
  uint32_t masked = v;
  if(k == JELLO_T_I8) masked = v & 0xFFu;
  else if(k == JELLO_T_I16) masked = v & 0xFFFFu;
  vm_store_u32(rf, r, masked);
}

/** The tag a register of kind `k` boxes to with its value in the Dynamic word itself, as
 * vm_box_from_typed boxes it: I8, I16 and I32 to I32, and Bool and Atom to their own; -1 for any
 * other kind. */
static inline int vm_kind_immediate_tag(jello_type_kind k) {
  switch(k) {
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32: return (int)JELLO_TAG_I32;
    case JELLO_T_BOOL: return (int)JELLO_TAG_BOOL;
    case JELLO_T_ATOM: return (int)JELLO_TAG_ATOM;
    default: return -1;
  }
}

/** How many low bits of an immediate's payload vm_store_from_boxed keeps for kind `k`: 8 and 16
 * for I8 and I16, as vm_store_u32_masked masks them, 1 for Bool and 32 otherwise. */
static inline uint32_t vm_kind_immediate_bits(jello_type_kind k) {
  switch(k) {
    case JELLO_T_I8: return 8u;
    case JELLO_T_I16: return 16u;
    case JELLO_T_BOOL: return 1u;
    default: return 32u;
  }
}

/** Widens IEEE-754 binary16 `bits` to float exactly. Shared with jdll/marshal.c so natives see
 * the same F16 values as the interpreter. */
float vm_f16_to_f32(uint16_t bits);
/** Narrows `f` to the nearest binary16, ties to even, as IEEE 754 does; small values become
 * subnormals or signed zero, large ones infinity, and a NaN stays a NaN. */
uint16_t vm_f32_to_f16(float f);

/* A float converts to I32 or I64, truncating, only when it lies strictly between that type's
 * bounds below, as doubles; NaN never does, and any other value traps ARITHMETIC. The I64 lower
 * bound is the double just below -2^63. */
#define VM_I32_FROM_FLOAT_LO (-2147483649.0)
#define VM_I32_FROM_FLOAT_HI 2147483648.0
#define VM_I64_FROM_FLOAT_LO (-9223372036854777856.0)
#define VM_I64_FROM_FLOAT_HI 9223372036854775808.0

/** Register `r`'s declared kind, looked up through `f`'s register types and `m`'s type table. */
jello_type_kind vm_reg_kind(const jello_bc_module* m, const jello_bc_function* f, uint32_t r);
/** Whether any of `f`'s registers holds a reference or Dynamic value, so its frame starts
 * as its layout's init image. */
int vm_func_has_pointer(const jello_bc_module* m, const jello_bc_function* f);
/** Gives `vm` the frame stack `mem` of `bytes`, which must not move while it is in use, and allows
 * it `call_frames` call frames; pushing past either traps STACK_OVERFLOW. `mem` lies within a
 * jello_pages_reserve reservation, whose pages pushes commit as they reach them. */
void vm_frame_stack_attach(jello_vm* vm, uint8_t* mem, uint32_t bytes, uint32_t call_frames);

/** Pushes a frame for `func_index`, copying registers `arg_first` to
 * `arg_first + arg_count - 1` of the current top frame into its registers from 0, at the
 * callee's register widths. Returns 0 with a STACK_OVERFLOW trap when the frame stack or the
 * call-frame limit is exhausted or an allocation fails. */
int vm_push_frame(jello_vm* vm, const jello_bc_module* m, uint32_t func_index, uint32_t caller_dst,
                  uint32_t arg_first, uint32_t arg_count, uint8_t has_caller);
/** Like vm_push_frame, but takes the arguments from `argbuf` and the captures from `capbuf`,
 * one REG_SLOT_BYTES slot each. TAILCALL and TAILCALLR copy their arguments out before popping
 * the frame that holds them. NULL buffers with zero counts pass nothing. */
int vm_push_frame_raw(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                      uint32_t caller_dst, uint8_t has_caller, const uint64_t* argbuf,
                      uint32_t arg_count, const uint64_t* capbuf, uint32_t ncaps);
/** CALLR's push: checks that `v` is a Function, pushes its frame as vm_push_frame does and copies
 * its captures into the registers from its function's `cap_start`. Returns the Function, or
 * NULL with a NULL_DEREF, TYPE_MISMATCH or STACK_OVERFLOW trap. */
/** The Function that CALLR or TAILCALLR value `v` calls, or NULL with a NULL_DEREF trap (null)
 * or a TYPE_MISMATCH one (not a Function). */
const jello_function* vm_callee_function(jello_vm* vm, jello_value v);
const jello_function* vm_push_callr_frame(jello_vm* vm, const jello_bc_module* m, jello_value v,
                                          uint32_t caller_dst, uint32_t arg_first,
                                          uint32_t arg_count);
/** Pops the top frame, freeing its spill slots and discarding its try scopes. */
void vm_pop_frame(jello_vm* vm);
/** Records in vm->trap_trace the call sites of the frames from the top down to index `stop`,
 * which a trap is leaving; nothing for a throw on its way to a catch. The top frame records the
 * pending site, if any (it is otherwise where the trap happened), and the others their calls. */
void vm_trace_frames(jello_vm* vm, uint32_t stop);
/** Forgets the last trap: its code, its detail and the calls that led to it. */
static inline void vm_clear_trap(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_NONE;
  vm->trap_detail[0] = '\0';
  vm->trap_trace.len = 0;
  vm->trap_trace.site = 0;
  vm->trap_trace.full = 0;
}
/** Pops every frame, leaving the frame stack empty. */
void vm_frame_unwind(jello_vm* vm);
/** Unmaps the frame stack segments `vm` mapped above its first, which no frame is in. */
void vm_frame_segments_free(jello_vm* vm);
/** Pops every frame, clears the trap and drops the layout cache, before a top-level call. */
void vm_frame_reset(jello_vm* vm);
/** Ends `vm`'s processes, a main process's, then frees the JIT module a run kept for them. */
void vm_end_kept(jello_vm* vm);
/** Drops `vm`'s layout cache, freeing it unless it is shared. */
void vm_free_layouts(jello_vm* vm);
/** Gives `vm` `from`'s layouts, which must all be built (vm_warm_jit_layouts) and outlive `vm`'s
 * use of them; `vm` then only reads them. */
void vm_share_layouts(jello_vm* vm, const jello_vm* from);

typedef enum op_result {
  OP_CONTINUE,
  OP_RETURN,
  OP_TRAP,
  OP_TRAP_LOCATED, /**< Trapped, and vm->trap_func_index and vm->trap_pc are already set. */
} op_result;

/** State shared by an interpreter loop and the cold-path handlers it dispatches to. */
typedef struct exec_ctx {
  jello_vm* vm;
  const jello_bc_module* m;
  jello_numeric_result* out;
  /* The loop returns once RET brings call_frames_len down to this, and a throw is caught by
   * this loop only above it. A loop the JIT starts for one frame (jit_run_interp_top) sets it to
   * the frame count below that frame; it is 0 for an outermost loop. */
  uint32_t stop_len;
} exec_ctx;

/** Runs a cold-path opcode handler for `ins`. */
op_result op_dispatch(exec_ctx* ctx, const jello_insn* ins);
jello_exec_status vm_exec_loop(exec_ctx* ctx);
/** Calls `fn` with the `nargs` arguments `args`, one register slot each, on `vm`, which has no
 * frames and runs its running_module, as JIT code from its active_jit when it has one. Stores the
 * result in `out`. */
jello_exec_status vm_exec_function(jello_vm* vm, const jello_function* fn, const uint64_t* args,
                                   uint32_t nargs, jello_numeric_result* out);

/* Numeric handlers outside the dispatch loop's inline cases (ops_arith.c). Integer division
 * and remainder trap ARITHMETIC on a zero divisor and on INT_MIN / -1. */
op_result op_div_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_u32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_u32(exec_ctx* ctx, const jello_insn* ins);
op_result op_shl_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_shr_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitand_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitor_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitxor_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_i32(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_u64(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_u64(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_shl_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_shr_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitand_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitor_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitxor_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_eq_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_u32(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_u64(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_f16(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_eq_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_f32(exec_ctx* ctx, const jello_insn* ins);

op_result op_sext_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_zext_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_fma_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_ldexp_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_copysign_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_sext_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_trunc_i8(exec_ctx* ctx, const jello_insn* ins);
op_result op_trunc_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f64_from_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_i64_from_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f64_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_i64_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f16_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_f16_from_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_f16(exec_ctx* ctx, const jello_insn* ins);

/* Bytes handlers (ops_bytes.c). */
op_result op_const_bytes(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_len(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_get_u8(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_set_u8(exec_ctx* ctx, const jello_insn* ins);

op_result op_bytes_concat2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_concat_many(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_join(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_eq(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_bitand2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_bitor2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_bitxor2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u16_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u16_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u16_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u16_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bits_write(exec_ctx* ctx, const jello_insn* ins);
op_result op_bits_read(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_blit(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_slice(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_freeze(exec_ctx* ctx, const jello_insn* ins);

/* Null and enum tests (ops_dyn.c, value/value.c). */
op_result op_is_null(exec_ctx* ctx, const jello_insn* ins);
op_result op_is_enum(exec_ctx* ctx, const jello_insn* ins);
/** Whether `v`, a Dynamic, holds an object that a register of pointer kind `k` and type `type`
 * may hold. The object's kind must match `k`, and for a struct, array, list, enum or function its
 * header must name exactly that type (a struct by its struct signature), because the type fixes
 * the object's layout. False for null. Shared by FROM_DYN_PTR and IS_ENUM. */
int vm_dynamic_fits_ptr(const jello_bc_module* m, jello_value v, jello_type_kind k,
                        jello_type_id type);
/** IS_ENUM: whether `v` holds an enum of type `type`; false for null. */
uint32_t vm_dynamic_is_enum(jello_value v, jello_type_id type);
op_result op_null_check(exec_ctx* ctx, const jello_insn* ins);

/** Boxes register `r`, of the kind `f` declares for it, as a jello_value. Allocates for I64,
 * F64, F32 and F16, so the GC may run. A null pointer register boxes as null. */
jello_value vm_box_from_typed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                              reg_frame* rf, uint32_t r);
/** Unboxes `v` into register `dst` at the kind `f` declares for it. I64, F32 and F64 registers
 * also accept an I32 value, converted; an F16 register takes its low 16 bits as binary16 bits.
 * Returns 0 with a NULL_DEREF trap for null into a scalar register, or a TYPE_MISMATCH trap when
 * `v` cannot be held there. */
int vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                        reg_frame* rf, uint32_t dst, jello_value v);
/** KINDOF: 0 for null, 1 Bool, 2 I32, 3 Atom, then 4 to 15 for a Bytes, Struct, Object, boxed
 * I64, F64, F32 or F16, Array, Function, List, Enum or native handle, and 0x7fffffff for anything
 * else. Never traps. */
uint32_t vm_kindof_dynamic(jello_value v);
/** SWITCH_KIND: the jello_type_kind of `v`'s runtime representation. A boxed number reports
 * its numeric kind, a native handle reports Abstract, and null reports Dynamic. Using
 * jello_type_kind's numbering lets a compiler emit a pattern's static kind directly. */
uint32_t vm_dynamic_static_kind(jello_value v);

/** Stores `o`'s own value for `atom_id` in `*out` and returns 1, or returns 0 when absent; one
 * probe where jello_object_has then jello_object_get take two. With `keep` the caller may keep
 * the value, so a slot that owned its F64 box gives it up; without, the caller unboxes it at
 * once. */
int vm_object_find(const jello_object* o, uint32_t atom_id, int keep, jello_value* out);
/** Sets `o`'s own `atom_id` to the F64 `d`, writing into the slot's box when the slot owns one
 * and otherwise storing a new box the slot owns. May collect; `o` must be reachable. */
void vm_object_set_f64(jello_vm* vm, jello_object* o, uint32_t atom_id, double d);
/** OBJ_GET_ATOM and OBJ_GET for JIT code; reads `atom_id` through the prototype chain, keeping
 * the value (vm_object_find) for a Dynamic destination.
 * Clears vm->trap_code first and returns null with a BOUNDS trap when the chain is too long, so
 * the caller checks trap_code after the call. */
JELLO_JIT_ABI jello_value jit_obj_chain_get(jello_vm* vm, const jello_bc_module* m, jello_object* o,
                                            uint32_t atom_id, int keep);
/** OBJ_HAS_ATOM and OBJ_HAS for JIT code; whether `o` has `atom_id` through the prototype chain,
 * as jit_obj_chain_get finds it. Clears vm->trap_code first and returns 0 with a BOUNDS trap
 * when the chain is too long. */
JELLO_JIT_ABI uint32_t jit_obj_chain_has(jello_vm* vm, const jello_bc_module* m,
                                         const jello_object* o, uint32_t atom_id);
/** OBJ_SET_ATOM and OBJ_SET for JIT code; sets `o`'s own slot, updating its prototype link when
 * `atom_id` is __proto__. Clears vm->trap_code first and sets a SEALED trap, writing nothing,
 * when the write would change a sealed method. */
JELLO_JIT_ABI void jit_obj_set_direct(jello_vm* vm, const jello_bc_module* m, jello_object* o,
                                      uint32_t atom_id, jello_value v);
/** OBJ_SET_ATOM and OBJ_SET of an F64 for interpreter and JIT code; as jit_obj_set_direct, without
 * allocating when the slot owns its box. May collect. */
JELLO_JIT_ABI void jit_obj_set_f64(jello_vm* vm, const jello_bc_module* m, jello_object* o,
                                   uint32_t atom_id, double d);
/** OBJ_REMOVE for JIT code; whether `o` had `atom_id`, which it no longer has. Clears
 * vm->trap_code first and sets a SEALED trap, removing nothing, when the slot is sealed. */
JELLO_JIT_ABI uint32_t jit_obj_remove(jello_vm* vm, jello_object* o, uint32_t atom_id);
/** OBJ_CLEAR for JIT code; as jit_obj_remove, for every slot of `o`. */
JELLO_JIT_ABI void jit_obj_clear(jello_vm* vm, jello_object* o);
/** The state of `o`'s slot for `atom_id`; JELLO_OBJ_SLOT_EMPTY when it has none. */
uint8_t vm_object_state(const jello_object* o, uint32_t atom_id);

/* Dynamic, function-value and call handlers (ops_dyn.c). */
op_result op_to_dyn(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i8(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_bool(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_ptr(exec_ctx* ctx, const jello_insn* ins);
op_result op_kindof(exec_ctx* ctx, const jello_insn* ins);
op_result op_physeq(exec_ctx* ctx, const jello_insn* ins);
op_result op_const_null(exec_ctx* ctx, const jello_insn* ins);
op_result op_const_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_const_fun(exec_ctx* ctx, const jello_insn* ins);
op_result op_closure(exec_ctx* ctx, const jello_insn* ins);
op_result op_callr(exec_ctx* ctx, const jello_insn* ins);
op_result op_tailcall(exec_ctx* ctx, const jello_insn* ins);
op_result op_tailcallr(exec_ctx* ctx, const jello_insn* ins);
op_result op_spill_push(exec_ctx* ctx, const jello_insn* ins);
op_result op_spill_pop(exec_ctx* ctx, const jello_insn* ins);
op_result op_switch_kind(exec_ctx* ctx, const jello_insn* ins);
op_result op_case_kind(exec_ctx* ctx, const jello_insn* ins);

op_result op_global_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_global_set(exec_ctx* ctx, const jello_insn* ins);

op_result op_struct_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_struct_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_struct_set(exec_ctx* ctx, const jello_insn* ins);

/* Array (ops_array.c) and List (ops_list.c) handlers. */
op_result op_array_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_len(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_set(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_resize(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_concat2(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_nil(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_cons(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_head(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_tail(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_is_nil(exec_ctx* ctx, const jello_insn* ins);

op_result op_enum_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_enum_tag(exec_ctx* ctx, const jello_insn* ins);
op_result op_enum_get(exec_ctx* ctx, const jello_insn* ins);

op_result op_try(exec_ctx* ctx, const jello_insn* ins);
op_result op_endtry(exec_ctx* ctx, const jello_insn* ins);
op_result op_throw(exec_ctx* ctx, const jello_insn* ins);

/** Internal and never reported; a caught throw is unwinding to the frame that catches it,
 * leaving vm->unwind_len frames once it arrives. */
#define JELLO_TRAP_INTERNAL_UNWIND ((jello_trap_code)0x7F)

/** TRY on the top frame, also called by JIT code; opens a scope whose handler is at absolute pc
 * `catch_pc` and receives the payload in `payload_reg`. Returns 0 with a STACK_OVERFLOW trap
 * when there is no memory for it; the loader keeps a frame's scopes within JELLO_TRY_STACK_MAX. */
int vm_try_push(jello_vm* vm, uint32_t catch_pc, uint32_t payload_reg);
/** ENDTRY on the top frame; closes its innermost scope, if any. */
void vm_try_pop(jello_vm* vm);

/** Finds the innermost open try scope on any frame. If there is one, closes it, stores `payload`
 * in its payload register, points that frame's pc at the handler, sets vm->unwind_len to the
 * frame count down to and including that frame, and returns 1; no frame is popped. Returns 0
 * when the throw is uncaught. */
int vm_throw_find(jello_vm* vm, jello_value payload);

/** After a callee activation returns with a trap; if the trap is an unwind whose catching frame
 * lies above `ctx->stop_len`, pops down to that frame, clears the trap and returns 1, and the
 * loop resumes at the handler. Returns 0 otherwise. */
int vm_catch_in_segment(exec_ctx* ctx);

op_result op_obj_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_has_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_get_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_set_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_set(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_has(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_remove(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_clear(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_copy(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_keys(exec_ctx* ctx, const jello_insn* ins);

/** A shallow copy of `src`, which is left as it is, so that several threads may copy it at once;
 * the caller replaces each of the copy's values, boxes included, with a copy of its own. */
jello_object* vm_object_clone(jello_vm* vm, const jello_object* src);

/** The Objects of a copy vm_parcel_pack made whose prototype was held in a global of the process
 * that made it; each is to delegate to the receiving process's value of that global. */
typedef struct vm_protos {
  uint32_t len;
  struct vm_proto_fix {
    jello_object* object;
    uint32_t global;
  } at[];
} vm_protos;
/** A copy of a value for another process, in no space: the blocks of the objects copied for it,
 * which no collector traces or frees until the receiver takes them into its space, and the
 * prototypes it is to point at its own. */
typedef struct vm_parcel {
  jello_value value;
  vm_protos* protos;     /**< Its Objects to delegate to the receiver's globals, or NULL. */
  jello_gc_hdr* objects; /**< The blocks copied for it, linked as a space's are. */
  jello_gc_hdr* tail;    /**< The last of them. */
  uint64_t bytes;        /**< What they count live, as alloc counts it. */
  /** The sockets it moves, which its taker owns then, and which close when it is freed untaken;
   * NULL for none. */
  struct proc_socket_list* sockets;
} vm_parcel;
/** A parcel holding nothing, its value null. */
static inline vm_parcel vm_parcel_empty(void) { return (vm_parcel){.value = jello_make_null()}; }
/** Moves `from` to `to`, leaving `from` empty. */
static inline void vm_parcel_move(vm_parcel* to, vm_parcel* from) {
  *to = *from;
  *from = vm_parcel_empty();
}
/** Copies `v` for another process into *out, sharing only immortal objects, its native handles
 * new ones holding the same payloads. An Object whose prototype one of `vm`'s globals holds
 * delegates to nothing until the receiver takes it. Any other prototype is copied. A handle whose
 * class has no `retain` is copied as null when `drop`, and refused otherwise. Never collects, as
 * it allocates nothing in `vm`'s space.
 * @return JELLO_TRAP_UNSENDABLE, leaving *out empty, when it refused one; else JELLO_TRAP_NONE */
jello_trap_code vm_parcel_pack(jello_vm* vm, jello_value v, int drop, vm_parcel* out);
/** The value of `pc`, its objects moved into `vm`'s space and its Objects pointed at `vm`'s
 * values of their globals, leaving `pc` empty. */
jello_value vm_parcel_take(jello_vm* vm, vm_parcel* pc);
/** A copy of the value of `pc` in `vm`'s space, whose Objects delegate to `vm`'s values of their
 * globals; `pc` is left as it was, for other reads, which may run at once, but for the sockets it
 * moves, which the first read takes. Never collects. */
jello_value vm_parcel_read(jello_vm* vm, vm_parcel* pc);
/** Moves the objects of `pc` into `space`, for `vm`, which may be running and takes the heap's
 * lock, leaving `pc` none; vm_parcel_take's first step, which leaves the prototypes to fix. */
void vm_parcel_splice(jello_vm* vm, jello_space* space, vm_parcel* pc);
/** Adds the objects of `from` to `into`, to free together, dropping its value and prototypes and
 * leaving it empty. Runs no finaliser, so a caller holding a lock can free them later. */
void vm_parcel_join(vm_parcel* into, vm_parcel* from);
/** Frees what `pc` holds, finalising its handles and closing the sockets it moves, leaving it
 * empty. */
void vm_parcel_free(vm_parcel* pc);
/** Adds socket `payload`, met while copying, to *list, which it makes when NULL; whether it could.
 */
int proc_socket_note(struct proc_socket_list** list, void* payload);
/** Keeps of *list, one a copy noted, the sockets `vm`'s process owns, taking them from it, and
 * frees the rest of it, leaving *list NULL when none is left. */
void proc_sockets_pack(const jello_vm* vm, struct proc_socket_list** list);
/** Gives `p` the sockets *list moves, closing them when `p` is NULL, and frees it, leaving *list
 * NULL; one call takes them, though several race. */
void proc_sockets_take(struct jello_process* p, struct proc_socket_list** list);
/** Adds the sockets *from moves to *into, which allocates nothing, leaving *from NULL. */
void proc_sockets_join(struct proc_socket_list** into, struct proc_socket_list** from);
/** Replaces the function `*root` and what each of `globals`, a table laid out as `vm`'s
 * running_module's, leads to with a copy in `into`, a new process's space, copying them as one
 * graph, so that what they share, prototypes too, stays shared. A handle whose class has no
 * `retain` is copied as null in the globals. Sets *sockets to those the function's copy takes
 * from `vm`'s process, for the new process to own. Never collects.
 * @return JELLO_TRAP_UNSENDABLE when the function captures such a handle; else JELLO_TRAP_NONE */
jello_trap_code vm_copy_start(jello_vm* vm, jello_space* into, void** root, uint64_t* globals,
                              struct proc_socket_list** sockets);

/** The type_id of a binary that may be written, as a ByteBuffer; 0 for one that never is. */
#define JELLO_BYTES_BUFFER 1u
/** Whether Bytes `b` may be written, as a buffer; a slice never is one, since slicing a buffer
 * copies it. Every other Bytes may be shared, between processes too, so writing one traps. */
static inline int vm_bytes_is_buffer(const jello_bytes* b) {
  return b->h.type_id == JELLO_BYTES_BUFFER;
}
/** Allocates a binary of `length` zero bytes that may be written, as a ByteBuffer. May collect;
 * aborts when out of memory. */
jello_bytes* vm_buffer_new(jello_vm* vm, uint32_t length);
/** A Bytes holding what Bytes `b` holds, for another process: a buffer's copy is a buffer, a
 * Bytes of at least VM_BYTES_SHARED_MIN bytes in a shared binary or an immortal one shares them,
 * and any other is copied. Allocates as jello_bytes_new does, into `vm`'s parcel while it packs. */
jello_bytes* vm_bytes_clone(jello_vm* vm, const jello_bytes* b);
/** The shared binaries not yet freed, in every heap. */
uint32_t vm_shared_binaries_live(void);
/** A new box holding what box `v` holds, or `v` itself when it is no box. May collect. */
jello_value vm_clone_numbox(jello_vm* vm, jello_value v);
/** `size` zero bytes for an immortal object, whose jello_obj_header the caller fills in. Aborts
 * when out of memory. */
void* vm_immortal_alloc(size_t size);
/** The 8-byte aligned bytes an immortal binary of `length` bytes takes, header included. */
size_t vm_bytes_immortal_size(uint32_t length);
/** An immortal binary of `length` bytes for the caller to fill in, made in the
 * vm_bytes_immortal_size(length) zero bytes at `at`, 8-byte aligned, which the caller frees. */
jello_bytes* vm_bytes_immortal_at(void* at, uint32_t length);
/** An immortal Function of type `type` without captures. Aborts when out of memory. */
jello_function* vm_function_immortal(uint32_t func_index, jello_type_id type);
/** An immortal enum value of variant `tag`, without fields. Aborts when out of memory. */
jello_enum* vm_enum_immortal(uint32_t type_id, uint32_t tag);
/** Frees every object of `space`, finalising its handles, and its free lists. */
void vm_space_free(jello_space* space);
/** Frees immortal object `p`, which may be NULL, once nothing can reach it. */
void vm_immortal_free(void* p);
/** The bytes of a struct of signature `struct_sig_id`'s fields, a multiple of 8. */
uint32_t vm_struct_fields_bytes(const jello_bc_module* m, uint32_t struct_sig_id);

/** Creates a VM for another thread that shares `existing`'s heap. Every VM on a heap must run the
 * same module. Outside a run, its thread touches the heap only under jello_vm_running. Returns
 * NULL when `existing` has no heap or allocation fails. */
jello_vm* jello_vm_create_joined(jello_vm* existing);
/** The in_exec jello_vm_running sets: a stop of the world waits for it, but a reload may patch,
 * as it runs no module's code. */
#define VM_RUNNING_OUTSIDE 2
/** Marks the calling thread as using `vm`'s heap outside a run (`is_running` nonzero) or done
 * (zero), so that a stop of the world waits for its next safepoint rather than sweeping an object
 * it is still filling in. Kept to allocating work: until the thread next allocates, a stop waits.
 * Not nested, nor inside or around a run; aborts when unbalanced. */
void jello_vm_running(jello_vm* vm, int is_running);
/** Creates a VM sharing `existing`'s heap, as jello_vm_create_joined does, on the frame stack
 * `mem` of `bytes`, which the caller owns, with `call_frames` call frames; with a space of its
 * own when `own_space`, for a process, rather than `existing`'s. */
jello_vm* vm_create_attached(jello_vm* existing, uint8_t* mem, uint32_t bytes, uint32_t call_frames,
                             int own_space);
/** Destroys a VM made by vm_create_attached, leaving its frame stack to its owner. */
void vm_destroy_attached(jello_vm* vm);

/* A thread brackets a region in which it does not touch the heap (a native call,
 * jello_vm_blocking) with vm_go_quiescent and vm_leave_quiescent; the collector does not wait
 * for a quiescent thread. vm_wait_for_resume blocks until a pending stop-the-world ends;
 * vm_leave_quiescent waits there before it decrements, so a thread never runs while the world
 * is stopped. */
void vm_go_quiescent(jello_vm* vm);
void vm_wait_for_resume(jello_vm* vm);
void vm_leave_quiescent(jello_vm* vm);

/** Parks the thread until a pending stop-the-world ends; one atomic load when none is pending.
 * Called on allocation, through vm_poll_and_charge, and on entry to a top-level call. */
void vm_safepoint_check(jello_vm* vm);

/** The reductions a window holds unless less fuel is left. */
#define VM_REDUCTIONS 2000u

/** The charge that ends `vm`'s window of reductions. It settles the window against the fuel and
 * starts the next, and when `vm` runs a process, lets the next waiting process run first.
 * Returns 0 to continue, or nonzero with a JELLO_TRAP_FUEL trap when this charge is past the
 * fuel, or JELLO_TRAP_STOPPED when the process is stopping. Allocation reaches a safepoint
 * without a charge. */
int vm_reductions_out(jello_vm* vm);
/** The end of a window of reductions for `vm`'s process: traps JELLO_TRAP_STOPPED when it is
 * stopping, and otherwise, unless `trapped`, lets the next process waiting run first. Returns
 * nonzero when it traps, and otherwise `trapped`. */
int proc_window_end(jello_vm* vm, int trapped);
/** Gives `vm` the fuel `from` has left, counting what it has used in its current window, and the
 * same limit, so that `vm` traps at once when `from` has none left. */
void vm_inherit_fuel(jello_vm* vm, const jello_vm* from);

/** The check at every possible loop back-edge; a backward JMP or JMP_IF, a CALL, CALLR, TAILCALL
 * or TAILCALLR, and a THROW. It polls for a stop-the-world and charges a reduction, so code that
 * never allocates still reaches a safepoint and cannot outrun a fuel limit. Returns 0, or
 * vm_reductions_out's result for the charge that ends a window. */
static inline int vm_poll_and_charge(jello_vm* vm) {
  if(atomic_load(&vm->heap->stop_requested)) vm_safepoint_check(vm);
  return --vm->reductions > 0 ? 0 : vm_reductions_out(vm);
}

/** Stops every other thread on the heap, for the collector or a JIT module reload. Waits until
 * each one is quiescent or outside jello_vm_exec_numeric, and holds alloc_lock until
 * vm_heap_resume_the_world. Returns 1 when this call stopped the world. Returns 0 when another
 * thread's stop was in progress; this thread waited for it to end, nothing is stopped, and the
 * caller must not proceed. */
int vm_heap_stop_the_world(jello_vm* vm);
/** Takes the heap's alloc_lock for `vm`, which may be running: it counts as quiescent while it
 * waits, so that a stop-the-world, which holds the lock while it waits for every running VM,
 * does not wait for it in turn. Allocation sites are safepoints, so its frames are complete. */
void vm_heap_lock(jello_vm* vm);
void vm_heap_resume_the_world(jello_vm* vm);

typedef struct jello_jdll_handle jello_jdll_handle;

/** Writes the running executable's path, links resolved, to `out`, of PATH_MAX bytes; whether it
 * is known. */
int jello_executable_path(char* out);
/** Writes the directory of `path` to `dir`, of PATH_MAX bytes: "." for a bare name, "/" for one
 * in the root.
 * @return whether it fits */
int jello_directory_of(const char* path, char* dir);
/** Sets the directories that jello_jdll_load searches first: `jdll/` beside the file `program`,
 * then its own directory, unless `program` is NULL, then those of the list `dirs`, separated by
 * JELLO_PATH_LIST_SEP, unless NULL. A host names its program here before loading it.
 * @return whether the directories fit */
int jello_jdll_search_beside(const char* program, const char* dirs);
/** Loads `<library_name>.jdll` from the first directory that has it, of those
 * jello_jdll_set_search named, the working directory, each of the list JELLO_JDLL_PATH, the
 * executable's directory and `../lib` from there. It then checks the
 * `jello_jdll_manifest_<library_name>` symbol's magic, ABI version and library name. "vm" is the
 * built-in library instead, never a file. Returns NULL on any failure, its reason in
 * jello_jdll_last_error. */
jello_jdll_handle* jello_jdll_load(const char* library_name);
/** Why this thread's last jello_jdll_load failed; overwritten by its next. */
const char* jello_jdll_last_error(void);
/** The data or function `name` that `h`'s file exports; NULL when it has none, or is the
 * built-in `vm` library. */
void* jello_jdll_symbol(const jello_jdll_handle* h, const char* name);
/** The path `h` was loaded from; NULL for the built-in `vm` library, which has no file. */
const char* jello_jdll_path(const jello_jdll_handle* h);
void jello_jdll_unload(jello_jdll_handle* h);
/** The export named `name`, or NULL if the library has none. */
const struct jello_jdll_export* jello_jdll_find_export(const jello_jdll_handle* h,
                                                       const char* name);

op_result op_call_native(exec_ctx* ctx, const jello_insn* ins);

#endif /* JELLO_VM_INTERNAL_H */
