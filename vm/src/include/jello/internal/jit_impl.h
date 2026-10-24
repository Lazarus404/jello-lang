// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * JIT internals shared by every file under src/jit; the JIR, executable buffers, the register
 * allocator, call-survive planning, and the C helpers JIT code calls. */

#ifndef JELLO_JIT_IMPL_H
#define JELLO_JIT_IMPL_H

#include <jello/internal/bytecode.h>
#include <jello/internal/vm_internal.h>

/** JIR opcodes. A JIR function is a flat array of jello_jit_ir_insn over the source bytecode
 * function's own register numbers; no SSA form and no control-flow graph. Each numeric family
 * collapses into one op per type with the operator or kind in `imm`; other ops map one to one.
 * Bytecode ops with no JIR form lower to JIR_SLOW. Trailing notes give each op's a, b, c and
 * imm. */
typedef enum jello_jit_ir_op {
  JIR_NOP = 0,
  JIR_RET,
  JIR_JMP,
  JIR_JMP_IF,
  JIR_MOV,
  /** Safepoint check and fuel charge, where the interpreter makes them; before a backward JMP or
   * JMP_IF, a CALL or CALLR, and a THROW. Native calls need none, as they run quiescent. */
  JIR_SAFEPOINT,
  JIR_SLOW, /**< a/b/c/imm copied from the source; see jello_jit_ir_slow_runs_handler. */

  JIR_CALL_SELF,   /**< Self-recursive CALL; a=dst, b=first_arg, c=nargs. */
  JIR_CALL_DIRECT, /**< CALL to another function; a=dst, b=first_arg, c=nargs, imm=callee index. */
  JIR_CALL_NATIVE, /**< a=dst, b=first_arg, c=nargs, imm=import index. */
  JIR_CALLR,       /**< Call through a Function value; a=dst, b=callee, c=nargs, imm=first_arg. */

  JIR_TRY,    /**< a=payload register (Dynamic), imm=handler offset from bc_pc + 1, as for JMP. */
  JIR_ENDTRY, /**< Closes the innermost try region. */
  JIR_THROW,  /**< a=payload register (Dynamic); never falls through. */

  JIR_CONST_I8_IMM, /**< a=dst, imm=literal, masked to the destination's width. */
  JIR_CONST_I32,    /**< a=dst, imm=literal value. */
  JIR_CONST_I64,    /**< a=dst, imm=const_i64 pool index. */
  JIR_CONST_F16,    /**< a=dst, imm=raw 16-bit payload. */
  JIR_CONST_F32,    /**< a=dst, imm=raw bit pattern. */
  JIR_CONST_F64,    /**< a=dst, imm=const_f64 pool index. */
  JIR_CONST_BOOL,   /**< a=dst, imm=0/1. */
  JIR_CONST_NULL,   /**< a=dst. */
  JIR_CONST_ATOM,   /**< a=dst, imm=atom id. */

  JIR_BIN_I32, /**< a=dst, b=lhs, c=rhs, imm=jello_jir_bin. */
  JIR_BIN_I64,
  JIR_BIN_F16,
  JIR_BIN_F32,
  JIR_BIN_F64,
  JIR_ADD_I32_IMM, /**< a=dst, b=src, imm=literal; imm cannot also name an operator. */
  JIR_SUB_I32_IMM,
  JIR_MUL_I32_IMM,
  JIR_NEG_I32, /**< a=dst, b=src. */
  JIR_NEG_I64,
  JIR_NEG_F32,
  JIR_NEG_F64,
  JIR_NOT_BOOL,

  JIR_CMP_I32, /**< a=dst Bool, b=lhs, c=rhs, imm=jello_jir_cmp. */
  JIR_CMP_I64,
  JIR_CMP_F32,
  JIR_CMP_F64,
  JIR_EQ_I32_IMM, /**< a=dst Bool, b=src, imm=literal. */
  JIR_LT_I32_IMM,

  JIR_CONV, /**< a=dst, b=src, imm=jello_jir_conv. */

  JIR_TO_DYN,     /**< a=dst Dynamic, b=src. */
  JIR_FROM_DYN,   /**< a=dst, b=src Dynamic, imm=target jello_type_kind (for PTR, a's). */
  JIR_KINDOF,     /**< a=dst I32, b=src Dynamic. */
  JIR_PHYSEQ,     /**< a=dst Bool, b=reg1, c=reg2. */
  JIR_IS_NULL,    /**< a=dst Bool, b=src Dynamic or reference. */
  JIR_NULL_CHECK, /**< a=src Dynamic or reference; traps NULL_DEREF when null. */
  JIR_IS_ENUM,    /**< a=dst Bool, b=src Dynamic, imm=enum type. */

  JIR_BYTES_NEW,     /**< a=dst, b=len. */
  JIR_BYTES_LEN,     /**< a=dst, b=bytes. */
  JIR_BYTES_GET_U8,  /**< a=dst, b=bytes, c=idx. */
  JIR_BYTES_SET_U8,  /**< a=val, b=bytes, c=idx. */
  JIR_BYTES_EQ,      /**< a=dst Bool, b=lhs, c=rhs. */
  JIR_BYTES_CONCAT2, /**< a=dst, b=lhs, c=rhs. */
  JIR_BYTES_BITOP,   /**< a=dst, b=lhs, c=rhs, imm=jello_jir_bin (AND, OR or XOR). */
  JIR_BYTES_READ,    /**< a=dst, b=bytes, c=offset, imm=jello_jir_bytes_rw. */
  JIR_BYTES_WRITE,   /**< a=val, b=bytes, c=offset, imm=jello_jir_bytes_rw. */
  JIR_BYTES_BLIT,    /**< a=dst, b=src, c=offset. */
  JIR_BITS_WRITE,    /**< a=bytes, b=val, c=width, imm=bit offset. */
  JIR_BITS_READ,     /**< a=dst, b=bytes, c=width, imm=bit offset. */
  JIR_BYTES_FREEZE,  /**< a=bytes. */

  JIR_STRUCT_NEW, /**< a=dst. */
  JIR_STRUCT_GET, /**< a=dst, b=struct, imm=field index. */
  JIR_STRUCT_SET, /**< a=val, b=struct, imm=field index. */

  JIR_OBJ_NEW,      /**< a=dst. */
  JIR_OBJ_HAS_ATOM, /**< a=dst Bool, b=object, imm=atom id. */
  JIR_OBJ_GET_ATOM, /**< a=dst, b=object, imm=atom id. */
  JIR_OBJ_SET_ATOM, /**< a=val, b=object, imm=atom id. */
  JIR_OBJ_GET,      /**< a=dst, b=object, c=key atom reg. */
  JIR_OBJ_SET,      /**< a=val, b=object, c=key atom reg. */
  JIR_OBJ_HAS,      /**< a=dst Bool, b=object, c=key atom reg. */
  JIR_OBJ_REMOVE,   /**< a=dst Bool, b=object, c=key atom reg. */
  JIR_OBJ_CLEAR,    /**< b=object. */
  JIR_OBJ_COPY,     /**< a=dst, b=src. */

  JIR_ARRAY_LEN, /**< a=dst I32, b=array. */
  JIR_ARRAY_GET, /**< a=dst, b=array, c=idx. */
  JIR_ARRAY_SET, /**< a=val, b=array, c=idx. */

  JIR_ENUM_TAG, /**< a=dst I32, b=enum. */
  JIR_ENUM_GET, /**< a=dst, b=enum, c=field index. */

  JIR_GLOBAL_GET, /**< a=dst, imm=global index. */
  JIR_GLOBAL_SET, /**< a=val, imm=global index. */
} jello_jit_ir_op;

typedef enum jello_jir_bin {
  JIR_BIN_ADD = 0,
  JIR_BIN_SUB,
  JIR_BIN_MUL,
  JIR_BIN_DIV,
  JIR_BIN_MOD,
  JIR_BIN_SHL,
  JIR_BIN_SHR,
  JIR_BIN_AND,
  JIR_BIN_OR,
  JIR_BIN_XOR,
} jello_jir_bin;

typedef enum jello_jir_cmp {
  JIR_CMP_EQ = 0,
  JIR_CMP_LT,
} jello_jir_cmp;

typedef enum jello_jir_conv {
  JIR_CONV_SEXT_I64 = 0,
  JIR_CONV_SEXT_I16,
  JIR_CONV_TRUNC_I8,
  JIR_CONV_TRUNC_I16,
  JIR_CONV_I32_FROM_I64,
  JIR_CONV_F64_FROM_I32,
  JIR_CONV_I32_FROM_F64,
  JIR_CONV_F64_FROM_I64,
  JIR_CONV_I64_FROM_F64,
  JIR_CONV_F32_FROM_I32,
  JIR_CONV_I32_FROM_F32,
  JIR_CONV_F64_FROM_F32,
  JIR_CONV_F32_FROM_F64,
  JIR_CONV_F32_FROM_I64,
  JIR_CONV_I64_FROM_F32,
  JIR_CONV_F16_FROM_F32,
  JIR_CONV_F32_FROM_F16,
  JIR_CONV_F16_FROM_I32,
  JIR_CONV_I32_FROM_F16,
} jello_jir_conv;

/** The value type and byte order of a JIR_BYTES_READ or JIR_BYTES_WRITE. */
typedef enum jello_jir_bytes_rw {
  JIR_BRW_U16_LE = 0,
  JIR_BRW_U16_BE,
  JIR_BRW_U32_LE,
  JIR_BRW_U32_BE,
  JIR_BRW_I32_LE,
  JIR_BRW_I32_BE,
  JIR_BRW_F32_LE,
  JIR_BRW_F32_BE,
  JIR_BRW_I64_LE,
  JIR_BRW_I64_BE,
  JIR_BRW_F64_LE,
  JIR_BRW_F64_BE,
} jello_jir_bytes_rw;

typedef enum jello_jit_ir_flags {
  JELLO_JIR_FLAG_NONE = 0,
  JELLO_JIR_FLAG_NUMERIC_FAST = 1u << 0, /**< No op is Dynamic, allocating or unimplemented. */
} jello_jit_ir_flags;

typedef struct jello_jit_ir_insn {
  uint16_t op; /**< A jello_jit_ir_op. */
  uint8_t a, b, c;
  uint32_t imm;
  uint32_t bc_pc; /**< Bytecode pc this was lowered from. */
} jello_jit_ir_insn;

typedef struct jello_jit_ir_function {
  uint32_t nregs;
  uint32_t ninsns;
  jello_jit_ir_insn* insns;
  uint32_t nslow;
  uint32_t flags; /**< jello_jit_ir_flags, shown by jlo_tool's dumpir. */
} jello_jit_ir_function;

/** Which of an instruction's a, b and c fields are register indices for `op`, as JELLO_JIR_REG_*
 * bits. The others hold literals; the width of BITS_READ and BITS_WRITE, the argument count of
 * a call, and JIR_SLOW's verbatim operands. Shared by jello_jit_ir_verify, survive planning
 * and jlo_tool's dumpir. */
#define JELLO_JIR_REG_A (1u << 0)
#define JELLO_JIR_REG_B (1u << 1)
#define JELLO_JIR_REG_C (1u << 2)
unsigned jello_jit_ir_op_reg_mask(jello_jit_ir_op op);

/** Lowers bytecode function `fi` of `m` into a fresh jello_jit_ir_function, owned by the caller
 * and freed with jello_jit_ir_free. Returns NULL if `fi` is out of range; aborts when memory
 * runs out. */
jello_jit_ir_function* jello_jit_ir_build(const jello_bc_module* m, uint32_t fi);

/** Checks that every register operand of `f` is below its nregs (a direct or native call's
 * argument run must fit), every bc_pc is an instruction of `src`, and every TRY handler lies in
 * `src`. Returns 1 when `f` is well-formed. */
int jello_jit_ir_verify(const jello_jit_ir_function* f, const jello_bc_function* src);

void jello_jit_ir_free(jello_jit_ir_function* f);

/** Registers JIT code reserves, all callee-saved in the platform ABI. JIT_REG_VM holds the
 * jello_vm*, JIT_REG_BASE the current frame's register memory (register r lives at
 * JIT_REG_BASE + r * REG_SLOT_BYTES), JIT_REG_OUT the jello_numeric_result* of an outermost
 * entry, and JIT_REG_SURVIVE a value kept across a call (jit_reg_survive_hold). On arm64, x27
 * is also reserved; the emitter keeps values in it across C helper calls. */
#if defined(__aarch64__)
typedef enum jit_arm64_reg {
  JIT_REG_OUT = 19,
  JIT_REG_VM = 20,
  JIT_REG_BASE = 21,
  JIT_REG_SURVIVE = 28,
} jit_arm64_reg;
#else
typedef enum jit_x64_reg {
  JIT_REG_OUT = 3,      /**< RBX */
  JIT_REG_VM = 12,      /**< R12 */
  JIT_REG_BASE = 13,    /**< R13 */
  JIT_REG_SURVIVE = 15, /**< R15 */
} jit_x64_reg;
#endif

/** Memory that JIT code is emitted into, writable or executable but never both. A module keeps
 * all its functions in one. */
typedef struct jit_exec_buf {
  uint8_t* mem; /**< NULL if allocation failed. */
  size_t cap;
  size_t len;     /**< Bytes written. */
  int finalized;  /**< Set while the code is executable; jit_exec_buf_write refuses to write. */
  int overflowed; /**< Set when a write did not fit; the emitter declines the function. */
  size_t limit;   /**< Writes stop here; cap, or the end of an open append window. */
  size_t append_from, append_end; /**< The open append window's start and its pages' end. */
} jit_exec_buf;

/** Maps `cap` bytes of private, anonymous, read-write memory. On failure the buffer's mem is
 * NULL. */
jit_exec_buf jit_alloc_exec_buf(size_t cap);

/** Appends `n` bytes. Returns 0 and sets `overflowed`, writing nothing, when the buffer has no
 * memory, is finalised, or would pass `limit`. The buffer never grows. */
int jit_exec_buf_write(jit_exec_buf* buf, const void* bytes, size_t n);

/** Makes the whole buffer read-execute and syncs the instruction cache over the written code.
 * Returns 0 if the pages cannot be protected. */
int jit_finalize_exec_buf(jit_exec_buf* buf);

/** Opens an append window on a finalised buffer, for lazy compilation; makes the pages from
 * the end of the code through `bound` more bytes writable (and not executable) and lets writes
 * through up to there. The page holding the end of the code may hold code the caller's own JIT
 * frames return into, so the window must close before control goes back to JIT code. Returns 0
 * on failure. */
int jit_exec_buf_begin_append(jit_exec_buf* buf, size_t bound);
/** Closes the append window; makes its pages executable again and syncs the instruction cache
 * over what was written. Returns 0 on failure. */
int jit_exec_buf_end_append(jit_exec_buf* buf);

void jit_free_exec_buf(jit_exec_buf* buf);

/** Allocator handles for physical registers; a GPR's number, JIT_PREG_XMM0 + i for FP pool slot
 * i, or JIT_PREG_NONE. */
enum {
  JIT_PREG_NONE = -1,
  JIT_PREG_XMM0 = 64,
};

typedef struct jit_reg_emit_ctx {
  jit_exec_buf* buf;
} jit_reg_emit_ctx;

/** Register-allocator state for one function's emission. Every bytecode register (vreg) has a
 * home slot in the frame, kept current by write-through stores; a physical register may cache
 * it besides. */
typedef struct jit_reg_state {
  uint32_t nregs;
  int8_t* current; /**< [nregs]: the register caching each vreg, or JIT_PREG_NONE. */
  /** Per pool slot, the vreg it caches or JIT_PREG_NONE. Sized to fit either backend's pool;
   * only the first jit_gpr_pool_len and jit_xmm_pool_len entries are used. 16 bits, since
   * vregs reach 255, which an int8_t would read back as JIT_PREG_NONE. */
  int16_t gpr_vreg[11];
  int16_t xmm_vreg[8];
  /** Pool slots holding an operand of the instruction being emitted, which eviction skips so a
   * later operand cannot take a register whose handle the emitter still uses. Round-robin
   * eviction alone does not ensure this; a first operand found cached never advances the
   * cursor. Cleared by jit_reg_unpin_all after each instruction. */
  uint8_t gpr_pinned[11];
  uint8_t xmm_pinned[8];
  unsigned next_gpr_victim;
  unsigned next_xmm_victim;
  jit_reg_emit_ctx emit;
  /** Whether JIT_REG_SURVIVE holds a value, and which vreg's. Kept apart from the pool; no
   * allocation binds JIT_REG_SURVIVE, and jit_reg_get_gpr answers the held vreg with it. */
  int survive_held;
  uint8_t survive_vreg;
} jit_reg_state;

/** Home loads emitted by allocations. */
extern _Atomic uint32_t jit_reg_reload_count;
extern _Atomic uint32_t jit_reg_discard_count; /**< Calls to jit_reg_state_discard_currents. */

/** Call sites armed and declined by jit_survive_plan_build, counted on every plan build. */
extern _Atomic uint32_t jit_call_survive_armed_count;
extern _Atomic uint32_t jit_call_survive_decline_count;

/** Backend data for regalloc.c, which names no physical register itself; the allocatable GPR
 * numbers and the pool lengths. Defined by whichever of regalloc_x64.c and regalloc_arm64.c
 * the build links. */
extern const uint8_t jit_gpr_pool[];
extern const uint32_t jit_gpr_pool_len;
extern const uint32_t jit_xmm_pool_len;

/** Backend encoders that load or store `vreg`'s home slot, JIT_REG_BASE + vreg * REG_SLOT_BYTES,
 * from or to a register. The GPR forms take a register number, the xmm forms an FP pool
 * index. */
void jit_spill_load_gpr64(jit_exec_buf* buf, unsigned dst_gpr, uint8_t vreg);
void jit_spill_store_gpr32(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr);
void jit_spill_store_gpr64(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr);
void jit_spill_load_xmm32(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg);
void jit_spill_load_xmm64(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg);
void jit_spill_store_xmm32(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm);
void jit_spill_store_xmm64(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm);

void jit_reg_state_init(jit_reg_state* st, uint32_t nregs, jit_reg_emit_ctx emit);
void jit_reg_state_free(jit_reg_state* st);
/** Forgets every vreg's cached register, as after a call; emits nothing. */
void jit_reg_state_clobber_call(jit_reg_state* st);
/** As jit_reg_state_clobber_call, also counted in jit_reg_discard_count. */
void jit_reg_state_discard_currents(jit_reg_state* st);
/** Binds `vreg` to a free pool register (else evicts one round-robin), pins it and returns it,
 * or JIT_PREG_NONE when `vreg` is out of range. Loads the home when `and_load` is set, 64 bits
 * wide for an xmm when `wide` is. Evicting stores nothing; homes are always current. */
int jit_reg_alloc_gpr(jit_reg_state* st, uint8_t vreg, int and_load);
int jit_reg_alloc_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide);
/** Like the alloc functions, but returns (and pins) the register already caching `vreg` if
 * there is one. A vreg held by jit_reg_survive_hold reads as JIT_REG_SURVIVE. */
int jit_reg_get_gpr(jit_reg_state* st, uint8_t vreg, int and_load);
int jit_reg_get_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide);
/** Records that `preg` caches `vreg`, dropping both sides' previous bindings. */
void jit_reg_bind(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_unbind_vreg(jit_reg_state* st, uint8_t vreg);
void jit_reg_unbind_preg(jit_reg_state* st, int preg);
/** Stores `preg` to `vreg`'s home (32 or 64 bits) and binds them. Ignored when `preg` is of the
 * other register class. */
void jit_reg_store_home_gpr32(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_store_home_gpr64(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_store_home_xmm32(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_store_home_xmm64(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_unpin_all(jit_reg_state* st);

/** Hold and release bracket a call site that keeps `vreg` in JIT_REG_SURVIVE across its call.
 * Hold loads the home (always current) straight into JIT_REG_SURVIVE without binding it, so
 * ordinary allocation never touches the register, and jit_reg_get_gpr serves the held vreg
 * from it, so the consuming instruction needs no special code. One hold at a time. */
void jit_reg_survive_hold(jit_reg_state* st, uint8_t vreg);
void jit_reg_survive_release(jit_reg_state* st);
int jit_reg_survive_is_held(const jit_reg_state* st);
uint8_t jit_reg_survive_held_vreg(const jit_reg_state* st);

/** Which typed calls keep one I32 in JIT_REG_SURVIVE, planned once per function before
 * emission. Declining is always safe; it only misses the saving. */
typedef struct jit_survive_plan {
  uint8_t* armed;       /**< [ninsns]: 1 where the call holds `candidate` across it. */
  uint8_t* candidate;   /**< [ninsns]: the vreg held, where armed. */
  uint8_t* is_consumer; /**< [ninsns]: 1 where JIT_REG_SURVIVE is released after emitting it. */
  uint32_t nregs;
  int any_armed; /**< Set when some call is armed; the prologue saves JIT_REG_SURVIVE. */
} jit_survive_plan;

/** Builds `out_plan` for `ir`; `is_target` marks bytecode pcs that are branch targets. Returns
 * 0 when memory runs out. */
int jit_survive_plan_build(const jello_jit_ir_function* ir, const jello_bc_module* m,
                           const jello_bc_function* f, const uint8_t* is_target,
                           jit_survive_plan* out_plan);
void jit_survive_plan_free(jit_survive_plan* plan);

/** Sets vm->trap_code to the named trap; called from JIT trap tails. */
JELLO_JIT_ABI void jit_trap_arithmetic(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_stack_overflow(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_null_deref(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_bounds(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_type_mismatch(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_limit(jello_vm* vm);

/** Code-buffer sizing in bytes; the smallest blob, and the room reserved per JIR instruction for
 * a function without and with calls. */
enum {
  JIT_EMIT_BUF_MIN_CAP = 65536u,
  JIT_EMIT_PER_INSN_BUDGET = 128u,
  JIT_EMIT_CALL_INSN_BUDGET = 512u,
};

/** Returns the topmost frame's register memory, the `frame_base` a JIT entry takes. JIT calls
 * push and pop ordinary call frames, so the GC and the interpreter see one frame shape. */
uint8_t* jit_frame_entry_base(jello_vm* vm);

/** Allocates the bytewise AND (`op_kind` 0), OR (1) or XOR (2) of `b` and `c`, so the GC may
 * run. Returns NULL with vm->trap_code BOUNDS when their lengths differ. */
JELLO_JIT_ABI jello_bytes* jit_bytes_bitop(jello_vm* vm, jello_bytes* b, jello_bytes* c,
                                           int op_kind);
/** Writes the low `width` bits of `value` to, or reads `width` bits from, bit offset `bit_off` of
 * `b`. Bits run most significant first, from each byte's top bit. No bounds check. */
JELLO_JIT_ABI void jit_bits_write(jello_bytes* b, uint32_t width, uint32_t bit_off, uint32_t value);
JELLO_JIT_ABI uint32_t jit_bits_read(const jello_bytes* b, uint32_t width, uint32_t bit_off);
/** `jello_bytes_join` of `b` and `c`. */
JELLO_JIT_ABI jello_bytes* jit_bytes_concat2(jello_vm* vm, jello_bytes* b, jello_bytes* c);

/** Whether JIT code may run a JIR_SLOW instruction with source opcode `op` by calling its
 * interpreter handler; true for handlers that only read and write the current frame's
 * registers and return CONTINUE or TRAP. Those that move the pc or push or pop frames
 * (SWITCH_KIND, TAILCALL, ...) are excluded, so a function using one is never compiled; CALLR,
 * TRY, ENDTRY and THROW have JIR ops of their own instead. */
int jello_jit_ir_slow_runs_handler(jello_op op);

/** Runs instruction `pc` of function `fi` through its interpreter handler against the topmost
 * frame, which is `fi`'s. JIT code keeps every home current and drops its register bindings
 * around the call, so the handler sees the frame as the interpreter would. Returns nonzero
 * when it trapped, with the trap located at (`fi`, `pc`). */
JELLO_JIT_ABI int jit_run_handler(jello_vm* vm, const jello_bc_module* m, uint32_t fi, uint32_t pc);

/** Pops the top frame as a trap leaves it, first recording its call site when a callee's trap
 * is passing through it (see vm_trace_frames). */
JELLO_JIT_ABI void jit_trap_pop_frame(jello_vm* vm);
/** Where JIT code stores vm->trap_trace.site. */
#define JIT_TRAP_SITE_OFF                                                                          \
  ((uint32_t)(offsetof(jello_vm, trap_trace) + offsetof(jello_trap_trace, site)))

/** JIT_C(fn) is what x64 JIT code calls to reach `fn`, a C function that code other than JIT code
 * also calls. On Windows, where `fn` keeps the Win64 convention so that its other callers pay
 * nothing, it is a System V thunk for `fn`; elsewhere it is `fn` itself. */
#ifdef JELLO_JIT_SYSV_THUNKS
#define JIT_C(fn) jit_sysv_##fn
JELLO_JIT_ABI void* jit_sysv_memcpy(void* dst, const void* src, size_t n);
JELLO_JIT_ABI void* jit_sysv_memmove(void* dst, const void* src, size_t n);
JELLO_JIT_ABI int jit_sysv_memcmp(const void* a, const void* b, size_t n);
JELLO_JIT_ABI jello_box_f64* jit_sysv_jello_box_f64_new(jello_vm* vm, double v);
JELLO_JIT_ABI jello_object* jit_sysv_jello_object_new(jello_vm* vm, uint32_t type_id);
JELLO_JIT_ABI jello_object* jit_sysv_jello_object_copy(jello_vm* vm, const jello_object* src);
JELLO_JIT_ABI int jit_sysv_jello_object_has(const jello_object* o, uint32_t atom_id);
JELLO_JIT_ABI jello_struct* jit_sysv_jello_struct_new(jello_vm* vm, const jello_bc_module* m,
                                                      uint32_t struct_sig_id);
JELLO_JIT_ABI jello_value jit_sysv_vm_box_from_typed(jello_vm* vm, const jello_bc_module* m,
                                                     const jello_bc_function* f, reg_frame* rf,
                                                     uint32_t r);
JELLO_JIT_ABI int jit_sysv_vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m,
                                               const jello_bc_function* f, reg_frame* rf,
                                               uint32_t dst, jello_value v);
JELLO_JIT_ABI jello_bytes* jit_sysv_vm_buffer_new(jello_vm* vm, uint32_t length);
JELLO_JIT_ABI uint32_t jit_sysv_vm_dynamic_is_enum(jello_value v, jello_type_id type);
JELLO_JIT_ABI uint32_t jit_sysv_vm_kindof_dynamic(jello_value v);
JELLO_JIT_ABI float jit_sysv_vm_f16_to_f32(uint16_t bits);
JELLO_JIT_ABI uint16_t jit_sysv_vm_f32_to_f16(float f);
JELLO_JIT_ABI void jit_sysv_vm_pop_frame(jello_vm* vm);
JELLO_JIT_ABI int jit_sysv_vm_try_push(jello_vm* vm, uint32_t catch_pc, uint32_t payload_reg);
JELLO_JIT_ABI void jit_sysv_vm_try_pop(jello_vm* vm);
JELLO_JIT_ABI void jit_sysv_vm_safepoint_check(jello_vm* vm);
JELLO_JIT_ABI int jit_sysv_vm_reductions_out(jello_vm* vm);
#else
#define JIT_C(fn) fn
#endif

#endif /* JELLO_JIT_IMPL_H */
