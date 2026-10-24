// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>
#include <jello/internal/platform.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__)
enum { k_jit_body_off = 17 };
#elif defined(__aarch64__)
enum { k_jit_body_off = 44 };
#endif

static int jit_call_site_targets(const uint8_t* mem, size_t start, size_t end, size_t target) {
  /* Both backends call through the module's entry table, and TARGET is the table slot's address.
   * x64 loads the slot's address (`movabs rax, imm64`), then the slot (`mov rax, [rax+0]`). arm64
   * builds x16 from movz/movk, then loads through it (`ldr x16, [x16]`). */
#if defined(__x86_64__)
  static const uint8_t k_load_slot[7] = { 0x48, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x00 };
  for(size_t i = start; i + 17 <= end; i++) {
    if(mem[i] != 0x48 || mem[i + 1] != 0xB8) continue;
    uint64_t imm = 0;
    memcpy(&imm, mem + i + 2, 8);
    if(imm != (uint64_t)target) continue;
    if(memcmp(mem + i + 10, k_load_slot, sizeof(k_load_slot)) == 0) return 1;
  }
  return 0;
#elif defined(__aarch64__)
  uint64_t x16 = 0;
  for(size_t i = start; i + 4 <= end; i += 4) {
    uint32_t w = 0;
    memcpy(&w, mem + i, 4);
    unsigned sh = ((w >> 21) & 3u) * 16u;
    uint64_t imm = (uint64_t)((w >> 5) & 0xFFFFu) << sh;
    if((w & 0xFF80001Fu) == 0xD2800010u) x16 = imm;
    else if((w & 0xFF80001Fu) == 0xF2800010u) x16 = (x16 & ~((uint64_t)0xFFFFu << sh)) | imm;
    else if(w == 0xF9400210u && x16 == (uint64_t)target) return 1;
  }
  return 0;
#else
  (void)mem;
  (void)start;
  (void)end;
  (void)target;
  return 0;
#endif
}

static void build_call_direct_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
                                     jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
                                     jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 4;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_ADD_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 1;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 1;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 2; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

#if defined(__x86_64__) || defined(__aarch64__)
/* fib(n) = n<2 ? n : fib(n-1)+fib(n-2) is the canonical shape a
 * register-survive mechanism is *for* -- fib(n-1)'s own CALL_SELF result
 * (r3) is exactly a survive candidate: a value defined before a second,
 * sibling CALL_SELF (fib(n-2)) that is only read again *after* that second
 * call returns (the ADD summing both). It is also the reentrancy stress
 * case: fib(n-1) recurses *through* fib's own call-2 survive window, so a
 * naive "hold in R15 with no save/restore" implementation has every nested
 * activation clobber its caller's still-live R15 the moment it arms its own
 * hold -- only a real, function-scoped `push r15`/`pop r15` keeps each
 * activation's own hold independent of every other one live on the native
 * call stack at the same time. */
static void build_fib_survive_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
                                     jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
                                     jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 1; r1[2] = 0; r1[3] = 0; r1[4] = 0; r1[5] = 0; r1[6] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = n;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_LT_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 2;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6; /* -> pc 8 (base case) */
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1; /* r3 = fib(n-1) -- survive candidate */
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1; /* r5 = fib(n-2) */
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5; /* consumer: r3 + r5 */
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_RET; e1[8].a = 0; /* base case: n < 2 */

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 7; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 9; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

static void build_call_self_sum_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
                                       jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
                                       jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  for(uint32_t i = 0; i < 5; i++) r1[i] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 10;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_EQ_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 0;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 4;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_ADD_I32; e1[4].a = 4; e1[4].b = 0; e1[4].c = 3;
  e1[5].op = (uint8_t)JOP_RET; e1[5].a = 4;
  e1[6].op = (uint8_t)JOP_CONST_I32; e1[6].a = 4; e1[6].imm = 0;
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 5; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 8; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}
#endif

static void test_module_prepare_produces_one_blob(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  assert(jm.prepared == 1);
  assert(jm.blob.mem != NULL);
  assert(jm.nfuncs == 2);
  assert(jm.funcs[0].size > 0);
  assert(jm.funcs[1].size > 0);
  assert(jm.funcs[0].body_off == (size_t)k_jit_body_off);
  assert(jm.funcs[1].body_off == (size_t)k_jit_body_off);
  /* Both functions live in the same mapping -- checked directly rather than
   * assumed, since a per-function fallback allocator would also satisfy
   * every other assertion here. */
  assert(jm.funcs[1].blob_off > jm.funcs[0].blob_off);
  assert(jm.funcs[1].blob_off < jm.blob.cap);
  jello_jit_module_free(&jm);
  printf("test_module_prepare_produces_one_blob: PASS\n");
}

static void test_call_site_loads_entry_slot(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);

  /* func[0] contains exactly one CALL_DIRECT: find its call site in func[0]'s own byte range and
   * confirm it loads func[1]'s entry-table slot, which holds func[1]'s body_off. This is not
   * "some call exists somewhere"; it is "this call site resolves to this callee". */
  size_t f0_start = jm.funcs[0].blob_off;
  size_t f0_end = f0_start + jm.funcs[0].size;
  size_t callee_target = jm.funcs[1].blob_off + jm.funcs[1].body_off;
  assert(jm.entries[1] == jm.blob.mem + callee_target);
  callee_target = (size_t)(uintptr_t)&jm.entries[1];

  int found = jit_call_site_targets(jm.blob.mem, f0_start, f0_end, callee_target);
  assert(found == 1);
  jello_jit_module_free(&jm);
  printf("test_call_site_loads_entry_slot: PASS\n");
}

static void test_module_prepare_rejects_call_in_single_compile(void) {
  jello_type_entry types[1];
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  jello_type_id r[2];
  r[0] = 0; r[1] = 0;
  jello_insn ins[2];
  ins[0].op = (uint8_t)JOP_CALL; ins[0].a = 0; ins[0].b = 0; ins[0].imm = 0;
  ins[1].op = (uint8_t)JOP_RET; ins[1].a = 0;
  jello_bc_function func = {0};
  func.nregs = 2; func.cap_start = 0; func.reg_types = r; func.ninsns = 2; func.insns = ins;
  jello_bc_module m = {0};
  m.ntypes = 1; m.types = types; m.nfuncs = 1; m.funcs = &func; m.entry = 0;

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 0);
  jello_jit_code_free(&code);
  printf("test_module_prepare_rejects_call_in_single_compile: PASS\n");
}

/* The JIT runs CALL_NATIVE through its interpreter handler. */
static void test_module_prepare_call_native(void) {
  jello_type_entry types[1];
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  jello_type_id r[2];
  r[0] = 0; r[1] = 0;

  jello_insn ins[3];
  ins[0].op = (uint8_t)JOP_CONST_I32; ins[0].a = 0; ins[0].imm = 0;
  ins[1].op = (uint8_t)JOP_CALL_NATIVE; ins[1].a = 1; ins[1].b = 0; ins[1].c = 0; ins[1].imm = 0;
  ins[2].op = (uint8_t)JOP_RET; ins[2].a = 1;

  jello_bc_function func = {0};
  func.nregs = 2; func.cap_start = 0; func.reg_types = r; func.ninsns = 3; func.insns = ins;

  jello_bc_module m = {0};
  m.ntypes = 1; m.types = types; m.nfuncs = 1; m.funcs = &func; m.entry = 0;

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  assert(jm.prepared == 1);
  jello_jit_module_free(&jm);
  printf("test_module_prepare_call_native: PASS\n");
}

/* A callee whose own frame contains a Bytes/Struct/Object/Dynamic register is
 * no longer rejected here -- func[1] takes and immediately returns a T_BYTES
 * argument (reg 0), which is a POLY-typed CALL target check.c already
 * validates matches the caller's own argument kind; emit_call_insn's inline
 * frame push now sets has_pointer and zeroes (+ null-inits Dynamic) that
 * frame exactly like vm_push_frame does, so this must now succeed and produce
 * real generated code for both functions, not silently fall back. func[0]
 * passes r0 uninitialised (this hand-built module has no Bytes pool for
 * CONST_BYTES to give it a defined value) -- module preparation only compiles the code, it never runs
 * it, so this proves the shape is accepted, not any Bytes-op runtime
 * correctness (jit_bytes_tests.c owns that). */
static void test_module_prepare_accepts_pointer_callee(void) {
  jello_type_entry types[2];
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BYTES;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  jello_type_id r0[2];
  r0[0] = 1; r0[1] = 1;
  jello_type_id r1[1];
  r1[0] = 1;

  jello_insn e0[2];
  e0[0].op = (uint8_t)JOP_CALL; e0[0].a = 1; e0[0].b = 0; e0[0].c = 1; e0[0].imm = 1;
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 1;

  jello_insn e1[1];
  e1[0].op = (uint8_t)JOP_RET; e1[0].a = 0;

  jello_bc_function funcs[2] = {0};
  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nregs = 1; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 1; funcs[1].insns = e1;

  jello_bc_module m = {0};
  m.ntypes = 2; m.types = types; m.nfuncs = 2; m.funcs = funcs; m.entry = 0;

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  assert(jm.prepared == 1);
  assert(jm.funcs[0].size > 0);
  assert(jm.funcs[1].size > 0);
  jello_jit_module_free(&jm);
  printf("test_module_prepare_accepts_pointer_callee: PASS\n");
}

#if defined(__x86_64__) || defined(__aarch64__)

static jello_exec_status run_module_exec(const jello_bc_module* m, jello_numeric_result* out) {
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_exec_status st = jello_vm_exec_numeric(vm, m, out);
  if(st != JELLO_EXEC_OK)
    fprintf(stderr, "run_module_exec: st=%d trap=%d\n", (int)st, (int)jello_vm_last_trap_code(vm));
  jello_vm_destroy(vm);
  return st;
}

static void test_call_self_recursive_sum(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[8];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_self_sum_module(types, r0, r1, e0, e1, funcs, &m);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  vm_warm_jit_layouts(vm, &m);
  assert(vm->jit_layout_by_func_len == 2);
  assert(vm->jit_layout_by_func[0] != NULL);
  assert(vm->jit_layout_by_func[1] != NULL);
  jello_vm_destroy(vm);

  /* Prove jello_vm_exec_numeric's own production path (not just a C test
   * harness calling entry_off directly) is what runs this -- the counter
   * only advances on the try_enter branch that actually calls into
   * JIT-compiled code. */
  jit_try_enter_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = run_module_exec(&m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 55);
  assert(jit_try_enter_count == 1);
  printf("test_call_self_recursive_sum: PASS\n");
}

/* A frame stack committed only as pushes reach it, as on Windows, grows through the JIT's inline
 * pushes to the depth a recursion needs; past its size the frames go on in a segment above it,
 * and past the most call frames the recursion traps STACK_OVERFLOW. */
static void test_frame_stack_grows(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[8];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_self_sum_module(types, r0, r1, e0, e1, funcs, &m);
  e0[0].imm = 3000; /* 3000 frames of 40 bytes, past the first commit's 16 KiB */

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  uint32_t max = vm->frame_stack_max;
  vm->frame_stack_limit = 0;
  jit_try_enter_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_OK);
  assert(jit_try_enter_count == 1);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 3000 * 3001 / 2);
  assert(vm->frame_stack_limit >= 3000u * 40u && vm->frame_stack_limit < max);

  vm->frame_stack_limit = 0;
  vm->frame_stack_max = vm->first_segment.bytes = 64u * 1024u;
  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 3000 * 3001 / 2);
  /* Back in the first segment, keeping the one it left for the next recursion this deep. */
  assert(vm->segment == &vm->first_segment && vm->first_segment.above != NULL);
  assert(vm->segment_bytes > 0);
  assert(vm->frame_stack_max == 64u * 1024u);

  uint32_t frames = vm->call_frames_limit;
  vm->call_frames_limit = 1000u;
  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_STACK_OVERFLOW);
  vm->call_frames_limit = frames;
  vm->frame_stack_max = vm->first_segment.bytes = max;
  jello_vm_destroy(vm);
  printf("test_frame_stack_grows: PASS\n");
}

/* An interpreted trap leaves its frames pushed, here in a segment above the frame stack the VM
 * mapped; destroying the VM unmaps both, each once. */
static void test_destroy_after_trap_in_segment(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[8];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_self_sum_module(types, r0, r1, e0, e1, funcs, &m);
  e0[0].imm = 3000;

  jello_setenv("JELLO_JIT_DISABLE", "1");
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  uint8_t* base = vm->frame_stack;
  vm->frame_stack_limit = 0;
  vm->frame_stack_max = vm->first_segment.bytes = 64u * 1024u;
  vm->call_frames_limit = 2000u; /* 2000 frames of 40 bytes: past 64 KiB, short of 3000 deep */
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_STACK_OVERFLOW);
  assert(vm->segment != &vm->first_segment && vm->frame_stack != base);
  jello_vm_destroy(vm);
  jello_unsetenv("JELLO_JIT_DISABLE");
  printf("test_destroy_after_trap_in_segment: PASS\n");
}

static void test_call_direct_executes(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 5);
  printf("test_call_direct_executes: PASS\n");
}

static int32_t fib_ref(int32_t n) {
  return (n < 2) ? n : fib_ref(n - 1) + fib_ref(n - 2);
}

/* The survive mechanism is not CALL_SELF-specific --
 * find_candidate/find_consumer (survive.c) only care that the intervening
 * instruction is *a* JIR_CALL_SELF/JIR_CALL_DIRECT, not which. func[2] here
 * calls the *same*, distinct leaf function (func[0], doubling its argument)
 * twice via JOP_CALL with imm != its own function index, i.e. JIR_CALL_DIRECT
 * both times -- r1 (func[0](n)) must survive across the second CALL_DIRECT to
 * be summed with r3 (func[0](n-1)) afterward. */
static void build_call_direct_survive_module(jello_type_entry* types,
    jello_type_id* r0, jello_type_id* r1, jello_type_id* r2,
    jello_insn* e0, jello_insn* e1, jello_insn* e2,
    jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;
  r2[0] = 0; r2[1] = 0; r2[2] = 0; r2[3] = 0; r2[4] = 0;

  e0[0].op = (uint8_t)JOP_MUL_I32_IMM; e0[0].a = 1; e0[0].b = 0; e0[0].imm = 2; /* leaf(x) = 2x */
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 1;

  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = n;
  e1[1].op = (uint8_t)JOP_CALL; e1[1].a = 1; e1[1].b = 0; e1[1].c = 1; e1[1].imm = 2; /* -> func[2] */
  e1[2].op = (uint8_t)JOP_RET; e1[2].a = 1;

  e2[0].op = (uint8_t)JOP_CALL; e2[0].a = 1; e2[0].b = 0; e2[0].c = 1; e2[0].imm = 0; /* r1 = leaf(n) -- candidate */
  e2[1].op = (uint8_t)JOP_SUB_I32_IMM; e2[1].a = 2; e2[1].b = 0; e2[1].imm = 1; /* r2 = n-1 */
  e2[2].op = (uint8_t)JOP_CALL; e2[2].a = 3; e2[2].b = 2; e2[2].c = 1; e2[2].imm = 0; /* r3 = leaf(n-1) */
  e2[3].op = (uint8_t)JOP_ADD_I32; e2[3].a = 4; e2[3].b = 1; e2[3].c = 3; /* consumer: r1 + r3 */
  e2[4].op = (uint8_t)JOP_RET; e2[4].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 3; funcs[1].insns = e1;
  funcs[2].nregs = 5; funcs[2].cap_start = 0; funcs[2].reg_types = r2; funcs[2].ninsns = 5; funcs[2].insns = e2;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1;
  m->types = types;
  m->nfuncs = 3;
  m->funcs = funcs;
  m->entry = 1;
}

static void test_call_direct_survive_correct(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2], r2[5];
  jello_insn e0[2], e1[3], e2[5];
  jello_bc_function funcs[3] = {0};
  jello_bc_module m = {0};
  build_call_direct_survive_module(types, r0, r1, r2, e0, e1, e2, funcs, &m, 5);

  jit_call_survive_armed_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 2 * 5 + 2 * 4); /* 18 */
  assert(jit_call_survive_armed_count > 0);
  printf("test_call_direct_survive_correct: PASS (result=%d, armed=%u)\n",
      out.as.i32, jit_call_survive_armed_count);
}

/* A candidate with a syntactically valid consumer *later* in the function
 * must still decline if a branch sits anywhere in between -- r1 (func[0](n))
 * has a real consumer (the ADD at the end), but the JMP_IF between the second
 * call and that ADD means the two are not in the same straight-line run, so
 * the same-basic-block restriction must decline this site regardless of which
 * way the branch actually goes at runtime. The point is not the runtime
 * outcome (r4 is always false here) -- it is that the *static* analysis
 * cannot know that, and must not assume it. Correctness (not armed=0) is the
 * real assertion; the decline count is instrumentation to confirm this test
 * is not vacuous. */
static void build_call_direct_branch_decline_module(jello_type_entry* types,
    jello_type_id* r0, jello_type_id* r1, jello_type_id* r2,
    jello_insn* e0, jello_insn* e1, jello_insn* e2,
    jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;
  r2[0] = 0; r2[1] = 0; r2[2] = 0; r2[3] = 0; r2[4] = 1; r2[5] = 0;

  e0[0].op = (uint8_t)JOP_ADD_I32_IMM; e0[0].a = 1; e0[0].b = 0; e0[0].imm = 100; /* leaf(x) = x+100 */
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 1;

  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = n;
  e1[1].op = (uint8_t)JOP_CALL; e1[1].a = 1; e1[1].b = 0; e1[1].c = 1; e1[1].imm = 2; /* -> func[2] */
  e1[2].op = (uint8_t)JOP_RET; e1[2].a = 1;

  e2[0].op = (uint8_t)JOP_CALL; e2[0].a = 1; e2[0].b = 0; e2[0].c = 1; e2[0].imm = 0; /* r1 = leaf(n) -- candidate */
  e2[1].op = (uint8_t)JOP_SUB_I32_IMM; e2[1].a = 2; e2[1].b = 0; e2[1].imm = 1; /* r2 = n-1 */
  e2[2].op = (uint8_t)JOP_CALL; e2[2].a = 3; e2[2].b = 2; e2[2].c = 1; e2[2].imm = 0; /* r3 = leaf(n-1) */
  e2[3].op = (uint8_t)JOP_EQ_I32_IMM; e2[3].a = 4; e2[3].b = 0; e2[3].imm = 0; /* r4 = (n == 0), unrelated to r1 */
  e2[4].op = (uint8_t)JOP_JMP_IF; e2[4].a = 4; e2[4].imm = 1; /* -> pc 6 -- a branch sits between call2 and the would-be consumer */
  e2[5].op = (uint8_t)JOP_ADD_I32; e2[5].a = 5; e2[5].b = 1; e2[5].c = 3; /* would-be consumer -- must not be reached across the branch */
  e2[6].op = (uint8_t)JOP_RET; e2[6].a = 5;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 3; funcs[1].insns = e1;
  funcs[2].nregs = 6; funcs[2].cap_start = 0; funcs[2].reg_types = r2; funcs[2].ninsns = 7; funcs[2].insns = e2;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 3;
  m->funcs = funcs;
  m->entry = 1;
}

static void test_call_survive_declines_across_branch(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[2], r2[6];
  jello_insn e0[2], e1[3], e2[7];
  jello_bc_function funcs[3] = {0};
  jello_bc_module m = {0};
  build_call_direct_branch_decline_module(types, r0, r1, r2, e0, e1, e2, funcs, &m, 5);

  jit_call_survive_armed_count = 0;
  jit_call_survive_decline_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == (5 + 100) + (4 + 100)); /* 209 -- correct regardless of survive */
  assert(jit_call_survive_decline_count > 0);
  printf("test_call_survive_declines_across_branch: PASS (result=%d, armed=%u, declined=%u)\n",
      out.as.i32, jit_call_survive_armed_count, jit_call_survive_decline_count);
}

static void test_call_self_survive_fib_correct(void) {
  /* fib(15) recurses to depth 15 with both branches active at once (wide
   * *and* deep), so many activations of func[1] -- each with its own
   * fib(n-1) result live in R15 -- coexist on the native call stack at
   * the same time. Wrong output here (rather than a crash) is the more
   * dangerous failure mode: a clobbered R15 still produces *a* number,
   * just the wrong one. */
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_fib_survive_module(types, r0, r1, e0, e1, funcs, &m, 15);

  jit_call_survive_armed_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == fib_ref(15));
  /* Prove the survive mechanism actually armed for this call site, not
   * merely that fib happens to produce the right answer via the decline
   * path (which would make this test worthless as a regression guard). */
  assert(jit_call_survive_armed_count > 0);
  printf("test_call_self_survive_fib_correct: PASS (fib(15)=%d, armed=%u)\n",
      out.as.i32, jit_call_survive_armed_count);
}

/* fib(n) with survive armed, but fib(0) traps (div-by-zero) on the nested
 * path -- proves the trap tail undoes the per-activation survive spill before
 * `ret`, not only the nested OK-return path. */
static void build_fib_survive_nested_trap_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 1; r1[2] = 0; r1[3] = 0; r1[4] = 0; r1[5] = 0; r1[6] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = n;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_LT_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 2;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6; /* -> base at pc 8 */
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1;
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5;
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_EQ_I32_IMM; e1[8].a = 1; e1[8].b = 0; e1[8].imm = 0;
  e1[9].op = (uint8_t)JOP_JMP_IF; e1[9].a = 1; e1[9].imm = 2; /* n==0 -> trap at pc 12 */
  e1[10].op = (uint8_t)JOP_RET; e1[10].a = 0; /* n==1 base */
  e1[11].op = (uint8_t)JOP_CONST_I32; e1[11].a = 2; e1[11].imm = 1;
  e1[12].op = (uint8_t)JOP_CONST_I32; e1[12].a = 3; e1[12].imm = 0;
  e1[13].op = (uint8_t)JOP_DIV_I32; e1[13].a = 4; e1[13].b = 2; e1[13].c = 3;
  e1[14].op = (uint8_t)JOP_RET; e1[14].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 7; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 15; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

static void test_nested_survive_trap_propagates(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[15];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_fib_survive_nested_trap_module(types, r0, r1, e0, e1, funcs, &m, 2);

  jit_call_survive_armed_count = 0;
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);
  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  assert(jit_call_survive_armed_count > 0);
  jello_vm_destroy(vm);
  printf("test_nested_survive_trap_propagates: PASS (armed=%u)\n", jit_call_survive_armed_count);
}

static void build_call_self_trap_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  for(uint32_t i = 0; i < 5; i++) r1[i] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 2;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_EQ_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 0;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 5;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_CONST_I32; e1[4].a = 4; e1[4].imm = 0;
  e1[5].op = (uint8_t)JOP_RET; e1[5].a = 4;
  e1[6].op = (uint8_t)JOP_CONST_I32; e1[6].a = 2; e1[6].imm = 1;
  e1[7].op = (uint8_t)JOP_CONST_I32; e1[7].a = 3; e1[7].imm = 0;
  e1[8].op = (uint8_t)JOP_DIV_I32; e1[8].a = 4; e1[8].b = 2; e1[8].c = 3;
  e1[9].op = (uint8_t)JOP_RET; e1[9].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 5; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 10; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

static void test_nested_jit_trap_propagates(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[10];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_self_trap_module(types, r0, r1, e0, e1, funcs, &m);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);
  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  jello_vm_destroy(vm);
  printf("test_nested_jit_trap_propagates: PASS\n");
}

static void test_wx_icache_prepare_exec_repeated(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  for(int i = 0; i < 50; i++) {
    jello_jit_module jm;
    memset(&jm, 0, sizeof(jm));
    assert(jello_jit_module_prepare(&m, &jm) == 1);
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
    assert(out.as.i32 == 5);
    jello_jit_module_free(&jm);
  }
  printf("test_wx_icache_prepare_exec_repeated: PASS\n");
}

static void test_nested_ret_does_not_hit_boundary_epilogue(void) {
  /* A recursive depth > 1 result only comes out correct if every nested RET
   * takes the has_caller path (write caller_dst, pop, native ret) instead
   * of falling into the shared !has_caller epilogue -- that epilogue would
   * return JELLO_EXEC_OK to the *caller's* JIT code as if it were a raw
   * status word, corrupting every subsequent add. depth 10 exercises the
   * nested path nine times before the outermost boundary return. */
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[8];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_self_sum_module(types, r0, r1, e0, e1, funcs, &m);
  e0[0].imm = 1;

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 1);
  printf("test_nested_ret_does_not_hit_boundary_epilogue: PASS\n");
}

/** The JIT's inline frame push writes has_caller and has_pointer, both single bytes, as bytes: a
 * wider store would overrun into the padding and caller_dst after them. The padding of the callee's
 * frame slot is filled before a run that pushes it and must come back unchanged. */
static void test_frame_push_writes_single_bytes(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  size_t pad_lo = offsetof(call_frame, has_pointer) + 1u;
  size_t pad_hi = offsetof(call_frame, caller_dst);
  assert(offsetof(call_frame, has_caller) + 1u == offsetof(call_frame, has_pointer));
  assert(pad_hi > pad_lo);

  jello_jit_module* jm = jit_module_create(&m, 1);
  assert(jm != NULL && jm->funcs[0].compiled && jm->funcs[1].compiled);
  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &m, &out) == JELLO_EXEC_OK && out.as.i32 == 5);

  assert(vm->call_frames_cap > 1u);
  uint8_t* slot = (uint8_t*)&vm->call_frames[1];
  memset(slot + pad_lo, 0xA5, pad_hi - pad_lo);
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &m, &out) == JELLO_EXEC_OK && out.as.i32 == 5);
  assert(vm->call_frames[1].has_caller == 1);
  for(size_t i = pad_lo; i < pad_hi; i++) assert(slot[i] == 0xA5);

  jello_vm_destroy(vm);
  jello_jit_module_destroy(jm);
  printf("test_frame_push_writes_single_bytes: PASS\n");
}

#endif

int main(void) {
  test_module_prepare_produces_one_blob();
  test_call_site_loads_entry_slot();
  test_module_prepare_rejects_call_in_single_compile();
  test_module_prepare_call_native();
  test_module_prepare_accepts_pointer_callee();
#if defined(__x86_64__) || defined(__aarch64__)
  test_call_self_recursive_sum();
  test_frame_stack_grows();
  test_destroy_after_trap_in_segment();
  test_call_direct_executes();
  test_nested_jit_trap_propagates();
  test_wx_icache_prepare_exec_repeated();
  test_call_self_survive_fib_correct();
  test_call_direct_survive_correct();
  test_call_survive_declines_across_branch();
  test_nested_survive_trap_propagates();
  test_nested_ret_does_not_hit_boundary_epilogue();
  test_frame_push_writes_single_bytes();
#else
  printf("test_call_self_recursive_sum: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_frame_stack_grows: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_call_direct_executes: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_nested_jit_trap_propagates: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_wx_icache_prepare_exec_repeated: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_call_self_survive_fib_correct: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_call_direct_survive_correct: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_call_survive_declines_across_branch: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_nested_survive_trap_propagates: SKIPPED (host is not x86_64 or aarch64)\n");
  printf("test_nested_ret_does_not_hit_boundary_epilogue: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
  printf("jit_calls_tests: all tests passed\n");
  return 0;
}
