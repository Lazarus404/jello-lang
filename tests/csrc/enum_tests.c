// SPDX-License-Identifier: BSD-3-Clause

#include <jello/internal.h>
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static jello_bc_module module_with(const jello_type_entry* types, uint32_t ntypes,
                                   const jello_type_id* regs, uint32_t nregs,
                                   const jello_insn* insns, uint32_t ninsns,
                                   jello_bc_function* function) {
  memset(function, 0, sizeof(*function));
  function->nregs = nregs;
  function->reg_types = regs;
  function->ninsns = ninsns;
  function->insns = insns;

  jello_bc_module m = {0};
  m.ntypes = ntypes;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = function;
  return m;
}

static void test_execute_and_dynamic_kind(void) {
  const jello_type_entry types[] = {
    { .kind = JELLO_T_ENUM },
    { .kind = JELLO_T_I32 },
    { .kind = JELLO_T_DYNAMIC },
  };
  const jello_type_id regs[] = { 0, 1, 1, 1, 1, 2, 0 };
  const jello_insn insns[] = {
    { JOP_CONST_I32, 1, 0, 0, 42 },
    { JOP_ENUM_NEW, 0, 1, 1, 7 },
    { JOP_ENUM_TAG, 2, 0, 0, 0 },
    { JOP_CONST_I32, 3, 0, 0, 0 },
    { JOP_ENUM_GET, 4, 0, 3, 0 },
    { JOP_TO_DYN, 5, 0, 0, 0 },
    { JOP_KINDOF, 2, 5, 0, 0 },
    { JOP_ADD_I32, 4, 4, 2, 0 },
    { JOP_FROM_DYN_PTR, 6, 5, 0, 0 },
    { JOP_ENUM_TAG, 2, 6, 0, 0 },
    { JOP_ADD_I32, 4, 4, 2, 0 },
    { JOP_RET, 4, 0, 0, 0 },
  };
  jello_bc_function function = {0};
  jello_bc_module m = module_with(types, 3, regs, 7, insns, 12, &function);

  for(uint32_t pc = 0; pc < function.ninsns; pc++) {
    assert(jello_bc_validate_insn(&m, regs, &insns[pc], 7, pc, 12, 1).err == JELLO_BC_OK);
  }

  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 63);
  jello_vm_destroy(vm);
}

static void test_validation_rejects_bad_operands(void) {
  const jello_type_entry types[] = {
    { .kind = JELLO_T_ENUM },
    { .kind = JELLO_T_I32 },
    { .kind = JELLO_T_BOOL },
  };
  const jello_type_id regs[] = { 0, 1, 2 };
  jello_bc_function function = {0};
  jello_bc_module m = module_with(types, 3, regs, 3, NULL, 0, &function);
  const jello_insn bad_window = { JOP_ENUM_NEW, 0, 2, 2, 0 };
  const jello_insn bad_index = { JOP_ENUM_GET, 1, 0, 2, 0 };
  const jello_insn bad_tag_src = { JOP_ENUM_TAG, 1, 2, 0, 0 };

  assert(jello_bc_validate_insn(&m, regs, &bad_window, 3, 0, 1, 1).err == JELLO_BC_BAD_FORMAT);
  assert(jello_bc_validate_insn(&m, regs, &bad_index, 3, 0, 1, 1).err == JELLO_BC_BAD_FORMAT);
  assert(jello_bc_validate_insn(&m, regs, &bad_tag_src, 3, 0, 1, 1).err == JELLO_BC_BAD_FORMAT);
}

static void test_get_bounds_traps(void) {
  const jello_type_entry types[] = {
    { .kind = JELLO_T_ENUM },
    { .kind = JELLO_T_I32 },
  };
  const jello_type_id regs[] = { 0, 1, 1 };
  const jello_insn insns[] = {
    { JOP_ENUM_NEW, 0, 0, 0, 0 },
    { JOP_CONST_I32, 1, 0, 0, 0 },
    { JOP_ENUM_GET, 2, 0, 1, 0 },
    { JOP_RET, 2, 0, 0, 0 },
  };
  jello_bc_function function = {0};
  jello_bc_module m = module_with(types, 2, regs, 3, insns, 4, &function);
  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;

  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_BOUNDS);
  jello_vm_destroy(vm);
}

static void test_recursive_graph_survives_forced_gc(void) {
  jello_vm* vm = jello_vm_create();
  jello_enum* child = jello_enum_new(vm, 0, 1, 1, NULL);
  child->fields[0] = jello_from_ptr(child);
  jello_value field = jello_from_ptr(child);
  jello_enum* root = jello_enum_new(vm, 0, 2, 1, &field);

  jello_gc_push_root(vm, root);
  for(uint32_t i = 0; i < 8; i++) {
    for(uint32_t j = 0; j < 4096; j++) (void)jello_bytes_new(vm, 64);
    jello_gc_collect(vm);
    assert(root->tag == 2 && root->nfields == 1);
    child = (jello_enum*)jello_as_ptr(root->fields[0]);
    assert(child->tag == 1 && jello_as_ptr(child->fields[0]) == child);
  }
  jello_gc_pop_roots(vm, 1);
  jello_vm_destroy(vm);
}

int main(void) {
  test_execute_and_dynamic_kind();
  test_validation_rejects_bad_operands();
  test_get_bounds_traps();
  test_recursive_graph_survives_forced_gc();
  puts("enum_tests: passed");
  return 0;
}
