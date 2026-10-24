// SPDX-License-Identifier: BSD-3-Clause
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <jello/internal.h>
#include <jello/internal/platform.h>

/* Runs `writer`, with r0 the Bytes constant `k` of the `nconsts` in `consts`, r1 0 and r2 7, in
 * a module of its own; the trap it ends with, or JELLO_TRAP_NONE. */
static jello_trap_code run_writer(jello_vm* vm, jello_bytes* const* consts, uint32_t nconsts,
                                  uint32_t k, jello_insn writer) {
  jello_type_entry types[2] = {0};
  types[0].kind = JELLO_T_BYTES;
  types[1].kind = JELLO_T_I32;
  jello_type_id reg_types[3] = {0, 1, 1};
  jello_insn insns[5] = {
      {JOP_CONST_BYTES, 0, 0, 0, k},
      {JOP_CONST_I32, 1, 0, 0, 0},
      {JOP_CONST_I32, 2, 0, 0, 7},
      writer,
      {JOP_RET, 1, 0, 0, 0},
  };
  jello_bc_function f = {0};
  f.name_atom = JELLO_BC_NO_NAME;
  f.nregs = 3;
  f.cap_start = 3;
  f.reg_types = reg_types;
  f.ninsns = 5;
  f.insns = insns;
  jello_bc_module m = {0};
  m.ntypes = 2;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &f;
  m.nconst_bytes = nconsts;
  m.const_bytes_values = consts;
  jello_numeric_result out = {0};
  if(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_OK) return JELLO_TRAP_NONE;
  return jello_vm_last_trap_code(vm);
}

/* Only a buffer may be written: every other Bytes, a constant above all, may be shared, so each
 * writer traps on one, interpreted and compiled alike, before it checks the range. A slice of a
 * buffer is a copy, and a frozen buffer is no buffer. */
static void check_writes_need_buffers(int jit) {
  if(jit) jello_unsetenv("JELLO_JIT_DISABLE");
  else jello_setenv("JELLO_JIT_DISABLE", "1");
  jello_vm* vm = jello_vm_create();
  assert(vm);
  jello_bytes* consts[4];
  consts[0] = jello_bytes_new_zeroed(vm, 4);
  jello_gc_push_root(vm, consts[0]);
  consts[1] = vm_buffer_new(vm, 4);
  jello_gc_push_root(vm, consts[1]);
  consts[2] = jello_bytes_slice(vm, consts[1], 0, 4);
  jello_gc_push_root(vm, consts[2]);
  consts[3] = jello_bytes_slice(vm, consts[0], 0, 4);
  jello_gc_push_root(vm, consts[3]);
  assert(!consts[2]->owner && consts[2]->data != consts[1]->data);
  assert(consts[3]->owner == consts[0]);
  const uint32_t nconsts = (uint32_t)(sizeof(consts) / sizeof(consts[0]));
  const struct {
    jello_insn insn;
    jello_trap_code on_buffer;
  } writers[] = {
      {{JOP_BYTES_SET_U8, 2, 0, 1, 0}, JELLO_TRAP_NONE},
      {{JOP_BYTES_SET_U8, 2, 0, 2, 0}, JELLO_TRAP_BOUNDS},
      {{JOP_BYTES_WRITE_U32_LE, 2, 0, 1, 0}, JELLO_TRAP_NONE},
      {{JOP_BITS_WRITE, 0, 2, 3, 0}, JELLO_TRAP_NONE},
      {{JOP_BYTES_BLIT, 0, 0, 1, 0}, JELLO_TRAP_NONE},
  };
  for(size_t w = 0; w < sizeof(writers) / sizeof(writers[0]); w++) {
    for(uint32_t k = 0; k < nconsts; k++) {
      jello_trap_code want = k == 1 ? writers[w].on_buffer : JELLO_TRAP_TYPE_MISMATCH;
      assert(run_writer(vm, consts, nconsts, k, writers[w].insn) == want);
    }
  }
  assert(consts[0]->data[0] == 0);
  assert(consts[1]->data[0] != 0);

  /* Freezing a buffer makes it plain; freezing plain Bytes leaves them alone. */
  const jello_insn freeze = {JOP_BYTES_FREEZE, 0, 0, 0, 0};
  assert(run_writer(vm, consts, nconsts, 0, freeze) == JELLO_TRAP_NONE);
  assert(run_writer(vm, consts, nconsts, 1, freeze) == JELLO_TRAP_NONE);
  assert(consts[0]->h.type_id == 0 && !vm_bytes_is_buffer(consts[1]));
  assert(run_writer(vm, consts, nconsts, 1, writers[0].insn) == JELLO_TRAP_TYPE_MISMATCH);
  jello_gc_pop_roots(vm, nconsts);
  jello_vm_destroy(vm);
  jello_unsetenv("JELLO_JIT_DISABLE");
}

int main(void) {
  check_writes_need_buffers(0);
  check_writes_need_buffers(1);

  jello_type_entry types[6] = {0};
  types[0].kind = JELLO_T_BYTES;
  types[1].kind = JELLO_T_LIST;
  types[1].as.unary.elem = 0;
  types[2].kind = JELLO_T_ARRAY;
  types[2].as.unary.elem = 1;
  types[3].kind = JELLO_T_DYNAMIC;
  types[4].kind = JELLO_T_I32;
  types[5].kind = JELLO_T_I64;

  jello_bc_module m = {0};
  m.ntypes = 6;
  m.types = types;

  jello_type_id dynamic_i32[2] = {3, 4};
  jello_insn add_dynamic = {JOP_ADD_I32_IMM, 1, 0, 0, 1};
  jello_insn to_dynamic = {JOP_TO_DYN, 0, 1, 0, 0};
  assert(jello_bc_validate_insn(&m, dynamic_i32, &add_dynamic, 2, 0, 1, 0).err == JELLO_BC_BAD_FORMAT);
  assert(jello_bc_validate_insn(&m, dynamic_i32, &to_dynamic, 2, 0, 1, 0).err == JELLO_BC_OK);

  jello_type_id dynamic_i64[2] = {3, 5};
  jello_insn mov_dynamic = {JOP_MOV, 1, 0, 0, 0};
  assert(jello_bc_validate_insn(&m, dynamic_i64, &mov_dynamic, 2, 0, 1, 0).err == JELLO_BC_BAD_FORMAT);

  jello_type_id sig_args[1] = {4};
  jello_fun_sig sig = {4, 1, sig_args};
  m.nsigs = 1;
  m.sigs = &sig;
  types[5].kind = JELLO_T_FUNCTION;
  types[5].as.fun.sig_id = 0;
  jello_type_id dynamic_callr[3] = {5, 3, 4};
  jello_insn callr_dynamic = {JOP_CALLR, 2, 0, 1, 1};
  assert(jello_bc_validate_insn(&m, dynamic_callr, &callr_dynamic, 3, 0, 1, 0).err == JELLO_BC_BAD_FORMAT);
  types[5].kind = JELLO_T_I64;

  jello_type_id wrong_array_get[3] = {2, 0, 4};
  jello_insn array_get = {JOP_ARRAY_GET, 1, 0, 2, 0};
  assert(jello_bc_validate_insn(&m, wrong_array_get, &array_get, 3, 0, 1, 0).err == JELLO_BC_BAD_FORMAT);

  jello_vm* vm = jello_vm_create();
  assert(vm);
  vm_enter_module(vm, &m);

  jello_array* array = jello_array_new(vm, &m, 2, 1);
  jello_bytes* bytes = jello_bytes_new(vm, 1);
  bytes->data[0] = 42;
  ((jello_list**)array->data)[0] = jello_list_cons(vm, 1, jello_from_ptr(bytes), NULL);

  jello_gc_push_root(vm, array);
  jello_gc_collect(vm);
  assert(vm->space->freed_objects == 0);
  jello_list* list = ((jello_list**)array->data)[0];
  assert(list && ((jello_bytes*)jello_as_ptr(list->head))->data[0] == 42);
  jello_gc_pop_roots(vm, 1);

  jello_vm_destroy(vm);
  puts("vm_safety_tests: PASS");
  return 0;
}
