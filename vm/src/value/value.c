// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Boxing between typed registers and jello_value, used wherever a value crosses into Dynamic or
 * into a boxed slot (TO_DYN, FROM_DYN_*, object fields, list heads, enum fields, spill slots),
 * and the runtime kind queries KINDOF, SWITCH_KIND and IS_ENUM. */

#include <jello/internal.h>

static uint16_t vm_load_f16_bits(reg_frame* rf, uint32_t r) { return (uint16_t)vm_load_u32(rf, r); }
static void vm_store_f16_bits(reg_frame* rf, uint32_t r, uint16_t bits) {
  vm_store_u32(rf, r, (uint32_t)bits);
}

static uint32_t vm_expected_obj_kind_for_ptr_kind(jello_type_kind k) {
  switch(k) {
    case JELLO_T_BYTES: return (uint32_t)JELLO_OBJ_BYTES;
    case JELLO_T_STRUCT: return (uint32_t)JELLO_OBJ_STRUCT;
    case JELLO_T_OBJECT: return (uint32_t)JELLO_OBJ_OBJECT;
    case JELLO_T_ARRAY: return (uint32_t)JELLO_OBJ_ARRAY;
    case JELLO_T_FUNCTION: return (uint32_t)JELLO_OBJ_FUNCTION;
    case JELLO_T_LIST: return (uint32_t)JELLO_OBJ_LIST;
    case JELLO_T_ENUM: return (uint32_t)JELLO_OBJ_ENUM;
    default: return 0u;
  }
}

int vm_dynamic_fits_ptr(const jello_bc_module* m, jello_value v, jello_type_kind k,
                        jello_type_id type) {
  uint32_t want = vm_expected_obj_kind_for_ptr_kind(k);
  if(want == 0u || !jello_is_ptr(v) || jello_as_ptr(v) == NULL) return 0;
  const jello_obj_header* h = (const jello_obj_header*)jello_as_ptr(v);
  if(h->kind != (uint8_t)want) return 0;
  switch(k) {
    case JELLO_T_STRUCT: return h->type_id == m->types[type].as.strct.struct_sig_id;
    case JELLO_T_ARRAY:
    case JELLO_T_LIST:
    case JELLO_T_ENUM:
    case JELLO_T_FUNCTION: return h->type_id == type;
    default: return 1;
  }
}

uint32_t vm_dynamic_is_enum(jello_value v, jello_type_id type) {
  return !jello_is_null(v) && vm_dynamic_fits_ptr(NULL, v, JELLO_T_ENUM, type);
}

jello_value vm_box_from_typed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                              reg_frame* rf, uint32_t r) {
  jello_type_kind k = vm_reg_kind(m, f, r);
  switch(k) {
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32: return jello_make_i32((int32_t)vm_load_u32(rf, r));
    case JELLO_T_BOOL: return jello_make_bool((int)(vm_load_u32(rf, r) != 0u));
    case JELLO_T_ATOM: return jello_make_atom(vm_load_u32(rf, r));
    case JELLO_T_I64: return jello_from_ptr(jello_box_i64_new(vm, vm_load_i64(rf, r)));
    case JELLO_T_F16: return jello_from_ptr(jello_box_f16_new(vm, vm_load_f16_bits(rf, r)));
    case JELLO_T_F32: return jello_from_ptr(jello_box_f32_new(vm, vm_load_f32(rf, r)));
    case JELLO_T_F64: return jello_from_ptr(jello_box_f64_new(vm, vm_load_f64(rf, r)));
    case JELLO_T_DYNAMIC: return vm_load_val(rf, r);
    case JELLO_T_VOID: return jello_make_null();
    default: {
      /* Every other kind is a heap pointer; a null pointer boxes as null. */
      void* p = vm_load_ptr(rf, r);
      return p ? jello_from_ptr(p) : jello_make_null();
    }
  }
}

int vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                        reg_frame* rf, uint32_t dst, jello_value v) {
  jello_type_kind k = vm_reg_kind(m, f, dst);
  /* A scalar register cannot hold null; unboxing null is a null dereference. */
  if(jello_is_null(v) && k != JELLO_T_DYNAMIC && !vm_kind_is_gc_pointer(k)) {
    vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return 0;
  }
  switch(k) {
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
      if(!jello_is_i32(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_u32_masked(rf, dst, (uint32_t)jello_as_i32(v), k);
      return 1;
    case JELLO_T_I64:
      if(jello_is_i32(v)) {
        vm_store_i64(rf, dst, (int64_t)jello_as_i32(v));
        return 1;
      }
      if(!jello_is_box_i64(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_i64(rf, dst, jello_as_box_i64(v));
      return 1;
    case JELLO_T_F16:
      if(jello_is_i32(v)) {
        vm_store_f16_bits(rf, dst, vm_f32_to_f16((float)jello_as_i32(v)));
        return 1;
      }
      if(!jello_is_box_f16(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_f16_bits(rf, dst, jello_as_box_f16(v));
      return 1;
    case JELLO_T_F32:
      if(jello_is_i32(v)) {
        vm_store_f32(rf, dst, (float)jello_as_i32(v));
        return 1;
      }
      if(!jello_is_box_f32(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_f32(rf, dst, jello_as_box_f32(v));
      return 1;
    case JELLO_T_F64:
      if(jello_is_i32(v)) {
        vm_store_f64(rf, dst, (double)jello_as_i32(v));
        return 1;
      }
      if(!jello_is_box_f64(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_f64(rf, dst, jello_as_box_f64(v));
      return 1;
    case JELLO_T_BOOL:
      if(!jello_is_bool(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_u32(rf, dst, (uint32_t)jello_as_bool(v));
      return 1;
    case JELLO_T_ATOM:
      if(!jello_is_atom(v)) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_u32(rf, dst, jello_as_atom(v));
      return 1;
    case JELLO_T_DYNAMIC: vm_store_val(rf, dst, v); return 1;
    default: {
      /* Every other kind is a heap pointer; vm_dynamic_fits_ptr checks the object. */
      if(jello_is_null(v)) {
        vm_store_ptr(rf, dst, NULL);
        return 1;
      }
      if(!vm_dynamic_fits_ptr(m, v, k, f->reg_types[dst])) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_ptr(rf, dst, jello_as_ptr(v));
      return 1;
    }
  }
}

uint32_t vm_dynamic_static_kind(jello_value v) {
  if(jello_is_bool(v)) return (uint32_t)JELLO_T_BOOL;
  if(jello_is_i32(v)) return (uint32_t)JELLO_T_I32;
  if(jello_is_atom(v)) return (uint32_t)JELLO_T_ATOM;
  if(jello_is_null(v) || !jello_is_ptr(v) || jello_as_ptr(v) == NULL)
    return (uint32_t)JELLO_T_DYNAMIC;
  switch(((const jello_obj_header*)jello_as_ptr(v))->kind) {
    case JELLO_OBJ_BYTES: return (uint32_t)JELLO_T_BYTES;
    case JELLO_OBJ_STRUCT: return (uint32_t)JELLO_T_STRUCT;
    case JELLO_OBJ_OBJECT: return (uint32_t)JELLO_T_OBJECT;
    case JELLO_OBJ_BOX_I64: return (uint32_t)JELLO_T_I64;
    case JELLO_OBJ_BOX_F64: return (uint32_t)JELLO_T_F64;
    case JELLO_OBJ_BOX_F32: return (uint32_t)JELLO_T_F32;
    case JELLO_OBJ_BOX_F16: return (uint32_t)JELLO_T_F16;
    case JELLO_OBJ_ARRAY: return (uint32_t)JELLO_T_ARRAY;
    case JELLO_OBJ_FUNCTION: return (uint32_t)JELLO_T_FUNCTION;
    case JELLO_OBJ_LIST: return (uint32_t)JELLO_T_LIST;
    case JELLO_OBJ_ENUM: return (uint32_t)JELLO_T_ENUM;
    case JELLO_OBJ_HANDLE: return (uint32_t)JELLO_T_ABSTRACT;
    default: return (uint32_t)JELLO_T_DYNAMIC;
  }
}

uint32_t vm_kindof_dynamic(jello_value v) {
  if(jello_is_null(v)) return 0u;
  if(jello_is_bool(v)) return 1u;
  if(jello_is_i32(v)) return 2u;
  if(jello_is_atom(v)) return 3u;
  if(!jello_is_ptr(v) || jello_as_ptr(v) == NULL) return 0x7fffffffu;
  switch(((const jello_obj_header*)jello_as_ptr(v))->kind) {
    case JELLO_OBJ_BYTES: return 4u;
    case JELLO_OBJ_STRUCT: return 5u;
    case JELLO_OBJ_OBJECT: return 6u;
    case JELLO_OBJ_BOX_I64: return 7u;
    case JELLO_OBJ_BOX_F64: return 8u;
    case JELLO_OBJ_BOX_F32: return 9u;
    case JELLO_OBJ_BOX_F16: return 10u;
    case JELLO_OBJ_ARRAY: return 11u;
    case JELLO_OBJ_FUNCTION: return 12u;
    case JELLO_OBJ_LIST: return 13u;
    case JELLO_OBJ_ENUM: return 14u;
    case JELLO_OBJ_HANDLE: return 15u;
    default: return 0x7fffffffu;
  }
}
