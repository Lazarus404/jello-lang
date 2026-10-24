// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * JIT module state; the per-module code blob, per-function compile records, lazy compilation
 * in mixed mode, the crossings between interpreter and JIT code, and hot reload. */

#ifndef JELLO_JIT_INTERNAL_H
#define JELLO_JIT_INTERNAL_H

#include <jello/internal/jit_impl.h>

/** A function compiled alone into a private buffer by jello_jit_compile_function. */
typedef struct jello_jit_code {
  jit_exec_buf buf;
  size_t entry_off;
  size_t body_off;
  uint32_t* loop_anchor_pcs; /**< Bytecode pcs of backward-branch targets, `nloop_anchors` long. */
  size_t* loop_anchor_offs;  /**< Native offset of each loop anchor. */
  uint32_t nloop_anchors;
} jello_jit_code;

/** Why a function was not compiled. The JIT runs it in the interpreter instead. */
typedef enum jello_jit_decline {
  JIT_DECLINE_NONE = 0,
  JIT_DECLINE_IR_BUILD, /**< The bytecode has no JIR form. */
  JIT_DECLINE_VERIFY,   /**< The JIR failed jello_jit_ir_verify. */
  JIT_DECLINE_SHAPE,    /**< jit_ir_shape_ok refused it. */
  JIT_DECLINE_SURVIVE,  /**< Call-survive planning failed. */
  JIT_DECLINE_EMIT,     /**< Emission failed; an op the backend cannot emit, a full buffer, or
                         * (x64) a call-patch list that could not grow. */
  JIT_DECLINE_OOM,
} jello_jit_decline;

static inline const char* jello_jit_decline_name(jello_jit_decline d) {
  switch(d) {
    case JIT_DECLINE_NONE: return "none";
    case JIT_DECLINE_IR_BUILD: return "ir-build";
    case JIT_DECLINE_VERIFY: return "verify";
    case JIT_DECLINE_SHAPE: return "shape";
    case JIT_DECLINE_SURVIVE: return "survive";
    case JIT_DECLINE_EMIT: return "emit";
    case JIT_DECLINE_OOM: return "oom";
  }
  return "unknown";
}

/** A module function's compile record. The function's code starts at `blob_off` in the module
 * blob; `entry_off` and `body_off` are relative to it. */
typedef struct jello_jit_func_code {
  size_t blob_off, entry_off, body_off, size;
  uint32_t* loop_anchor_pcs;
  size_t* loop_anchor_offs;
  uint32_t nloop_anchors;
  uint8_t compiled;
  uint8_t tried;          /**< Compiled or declined; set once. */
  uint8_t decline_reason; /**< A jello_jit_decline; NONE when compiled. */
  uint16_t decline_op;    /**< Bytecode opcode at decline_pc. */
  uint32_t decline_pc;    /**< UINT32_MAX when no single instruction is to blame. */
  uint32_t calls;         /**< Calls counted towards JIT_COMPILE_AFTER_CALLS while not tried. */
} jello_jit_func_code;

/** Mixed mode compiles a function on the call that brings its call count to this. */
#define JIT_COMPILE_AFTER_CALLS 1u

/** A bytecode module's JIT code; one executable blob holding every compiled function. */
typedef struct jello_jit_module {
  jit_exec_buf blob;
  jello_jit_func_code* funcs;
  uint32_t nfuncs;
  int prepared;
  /** Functions compile one by one, and declined ones run in the interpreter. A JIT call to a
   * declined function lands on the bridge stub; the interpreter enters a compiled callee through
   * the enter stub. */
  uint32_t ncompiled;
  size_t bridge_off, enter_off, lazy_off;
  /** Each function's current call target, which JIT calls load; its body, the bridge, or the
   * lazy stub until it is tried. One more entry, past the functions', is the bridge, for a call
   * too deep for the native stack. */
  const void** entries;
  const jello_bc_module* m;
} jello_jit_module;

/** Enter stub; saves the callee-saved registers JIT code uses, runs `body` (a compiled
 * function's body) on the topmost frame, whose register memory is `frame_base`, and restores
 * them. A trap is left in vm->trap_code. */
typedef void(JELLO_JIT_ABI* jello_jit_enter_fn)(jello_vm* vm, uint8_t* frame_base,
                                                jello_numeric_result* out, const void* body);

/** Emits the enter, bridge and lazy stubs and stores their offsets. Returns 0 when the buffer
 * overflows. */
int jello_jit_emit_mixed_stubs(jit_exec_buf* buf, size_t* enter_off, size_t* bridge_off,
                               size_t* lazy_off);

/** Compiles function `fi` of a module unless it was already tried, and points its entry
 * at the body or, when it declines, the bridge. Returns whether it is compiled.
 * @note Aborts if an append window cannot open or close. */
int jit_compile_function_now(jello_jit_module* jm, uint32_t fi);
/** Tests only; when nonzero, the first append window's size, so a function outgrows it and
 * takes the retry path. */
extern size_t jit_append_window_bytes;

/** The lazy stub's C half, for the topmost frame (pushed by a JIT call); counts the call,
 * compiles the function when due, and returns the address to run it at, its body or the
 * bridge. */
JELLO_JIT_ABI const void* jit_lazy_target(jello_vm* vm);

/** The bridge stub's C half; runs the topmost frame, which JIT code pushed for a declined
 * callee, in a nested interpreter loop until it returns. On a trap it pops that frame and
 * every frame above it, as a JIT callee's trap tail does. */
JELLO_JIT_ABI void jit_run_interp_top(jello_vm* vm);
/** JIR_CALLR's push half, mirroring op_callr; checks the callee value in `callee_reg`, pushes
 * its frame and copies its captures. Returns the callee's call target from the entries table,
 * or NULL with vm->trap_code set. */
JELLO_JIT_ABI const void* jit_callr_push(jello_vm* vm, uint32_t dst, uint32_t callee_reg,
                                         uint32_t arg_first, uint32_t nargs);
/** JIR_THROW, with the payload in register `payload_reg` of the topmost frame (function `fi`,
 * bytecode `pc`). Leaves either the unwind sentinel, for the catching activation to pick up,
 * or an uncaught-throw trap located at (`fi`, `pc`). */
JELLO_JIT_ABI void jit_throw(jello_vm* vm, uint32_t payload_reg, uint32_t fi, uint32_t pc);
/** Aborts; the unwind sentinel reached its activation, but the frame's pc is none of its
 * handlers, so the try bookkeeping is corrupt. */
JELLO_JIT_ABI void jit_unwind_lost(jello_vm* vm);

/** Called by the interpreter right after it pushes a callee frame. Runs the callee as JIT
 * code when it is compiled (compiling it when due) and the C stack is above
 * vm->jit_stack_floor. Returns 0 when it did not enter (the interpreter runs the frame), 1
 * when the callee ran and returned, 2 when it trapped (vm->trap_* describe the trap). A throw
 * that crosses the activation unwinds it like a trap. */
int jello_jit_enter_pushed_frame(jello_vm* vm);

/** Sets `vm`'s jit_stack_floor and native_stack_limit for this thread's stack. The interpreter
 * enters JIT code only above a quarter of the stack (at least 64 KiB), and JIT code calls only
 * above JIT_NATIVE_RESERVE. Where the stack's bounds are unknown, the interpreter never enters
 * JIT code. */
void jit_native_stack_here(jello_vm* vm);

/** An outermost function's entry point (entry_off); `frame_base` is its register memory and
 * `out` receives its result. */
typedef jello_exec_status(JELLO_JIT_ABI* jello_jit_entry_fn)(jello_vm* vm, uint8_t* frame_base,
                                                             jello_numeric_result* out);

/** Compiles function `fi` alone, with calls disallowed, into a fresh buffer. Returns 0 when it
 * declines or memory runs out. */
int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code);
void jello_jit_code_free(jello_jit_code* code);

/** Counts module preparations. */
extern _Atomic uint32_t jello_jit_module_prepare_count;

/** Prepares `out` with every function compiled up front. Declined functions run in the
 * interpreter, and preparation fails only when memory runs out or nothing compiled.
 * @note Ordinary runs prepare lazily (jello_jit_module_prepare_lazy), compiling each function on
 * its first call. The eager form serves jello --require-jit and hot reload. */
int jello_jit_module_prepare(const jello_bc_module* m, jello_jit_module* out);
/** Prepares `out` to compile functions as they are called. */
int jello_jit_module_prepare_lazy(const jello_bc_module* m, jello_jit_module* out);
/** Allocates and prepares a module for `m`, lazily, or eagerly when `eager` is set (as jello
 * --require-jit needs, to check every function). Returns NULL on failure. */
jello_jit_module* jit_module_create(const jello_bc_module* m, int eager);
void jello_jit_module_free(jello_jit_module* jm);
/** Runs the topmost frame as JIT code when it is an outermost call at pc 0 whose function is
 * compiled (compiling it first). Returns 1 with the status in `*out_st` when it
 * ran, 0 when the interpreter should run the frame. */
int jello_jit_try_enter(jello_vm* vm, jello_jit_module* jm, jello_numeric_result* out,
                        jello_exec_status* out_st);

/** Counts entries into JIT code through jello_jit_try_enter. */
extern uint32_t jit_try_enter_count;

/** Emits function `fi` into `buf` and fills `out_fc`, or records the decline in `out_fc` and
 * returns 0. Calls load their target from the module's `entries` table; without one, a
 * function that calls declines. */
int jello_jit_emit_function_into(const jello_bc_module* m, uint32_t fi, jit_exec_buf* buf,
                                 jello_jit_func_code* out_fc, const void* const* entries);

/** Resolves a branch at `bc_pc` with offset `imm` (from the next instruction) into `*out`.
 * Returns 0 when the target is outside the function's `ninsns` instructions. */
int jit_resolve_target_bc(uint32_t bc_pc, int32_t imm, uint32_t ninsns, uint32_t* out);
/** Checks `ir` against the op policy both backends share; every op allowed, every JIR_SLOW
 * runnable through its handler, every branch target and callee valid, and every RET of one kind
 * (never F16). Stores the return kind in `*out_ret_kind`, VOID for a function that never
 * returns, such as a process's loop. On failure `*bad_pc` is the refused instruction's bytecode
 * pc, or UINT32_MAX when no single instruction is to blame. */
int jit_ir_shape_ok(const jello_jit_ir_function* ir, const jello_bc_module* m,
                    const jello_bc_function* f, uint32_t fi, jello_type_kind* out_ret_kind,
                    int allow_calls, uint32_t* bad_pc);
/** Scans the branches of `ir`, which jit_ir_shape_ok accepted, over zeroed `ninsns`-byte maps.
 * In `is_target`, bit 0 marks a branch target and bit 1 a TRY handler; both are entered with
 * every register in its home. `is_loop_anchor` marks backward-branch targets, counted in
 * `*out_nloop_anchors`. `*out_catch_pcs` receives a malloc'd array of the `*out_ncatch` handler
 * pcs in ascending order. Returns 0 only when memory runs out. */
int jit_scan_targets(const jello_jit_ir_function* ir, uint32_t ninsns, uint8_t* is_target,
                     uint8_t* is_loop_anchor, uint32_t* out_nloop_anchors, uint32_t** out_catch_pcs,
                     uint32_t* out_ncatch);
/** Records in `fc` that it declined for `why` at bytecode `pc` of `f`, and returns 0. */
int jit_decline(jello_jit_func_code* fc, jello_jit_decline why, const jello_bc_function* f,
                uint32_t pc);

/** Appends the `elem`-byte `item` to the growable array whose pointer variable is at
 * `items_ptr` (a T**), doubling it as it fills. Returns 0, leaving the array as it was, only
 * when memory runs out. */
int jit_list_push(void* items_ptr, size_t* count, size_t* cap, const void* item, size_t elem);

/** How a function compares across a reload. Functions are matched by name. */
typedef enum jello_reload_fn_status {
  JELLO_RELOAD_FN_UNCHANGED,          /**< Same signature and body hash. */
  JELLO_RELOAD_FN_CHANGED,            /**< Same signature, different body hash. */
  JELLO_RELOAD_FN_SIGNATURE_CONFLICT, /**< Return, argument count or an argument type differs. */
  JELLO_RELOAD_FN_NEW,                /**< No function of this name in the old module. */
  JELLO_RELOAD_FN_REMOVED,            /**< No function of this name in the new module. */
} jello_reload_fn_status;

/** Compares one function's old and new bytecode, which the caller matched by name. Types are
 * compared by their full shape, never by type id. Compiles nothing. */
jello_reload_fn_status jello_reload_analyze_function(const jello_bc_module* old_m,
                                                     const jello_bc_function* old_f,
                                                     const jello_bc_module* new_m,
                                                     const jello_bc_function* new_f);

/** Whether struct sig `struct_sig_id` differs between the modules in field count, naming,
 * mutability, field types or field names; true when either module lacks it. Struct sigs have
 * no name, so they are matched by position; a reordered struct table reads as changes. */
int jello_reload_struct_sig_changed(const jello_bc_module* old_m, const jello_bc_module* new_m,
                                    uint32_t struct_sig_id);
/** Whether some register of `f` has a struct type with sig `struct_sig_id`. */
int jello_reload_function_uses_struct_sig(const jello_bc_module* m, const jello_bc_function* f,
                                          uint32_t struct_sig_id);

/** Stops the world for every thread sharing `vm`'s heap and checks that none is executing.
 * Returns 1 with the world paused, to be ended by jello_jit_reload_resume_all, or 0 with it
 * running again; another stop-the-world was in flight or some thread was executing. */
int jello_jit_reload_pause_all(jello_vm* vm);
void jello_jit_reload_resume_all(jello_vm* vm);

/** Result of jello_jit_module_reload. Removed functions have no index in the new module, so
 * they are only counted. */
typedef struct jello_jit_reload_report {
  uint32_t n_unchanged, n_changed, n_new, n_removed, n_signature_conflicts, n_type_conflicts;
  jello_reload_fn_status* fn_status; /**< [new_m->nfuncs], owned. */
} jello_jit_reload_report;

/** Replaces `jm`'s code, compiled from `old_m`, with code compiled from `new_m`. The swap is
 * all or nothing; the whole new module is compiled first, swapped in while every thread is
 * paused and idle, and the old blob freed. Unchanged functions are recompiled too, since code
 * points into the module it was compiled from and a removal shifts function indices. A function
 * that uses a struct whose shape changed counts as a type conflict.
 * @return 1 when the reload was applied or `jm` was already compiled from `new_m`; 0 with `jm`
 * untouched on bad arguments, a refused conflict, a failed compile, memory exhaustion or no idle
 * pause.
 * @note `*out_report` is zeroed first, then holds the per-function analysis whenever it ran.
 * With `allow_conflicts` set conflicting functions are swapped in too; live heap objects are
 * never migrated, so struct data already allocated keeps its old layout. */
int jello_jit_module_reload(jello_vm* vm, jello_jit_module* jm, const jello_bc_module* old_m,
                            const jello_bc_module* new_m, int allow_conflicts,
                            jello_jit_reload_report* out_report);
void jello_jit_reload_report_free(jello_jit_reload_report* report);

#endif /* JELLO_JIT_INTERNAL_H */
