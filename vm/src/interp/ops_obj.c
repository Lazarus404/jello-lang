// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for Object ops, and the prototype-chain helpers JIT code shares. */

#include <jello/internal.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

/** Stores in `*out` the value of `atom_id` on `o` or, when prototypes are enabled, on the first
 * object of its prototype chain that has it, and in `*found` whether one does; `*out` is null
 * when none does. __proto__ itself is looked up on `o`. Each object is probed once, with `keep`
 * as vm_object_find takes it. Returns 0 with a BOUNDS trap when the chain passes about 1024
 * links, which stops a cycle. */
static int obj_chain_find(jello_vm* vm, const jello_bc_module* m, const jello_object* o,
                          uint32_t atom_id, int keep, jello_value* out, int* found) {
  int chained = m->proto_enabled && atom_id != JELLO_ATOM___PROTO__;
  const jello_object* cur = o;
  for(uint32_t steps = 0;; steps++) {
    *found = vm_object_find(cur, atom_id, keep, out);
    if(*found) return 1;
    if(!chained || !cur->proto) {
      *out = jello_make_null();
      return 1;
    }
    cur = cur->proto;
    if(steps > 1024u) {
      vm->trap_code = JELLO_TRAP_BOUNDS;
      return 0;
    }
  }
}

/** Stores in `*out` the value of `atom_id` that `o` finds (obj_chain_find), or null. */
static int obj_get_chain(jello_vm* vm, const jello_bc_module* m, const jello_object* o,
                         uint32_t atom_id, int keep, jello_value* out) {
  int found;
  return obj_chain_find(vm, m, o, atom_id, keep, out, &found);
}

/** Stores in `*out` whether `o` finds `atom_id` (obj_chain_find). */
static int obj_has_chain(jello_vm* vm, const jello_bc_module* m, const jello_object* o,
                         uint32_t atom_id, uint32_t* out) {
  jello_value v;
  int found;
  if(!obj_chain_find(vm, m, o, atom_id, 0, &v, &found)) return 0;
  *out = (uint32_t)found;
  return 1;
}

/** The Object `v` points at, which a write of __proto__ links to, or NULL. */
static jello_object* obj_link_of(jello_value v) {
  if(!jello_is_ptr(v) || jello_as_ptr(v) == NULL) return NULL;
  jello_object* o = (jello_object*)jello_as_ptr(v);
  return o->h.kind == (uint8_t)JELLO_OBJ_OBJECT ? o : NULL;
}

/** Whether `o`, or an Object along its prototype chain, seals `atom_id`, or any slot when
 * `atom_id` is UINT32_MAX. Links past about 1024, which only a cycle reaches, are not followed. */
static int obj_chain_seals(const jello_object* o, uint32_t atom_id) {
  for(uint32_t steps = 0; o && steps <= 1024u; o = o->proto, steps++) {
    if(!o->sealed) continue;
    if(atom_id == UINT32_MAX || vm_object_state(o, atom_id) == JELLO_OBJ_SLOT_SEALED) return 1;
  }
  return 0;
}

/** Whether linking `o` to `link` in place of its prototype would change a method that calls are
 * compiled against: its chain seals a slot, or `link`'s seals a key `o` has unsealed. */
static int obj_relink_seals(const jello_object* o, const jello_object* link) {
  if(obj_chain_seals(o->proto, UINT32_MAX)) return 1;
  if(!obj_chain_seals(link, UINT32_MAX)) return 0;
  for(uint32_t i = 0; i < o->cap; i++) {
    uint8_t state = o->states[i];
    if(vm_obj_slot_live(state) && state != JELLO_OBJ_SLOT_SEALED &&
       o->keys[i] != JELLO_ATOM___PROTO__ && obj_chain_seals(link, o->keys[i])) {
      return 1;
    }
  }
  return 0;
}

/** Whether writing `atom_id` on `o`, linking it to `link` when that is __proto__, would change
 * a method that calls are compiled against: a sealed slot of `o`, a new key that shadows one
 * along `o`'s chain, or a new link (obj_relink_seals). Sets a SEALED trap when it would. Objects
 * nothing seals, the usual case, cost a walk of their chain and no probe. */
static int obj_write_sealed(jello_vm* vm, const jello_bc_module* m, const jello_object* o,
                            uint32_t atom_id, const jello_object* link) {
  int links = m->proto_enabled && atom_id == JELLO_ATOM___PROTO__;
  int chained = m->proto_enabled && obj_chain_seals(o->proto, UINT32_MAX);
  if(!o->sealed && !chained && !(links && obj_chain_seals(link, UINT32_MAX))) return 0;
  uint8_t state = vm_object_state(o, atom_id);
  int sealed = state == JELLO_OBJ_SLOT_SEALED;
  if(!sealed && links) {
    sealed = link != o->proto && obj_relink_seals(o, link);
  } else if(!sealed && chained && state == JELLO_OBJ_SLOT_EMPTY) {
    sealed = obj_chain_seals(o->proto, atom_id);
  }
  if(sealed) vm->trap_code = JELLO_TRAP_SEALED;
  return sealed;
}

/** Sets `o`'s own slot. When prototypes are enabled and `atom_id` is __proto__, also sets `o`'s
 * prototype link to `v` when it is an Object, and clears it otherwise. Returns 0, writing
 * nothing, with a SEALED trap when the write would change a sealed method (obj_write_sealed). */
static int obj_set_direct(jello_vm* vm, const jello_bc_module* m, jello_object* o, uint32_t atom_id,
                          jello_value v) {
  int links = m->proto_enabled && atom_id == JELLO_ATOM___PROTO__;
  jello_object* link = links ? obj_link_of(v) : NULL;
  if(obj_write_sealed(vm, m, o, atom_id, link)) return 0;
  jello_object_set(o, atom_id, v);
  if(links) o->proto = link;
  return 1;
}

/** As obj_set_direct, for an F64. */
static int obj_set_f64(jello_vm* vm, const jello_bc_module* m, jello_object* o, uint32_t atom_id,
                       double d) {
  if(obj_write_sealed(vm, m, o, atom_id, NULL)) return 0;
  vm_object_set_f64(vm, o, atom_id, d);
  if(m->proto_enabled && atom_id == JELLO_ATOM___PROTO__) o->proto = NULL;
  return 1;
}

void jit_obj_set_f64(jello_vm* vm, const jello_bc_module* m, jello_object* o, uint32_t atom_id,
                     double d) {
  vm->trap_code = JELLO_TRAP_NONE;
  (void)obj_set_f64(vm, m, o, atom_id, d);
}

/** Sets `o`'s own `atom_id` to register `r`, boxing it unless it is an F64; as obj_set_direct. */
static int obj_set_typed(exec_ctx* ctx, call_frame* fr, jello_object* o, uint32_t atom_id,
                         uint32_t r) {
  if(vm_reg_kind_fast(&fr->rf, r) == JELLO_T_F64) {
    return obj_set_f64(ctx->vm, ctx->m, o, atom_id, vm_load_f64(&fr->rf, r));
  }
  jello_value v = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, r);
  return obj_set_direct(ctx->vm, ctx->m, o, atom_id, v);
}

op_result op_obj_new(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t type_id = fr->f->reg_types[ins->a];
  jello_object* o = jello_object_new(ctx->vm, type_id);
  vm_store_ptr(&fr->rf, ins->a, o);
  return OP_CONTINUE;
}

op_result op_obj_has_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t has;
  if(!obj_has_chain(ctx->vm, ctx->m, o, ins->imm, &has)) return OP_TRAP;
  vm_store_u32(&fr->rf, ins->a, has);
  return OP_CONTINUE;
}

op_result op_obj_get_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_value v;
  int keep = vm_reg_kind_fast(&fr->rf, ins->a) == JELLO_T_DYNAMIC;
  if(!obj_get_chain(ctx->vm, ctx->m, o, ins->imm, keep, &v)) return OP_TRAP;
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_obj_set_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  return obj_set_typed(ctx, fr, o, ins->imm, ins->a) ? OP_CONTINUE : OP_TRAP;
}

op_result op_obj_get(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_value v;
  int keep = vm_reg_kind_fast(&fr->rf, ins->a) == JELLO_T_DYNAMIC;
  if(!obj_get_chain(ctx->vm, ctx->m, o, atom_id, keep, &v)) return OP_TRAP;
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_obj_set(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  return obj_set_typed(ctx, fr, o, atom_id, ins->a) ? OP_CONTINUE : OP_TRAP;
}

op_result op_obj_has(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t has;
  if(!obj_has_chain(ctx->vm, ctx->m, o, atom_id, &has)) return OP_TRAP;
  vm_store_u32(&fr->rf, ins->a, has);
  return OP_CONTINUE;
}

op_result op_obj_remove(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t removed = jit_obj_remove(ctx->vm, o, atom_id);
  if(ctx->vm->trap_code != JELLO_TRAP_NONE) return OP_TRAP;
  vm_store_u32(&fr->rf, ins->a, removed);
  return OP_CONTINUE;
}

op_result op_obj_clear(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jit_obj_clear(ctx->vm, o);
  return ctx->vm->trap_code != JELLO_TRAP_NONE ? OP_TRAP : OP_CONTINUE;
}

jello_value jit_obj_chain_get(jello_vm* vm, const jello_bc_module* m, jello_object* o,
                              uint32_t atom_id, int keep) {
  vm->trap_code = JELLO_TRAP_NONE;
  jello_value v;
  if(!obj_get_chain(vm, m, o, atom_id, keep, &v)) return jello_make_null();
  return v;
}

uint32_t jit_obj_chain_has(jello_vm* vm, const jello_bc_module* m, const jello_object* o,
                           uint32_t atom_id) {
  vm->trap_code = JELLO_TRAP_NONE;
  uint32_t has = 0u;
  (void)obj_has_chain(vm, m, o, atom_id, &has);
  return has;
}

void jit_obj_set_direct(jello_vm* vm, const jello_bc_module* m, jello_object* o, uint32_t atom_id,
                        jello_value v) {
  vm->trap_code = JELLO_TRAP_NONE;
  (void)obj_set_direct(vm, m, o, atom_id, v);
}

uint32_t jit_obj_remove(jello_vm* vm, jello_object* o, uint32_t atom_id) {
  vm->trap_code = JELLO_TRAP_NONE;
  if(o->sealed && vm_object_state(o, atom_id) == JELLO_OBJ_SLOT_SEALED) {
    vm->trap_code = JELLO_TRAP_SEALED;
    return 0u;
  }
  return (uint32_t)(jello_object_remove(o, atom_id) != 0);
}

void jit_obj_clear(jello_vm* vm, jello_object* o) {
  vm->trap_code = o->sealed ? JELLO_TRAP_SEALED : JELLO_TRAP_NONE;
  if(!o->sealed) jello_object_clear(o);
}

op_result op_obj_copy(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* src = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!src) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_object* dst = jello_object_copy(ctx->vm, src);
  vm_store_ptr(&fr->rf, ins->a, dst);
  return OP_CONTINUE;
}

/** The object's own keys as an Array<Atom> in ascending atom order; __proto__ is a link rather
 * than a key when prototypes are enabled, so it is left out. */
op_result op_obj_keys(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  int skip_proto = ctx->m->proto_enabled && jello_object_has(o, JELLO_ATOM___PROTO__);
  /* The collector does not move objects, and `o` stays reachable from its register. */
  jello_array* arr =
      jello_array_new(ctx->vm, ctx->m, fr->f->reg_types[ins->a], o->len - (uint32_t)skip_proto);
  jello_object_keys(o, skip_proto, (uint32_t*)arr->data);
  vm_store_ptr(&fr->rf, ins->a, arr);
  return OP_CONTINUE;
}
