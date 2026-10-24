// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for Enum ops, and jello_enum_new; an enum's fields are boxed values. */

#include <jello/internal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

static jello_enum* enum_init(jello_enum* e, uint32_t type_id, uint32_t tag, uint32_t nfields) {
  e->h.kind = (uint8_t)JELLO_OBJ_ENUM;
  e->h.type_id = type_id;
  e->tag = tag;
  e->nfields = nfields;
  return e;
}

jello_enum* vm_enum_immortal(uint32_t type_id, uint32_t tag) {
  return enum_init((jello_enum*)vm_immortal_alloc(offsetof(jello_enum, fields)), type_id, tag, 0);
}

jello_enum* jello_enum_new(jello_vm* vm, uint32_t type_id, uint32_t tag, uint32_t nfields,
                           const jello_value* fields) {
  /* Only a size_t no wider than the count can overflow. */
#if SIZE_MAX <= UINT32_MAX
  if((size_t)nfields > (SIZE_MAX - offsetof(jello_enum, fields)) / sizeof(jello_value)) abort();
#endif
  size_t payload = (size_t)nfields * sizeof(jello_value);
  size_t total = offsetof(jello_enum, fields) + payload;
  jello_enum* e = enum_init((jello_enum*)jello_gc_alloc(vm, total), type_id, tag, nfields);
  if(nfields && fields) {
    memcpy(e->fields, fields, payload);
  } else if(nfields) {
    for(uint32_t i = 0; i < nfields; i++) e->fields[i] = jello_make_null();
  }
  return e;
}

/** The one value of variant `tag` of enum type `type_id`, which has no fields; made immortal on
 * first use, so naming it costs a load rather than an allocation. Enums compare structurally, so
 * sharing it is invisible. */
static jello_enum* const_enum(const jello_bc_module* m, uint32_t type_id, uint32_t tag) {
  _Atomic(jello_enum*)* slot = &m->const_enums[m->const_enum_base[type_id] + tag];
  jello_enum* e = atomic_load_explicit(slot, memory_order_acquire);
  if(e) return e;
  jello_enum* made = vm_enum_immortal(type_id, tag);
  if(atomic_compare_exchange_strong_explicit(slot, &e, made, memory_order_acq_rel,
                                             memory_order_acquire)) {
    return made;
  }
  vm_immortal_free(made);
  return e;
}

op_result op_enum_new(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  call_frame* fr = current_frame(ctx);
  const jello_bc_function* f = fr->f;

  uint32_t type_id = f->reg_types[ins->a];
  uint32_t tag = ins->imm;
  uint32_t nfields = (uint32_t)ins->c;
  if(!nfields && m->const_enums) {
    vm_store_ptr(&fr->rf, ins->a, const_enum(m, type_id, tag));
    return OP_CONTINUE;
  }
  jello_enum* e = jello_enum_new(vm, type_id, tag, nfields, NULL);
  /* Boxing a field may collect before e reaches a register. */
  vm->gc_transient_root = e;
  for(uint32_t i = 0; i < nfields; i++) {
    e->fields[i] = vm_box_from_typed(vm, m, f, &fr->rf, (uint32_t)ins->b + i);
  }
  vm->gc_transient_root = NULL;
  vm_store_ptr(&fr->rf, ins->a, e);
  return OP_CONTINUE;
}

op_result op_enum_tag(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);

  jello_enum* e = (jello_enum*)vm_load_ptr(&fr->rf, ins->b);
  if(!e) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  if(((const jello_obj_header*)e)->kind != (uint8_t)JELLO_OBJ_ENUM) {
    ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  if(e->h.type_id != fr->f->reg_types[ins->b]) {
    ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, e->tag);
  return OP_CONTINUE;
}

op_result op_enum_get(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  call_frame* fr = current_frame(ctx);
  const jello_bc_function* f = fr->f;

  jello_enum* e = (jello_enum*)vm_load_ptr(&fr->rf, ins->b);
  if(!e) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  if(((const jello_obj_header*)e)->kind != (uint8_t)JELLO_OBJ_ENUM) {
    ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  if(e->h.type_id != f->reg_types[ins->b]) {
    ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= e->nfields) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  if(!vm_store_from_boxed(vm, m, f, &fr->rf, ins->a, e->fields[idx])) return OP_TRAP;
  return OP_CONTINUE;
}
