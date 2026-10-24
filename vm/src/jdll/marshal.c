// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The jdl_* accessors a native export calls. None checks kinds; at load time each import's
 * signature was matched to the library's export, and each CALL_NATIVE's registers to the import. */

#include <jello/internal/jdll_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t arg_reg(jdlo_ctx* c, int index) { return (uint32_t)c->ins->b + (uint32_t)index; }

int32_t jdl_arg_i8(jdlo_ctx* c, int index) {
  return (int32_t)(int8_t)vm_load_u32(c->caller_rf, arg_reg(c, index));
}
int32_t jdl_arg_i16(jdlo_ctx* c, int index) {
  return (int32_t)(int16_t)vm_load_u32(c->caller_rf, arg_reg(c, index));
}
int32_t jdl_arg_i32(jdlo_ctx* c, int index) {
  return (int32_t)vm_load_u32(c->caller_rf, arg_reg(c, index));
}
int64_t jdl_arg_i64(jdlo_ctx* c, int index) { return vm_load_i64(c->caller_rf, arg_reg(c, index)); }
float jdl_arg_f16(jdlo_ctx* c, int index) {
  uint16_t bits = (uint16_t)vm_load_u32(c->caller_rf, arg_reg(c, index));
  return vm_f16_to_f32(bits);
}
float jdl_arg_f32(jdlo_ctx* c, int index) { return vm_load_f32(c->caller_rf, arg_reg(c, index)); }
double jdl_arg_f64(jdlo_ctx* c, int index) { return vm_load_f64(c->caller_rf, arg_reg(c, index)); }
int jdl_arg_bool(jdlo_ctx* c, int index) {
  return (int)vm_load_u32(c->caller_rf, arg_reg(c, index));
}
uint32_t jdl_arg_atom(jdlo_ctx* c, int index) {
  return vm_load_u32(c->caller_rf, arg_reg(c, index));
}

const uint8_t* jdl_arg_bytes_data(jdlo_ctx* c, int index) {
  jello_bytes* b = (jello_bytes*)vm_load_ptr(c->caller_rf, arg_reg(c, index));
  return b ? b->data : NULL;
}
uint32_t jdl_arg_bytes_len(jdlo_ctx* c, int index) {
  jello_bytes* b = (jello_bytes*)vm_load_ptr(c->caller_rf, arg_reg(c, index));
  return b ? b->length : 0u;
}
jello_value jdl_arg_value(jdlo_ctx* c, int index) {
  return vm_load_val(c->caller_rf, arg_reg(c, index));
}
const char* jdl_atom_name(jdlo_ctx* c, uint32_t atom_id) {
  return atom_id < c->m->natoms ? c->m->atoms[atom_id] : NULL;
}

void jdl_release_ret(jdlo_ctx* c) {
  free(c->ret_bytes);
  c->ret_bytes = NULL;
  c->ret_bytes_len = 0;
  if(c->handle_class && c->handle_class->finalize) c->handle_class->finalize(c->handle_payload);
  c->handle_class = NULL;
  c->handle_payload = NULL;
  c->handle_bytes = 0;
}

/** Starts a result of kind `k`, dropping an earlier one so that the last jdl_return_* wins. */
static void set_ret(jdlo_ctx* c, jello_type_kind k) {
  if(c->returned) jdl_release_ret(c);
  c->ret_kind_given = k;
  c->returned = 1;
}

void jdl_return_i8(jdlo_ctx* c, int32_t v) {
  set_ret(c, JELLO_T_I8);
  c->ret.i32 = v;
}
void jdl_return_i16(jdlo_ctx* c, int32_t v) {
  set_ret(c, JELLO_T_I16);
  c->ret.i32 = v;
}
void jdl_return_i32(jdlo_ctx* c, int32_t v) {
  set_ret(c, JELLO_T_I32);
  c->ret.i32 = v;
}
void jdl_return_i64(jdlo_ctx* c, int64_t v) {
  set_ret(c, JELLO_T_I64);
  c->ret.i64 = v;
}
void jdl_return_f16(jdlo_ctx* c, float v) {
  set_ret(c, JELLO_T_F16);
  c->ret.f32 = v;
}
void jdl_return_f32(jdlo_ctx* c, float v) {
  set_ret(c, JELLO_T_F32);
  c->ret.f32 = v;
}
void jdl_return_f64(jdlo_ctx* c, double v) {
  set_ret(c, JELLO_T_F64);
  c->ret.f64 = v;
}
void jdl_return_bool(jdlo_ctx* c, int v) {
  set_ret(c, JELLO_T_BOOL);
  c->ret.i32 = v ? 1 : 0;
}
void jdl_return_atom(jdlo_ctx* c, uint32_t atom_id) {
  set_ret(c, JELLO_T_ATOM);
  c->ret.i32 = (int32_t)atom_id;
}
void jdl_return_bytes_copy(jdlo_ctx* c, const uint8_t* data, uint32_t len) {
  uint8_t* copy = NULL;
  if(len) {
    copy = (uint8_t*)malloc(len);
    if(!copy) {
      jdl_fail(c, "out of memory");
      return;
    }
    memcpy(copy, data, len);
  }
  set_ret(c, JELLO_T_BYTES);
  c->ret_bytes = copy;
  c->ret_bytes_len = len;
}
void jdl_return_value(jdlo_ctx* c, jello_value v) {
  set_ret(c, JELLO_T_DYNAMIC);
  c->ret.dyn = v;
}
void jdl_return_void(jdlo_ctx* c) { set_ret(c, JELLO_T_VOID); }

void* jdl_arg_handle(jdlo_ctx* c, int index, const jello_jdll_handle_class* cls) {
  jello_value v = vm_load_val(c->caller_rf, arg_reg(c, index));
  if(jello_is_null(v)) return NULL;
  if(jello_is_ptr(v) && jello_as_ptr(v) != NULL) {
    const jello_handle* hd = (const jello_handle*)jello_as_ptr(v);
    if(hd->h.kind == (uint8_t)JELLO_OBJ_HANDLE && hd->cls == cls) return hd->payload;
  }
  jdl_fail(c, "native handle of the wrong class");
  return NULL;
}
void jdl_return_handle(jdlo_ctx* c, const jello_jdll_handle_class* cls, void* payload,
                       uint32_t native_bytes) {
  set_ret(c, JELLO_T_DYNAMIC);
  c->handle_class = cls;
  c->handle_payload = payload;
  c->handle_bytes = native_bytes;
  c->ret.dyn = jello_make_null();
}
void jdl_return_null(jdlo_ctx* c) { jdl_return_value(c, jello_make_null()); }

int32_t jdl_program_arg_count(jdlo_ctx* c) { return c->vm->heap->program_argc; }
const char* jdl_program_arg(jdlo_ctx* c, int32_t index) {
  const jello_heap* heap = c->vm->heap;
  return index >= 0 && index < heap->program_argc ? heap->program_argv[index] : NULL;
}

void jdl_fail(jdlo_ctx* c, const char* msg) {
  c->failed = 1;
  c->fail_said = msg != NULL;
  if(msg)
    snprintf(c->vm->trap_detail, sizeof c->vm->trap_detail, "native %s failed: %s",
             c->sig->resolved->name, msg);
}

jello_value jdl_object_get(const jello_object* o, uint32_t atom_id) {
  return jello_object_get(o, atom_id);
}
void jdl_object_seal(jello_object* o, uint32_t atom_id, jello_value v) {
  jello_object_seal(o, atom_id, v);
}
uint32_t jdl_object_keys(const jello_object* o, int skip_proto, uint32_t* out) {
  return jello_object_keys(o, skip_proto, out);
}

#define HOST_MEMBER(f) .f = f,
const jello_jdll_api jello_jdll_host_api = {JELLO_JDLL_API(HOST_MEMBER)};
