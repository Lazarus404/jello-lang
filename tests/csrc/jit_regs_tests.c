// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* func[0] fills the x64 GPR pool in order (RSI, RDI, R8-R11) with an object and five I32s, so the
 * atom lands in R14, then sets and gets a field through it and passes the result to func[1],
 * which returns it plus one: 56. */
typedef struct regs_module {
  jello_type_entry types[3];
  jello_type_id r0[9], r1[2];
  jello_insn e0[12], e1[2];
  jello_bc_function funcs[2];
  const char* atoms[1];
  jello_bc_module m;
} regs_module;

static void build_regs_module(regs_module* rm) {
  memset(rm, 0, sizeof(*rm));
  rm->types[0].kind = JELLO_T_OBJECT;
  rm->types[1].kind = JELLO_T_I32;
  rm->types[2].kind = JELLO_T_ATOM;
  static const jello_type_id r0[9] = {0, 1, 1, 1, 1, 1, 2, 1, 1};
  memcpy(rm->r0, r0, sizeof(r0));
  rm->r1[0] = 1; rm->r1[1] = 1;

  jello_insn* e = rm->e0;
  e[0].op = (uint8_t)JOP_OBJ_NEW; e[0].a = 0;
  for(uint8_t r = 1; r <= 4; r++) {
    e[r].op = (uint8_t)JOP_CONST_I32; e[r].a = r; e[r].imm = r;
  }
  e[5].op = (uint8_t)JOP_CONST_I32; e[5].a = 5; e[5].imm = 55;
  e[6].op = (uint8_t)JOP_CONST_ATOM; e[6].a = 6; e[6].imm = 0;
  e[7].op = (uint8_t)JOP_OBJ_SET; e[7].a = 5; e[7].b = 0; e[7].c = 6;
  e[8].op = (uint8_t)JOP_OBJ_GET; e[8].a = 7; e[8].b = 0; e[8].c = 6;
  e[9].op = (uint8_t)JOP_CALL; e[9].a = 8; e[9].b = 7; e[9].c = 1; e[9].imm = 1;
  e[10].op = (uint8_t)JOP_RET; e[10].a = 8;
  rm->e1[0].op = (uint8_t)JOP_ADD_I32_IMM; rm->e1[0].a = 1; rm->e1[0].b = 0; rm->e1[0].imm = 1;
  rm->e1[1].op = (uint8_t)JOP_RET; rm->e1[1].a = 1;

  rm->funcs[0].nregs = 9; rm->funcs[0].cap_start = 9; rm->funcs[0].reg_types = rm->r0;
  rm->funcs[0].ninsns = 11; rm->funcs[0].insns = rm->e0;
  rm->funcs[0].name_atom = JELLO_BC_NO_NAME;
  rm->funcs[1].nargs = 1; rm->funcs[1].nregs = 2; rm->funcs[1].cap_start = 2;
  rm->funcs[1].reg_types = rm->r1; rm->funcs[1].ninsns = 2; rm->funcs[1].insns = rm->e1;
  rm->funcs[1].name_atom = JELLO_BC_NO_NAME;

  rm->atoms[0] = "x";
  rm->m.ntypes = 3; rm->m.types = rm->types;
  rm->m.nfuncs = 2; rm->m.funcs = rm->funcs;
  rm->m.atoms = rm->atoms; rm->m.natoms = 1;
}

#if defined(__x86_64__) || defined(__aarch64__)
/* OBJ_SET and OBJ_GET read their atom operand after staging the object; with the atom in R14,
 * staging the object must not overwrite it. */
static void test_obj_set_get_with_atom_in_last_pool_register(void) {
  regs_module rm;
  build_regs_module(&rm);
  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&rm.m, &jm) == 1);
  assert(jm.ncompiled == jm.nfuncs);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, &jm, &rm.m, &out);
  if(st != JELLO_EXEC_OK || out.kind != JELLO_NUM_I32 || out.as.i32 != 56)
    fprintf(stderr, "obj set/get: st=%d trap=%d kind=%d value=%d\n", (int)st,
            (int)jello_vm_last_trap_code(vm), (int)out.kind, out.as.i32);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 56);
  jello_vm_destroy(vm);
  jello_jit_module_free(&jm);
  printf("test_obj_set_get_with_atom_in_last_pool_register: PASS\n");
}

/* A struct of 4100 F64s and then an I32, so the last two fields lie past the scaled 12-bit offset
 * of an arm64 8-byte (32760) and 4-byte (16380) load or store. func[0] stores 2.0 and 40 there,
 * reads both back and returns their sum: 42. */
#define FAR_F64_FIELDS 4100u

static void test_struct_fields_at_large_offsets(void) {
  static uint32_t field_type[FAR_F64_FIELDS + 1], field_offset[FAR_F64_FIELDS + 1];
  for(uint32_t i = 0; i < FAR_F64_FIELDS; i++) {
    field_type[i] = 2;
    field_offset[i] = i * 8u;
  }
  field_type[FAR_F64_FIELDS] = 1;
  field_offset[FAR_F64_FIELDS] = FAR_F64_FIELDS * 8u;
  jello_struct_sig sig;
  memset(&sig, 0, sizeof(sig));
  sig.nfields = (uint16_t)(FAR_F64_FIELDS + 1);
  sig.is_mutable = 1;
  sig.field_type = field_type;
  sig.field_offset = field_offset;

  jello_type_entry types[3];
  memset(types, 0, sizeof(types));
  types[0].kind = JELLO_T_STRUCT;
  types[0].as.strct.struct_sig_id = 0;
  types[1].kind = JELLO_T_I32;
  types[2].kind = JELLO_T_F64;
  static const jello_type_id reg_types[7] = {0, 1, 2, 2, 1, 1, 1};

  const uint32_t f64_field = FAR_F64_FIELDS - 1, i32_field = FAR_F64_FIELDS;
  jello_insn e[11];
  memset(e, 0, sizeof(e));
  e[0].op = (uint8_t)JOP_STRUCT_NEW; e[0].a = 0;
  e[1].op = (uint8_t)JOP_CONST_I32; e[1].a = 1; e[1].imm = 2;
  e[2].op = (uint8_t)JOP_F64_FROM_I32; e[2].a = 2; e[2].b = 1;
  e[3].op = (uint8_t)JOP_STRUCT_SET; e[3].a = 2; e[3].b = 0; e[3].imm = f64_field;
  e[4].op = (uint8_t)JOP_CONST_I32; e[4].a = 1; e[4].imm = 40;
  e[5].op = (uint8_t)JOP_STRUCT_SET; e[5].a = 1; e[5].b = 0; e[5].imm = i32_field;
  e[6].op = (uint8_t)JOP_STRUCT_GET; e[6].a = 3; e[6].b = 0; e[6].imm = f64_field;
  e[7].op = (uint8_t)JOP_STRUCT_GET; e[7].a = 4; e[7].b = 0; e[7].imm = i32_field;
  e[8].op = (uint8_t)JOP_I32_FROM_F64; e[8].a = 5; e[8].b = 3;
  e[9].op = (uint8_t)JOP_ADD_I32; e[9].a = 6; e[9].b = 4; e[9].c = 5;
  e[10].op = (uint8_t)JOP_RET; e[10].a = 6;

  jello_bc_function func = {0};
  func.nregs = 7;
  func.cap_start = 7;
  func.reg_types = reg_types;
  func.ninsns = 11;
  func.insns = e;
  func.name_atom = JELLO_BC_NO_NAME;
  jello_bc_module m = {0};
  m.ntypes = 3;
  m.types = types;
  m.nstruct_sigs = 1;
  m.struct_sigs = &sig;
  m.nfuncs = 1;
  m.funcs = &func;

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  assert(jm.ncompiled == jm.nfuncs);
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, &jm, &m, &out);
  if(st != JELLO_EXEC_OK || out.kind != JELLO_NUM_I32 || out.as.i32 != 42)
    fprintf(stderr, "far struct fields: st=%d trap=%d kind=%d value=%d\n", (int)st,
            (int)jello_vm_last_trap_code(vm), (int)out.kind, out.as.i32);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 42);
  jello_vm_destroy(vm);
  jello_jit_module_free(&jm);
  printf("test_struct_fields_at_large_offsets: PASS\n");
}
#endif

#if defined(__x86_64__)
/* jello_cs_trampoline(entry, vm, base, out, regs): loads sentinels into RBX, RBP and R12-R15,
 * calls entry(vm, base, out) and stores what those registers hold afterwards in regs[0..5],
 * preserving its own caller's values. */
JELLO_JIT_ABI jello_exec_status jello_cs_trampoline(const void* entry, jello_vm* vm,
    uint8_t* base, jello_numeric_result* out, uint64_t* regs)
    __asm__("jello_cs_trampoline");

__asm__(
    ".text\n"
    ".globl jello_cs_trampoline\n"
    "jello_cs_trampoline:\n"
    "  pushq %rbp\n"
    "  pushq %rbx\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  pushq %r8\n" // seven pushes and the return address keep RSP 16-byte aligned
    "  movq %rdi, %rax\n"
    "  movq %rsi, %rdi\n"
    "  movq %rdx, %rsi\n"
    "  movq %rcx, %rdx\n"
    "  movabsq $0x1111111111111111, %rbx\n"
    "  movabsq $0x2222222222222222, %rbp\n"
    "  movabsq $0x3333333333333333, %r12\n"
    "  movabsq $0x4444444444444444, %r13\n"
    "  movabsq $0x5555555555555555, %r14\n"
    "  movabsq $0x6666666666666666, %r15\n"
    "  callq *%rax\n"
    "  popq %r8\n"
    "  movq %rbx, 0(%r8)\n"
    "  movq %rbp, 8(%r8)\n"
    "  movq %r12, 16(%r8)\n"
    "  movq %r13, 24(%r8)\n"
    "  movq %r14, 32(%r8)\n"
    "  movq %r15, 40(%r8)\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %rbx\n"
    "  popq %rbp\n"
    "  retq\n");

/* SysV makes RBX, RBP and R12-R15 callee-saved, so a C call into JIT code must return them
 * unchanged, even when the code uses R14 from its pool and for its calls. */
static void test_entry_preserves_callee_saved_registers(void) {
  regs_module rm;
  build_regs_module(&rm);
  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&rm.m, &jm) == 1);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  atomic_store(&vm->in_exec, 1);
  vm_frame_reset(vm);
  vm_enter_module(vm, &rm.m);
  vm_warm_jit_layouts(vm, &rm.m);
  assert(vm_push_frame(vm, &rm.m, 0, 0, 0, 0, 0) == 1);

  const void* entry = jm.blob.mem + jm.funcs[0].blob_off + jm.funcs[0].entry_off;
  uint64_t regs[6];
  memset(regs, 0, sizeof(regs));
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_cs_trampoline(entry, vm, jit_frame_entry_base(vm), &out, regs);
  atomic_store(&vm->in_exec, 0);

  static const char* names[6] = {"rbx", "rbp", "r12", "r13", "r14", "r15"};
  int bad = 0;
  for(int i = 0; i < 6; i++) {
    uint64_t want = 0x1111111111111111ull * (uint64_t)(i + 1);
    if(regs[i] != want) {
      fprintf(stderr, "%s: 0x%016llx, expected 0x%016llx\n", names[i],
              (unsigned long long)regs[i], (unsigned long long)want);
      bad = 1;
    }
  }
  assert(!bad && "JIT entry clobbered a callee-saved register");
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 56);

  jello_vm_destroy(vm);
  jello_jit_module_free(&jm);
  printf("test_entry_preserves_callee_saved_registers: PASS\n");
}
#endif

int main(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  test_obj_set_get_with_atom_in_last_pool_register();
  test_struct_fields_at_large_offsets();
#else
  printf("test_obj_set_get_with_atom_in_last_pool_register: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
#if defined(__x86_64__)
  test_entry_preserves_callee_saved_registers();
#else
  printf("test_entry_preserves_callee_saved_registers: SKIPPED (host is not x86_64)\n");
#endif
  printf("jit_regs_tests: all tests passed\n");
  return 0;
}
