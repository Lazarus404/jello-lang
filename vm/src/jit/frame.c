// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * C helpers JIT code calls; Bytes and bit operations, the frame stack base, interpreter handlers,
 * CALLR, throws, and the bridge into the interpreter. */

#include <jello/internal/jit_internal.h>
#include <jello/internal/vm_internal.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t* jit_frame_entry_base(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  return fr->rf.mem;
}

jello_bytes* jit_bytes_bitop(jello_vm* vm, jello_bytes* b, jello_bytes* c, int op_kind) {
  if(b->length != c->length) {
    vm->trap_code = JELLO_TRAP_BOUNDS;
    return NULL;
  }
  jello_bytes* dst = jello_bytes_new(vm, b->length);
  uint32_t n = b->length;
  for(uint32_t i = 0; i < n; i++) {
    switch(op_kind) {
      case 0: dst->data[i] = (uint8_t)(b->data[i] & c->data[i]); break;
      case 1: dst->data[i] = (uint8_t)(b->data[i] | c->data[i]); break;
      default: dst->data[i] = (uint8_t)(b->data[i] ^ c->data[i]); break;
    }
  }
  return dst;
}

void jit_bits_write(jello_bytes* b, uint32_t width, uint32_t bit_off, uint32_t value) {
  for(uint32_t i = 0; i < width; i++) {
    uint32_t bit_val = (value >> (width - 1u - i)) & 1u;
    uint64_t dst_bit = (uint64_t)bit_off + i;
    uint32_t byte_idx = (uint32_t)(dst_bit / 8u);
    uint32_t bit_in_byte = (uint32_t)(dst_bit % 8u);
    uint8_t mask = (uint8_t)(0x80u >> bit_in_byte);
    if(bit_val) b->data[byte_idx] = (uint8_t)(b->data[byte_idx] | mask);
    else b->data[byte_idx] = (uint8_t)(b->data[byte_idx] & (uint8_t)~mask);
  }
}

jello_bytes* jit_bytes_concat2(jello_vm* vm, jello_bytes* b, jello_bytes* c) {
  jello_bytes* parts[2] = {b, c};
  return jello_bytes_join(vm, parts, 2, NULL);
}

uint32_t jit_bits_read(const jello_bytes* b, uint32_t width, uint32_t bit_off) {
  uint32_t value = 0;
  for(uint32_t i = 0; i < width; i++) {
    uint64_t src_bit = (uint64_t)bit_off + i;
    uint32_t byte_idx = (uint32_t)(src_bit / 8u);
    uint32_t bit_in_byte = (uint32_t)(src_bit % 8u);
    uint32_t bit_val = (uint32_t)(b->data[byte_idx] >> (7u - bit_in_byte)) & 1u;
    value = (value << 1) | bit_val;
  }
  return value;
}

int jit_run_handler(jello_vm* vm, const jello_bc_module* m, uint32_t fi, uint32_t pc) {
  exec_ctx ctx = {vm, m, NULL, 0};
  if(op_dispatch(&ctx, &m->funcs[fi].insns[pc]) != OP_TRAP) return 0;
  vm->trap_func_index = fi;
  vm->trap_pc = pc;
  return 1;
}

const void* jit_callr_push(jello_vm* vm, uint32_t dst, uint32_t callee_reg, uint32_t arg_first,
                           uint32_t nargs) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  const jello_function* fn = vm_push_callr_frame(
      vm, vm->running_module, vm_load_val(&fr->rf, callee_reg), dst, arg_first, nargs);
  return fn ? vm->active_jit->entries[fn->func_index] : NULL;
}

void jit_throw(jello_vm* vm, uint32_t payload_reg, uint32_t fi, uint32_t pc) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  jello_value payload = vm_load_val(&fr->rf, payload_reg);
  if(vm_throw_find(vm, payload)) {
    vm->trap_code = JELLO_TRAP_INTERNAL_UNWIND;
    return;
  }
  vm->trap_code = JELLO_TRAP_UNCAUGHT_THROW;
  vm->thrown = payload;
  vm->trap_func_index = fi;
  vm->trap_pc = pc;
}

void jit_unwind_lost(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  fprintf(stderr, "jello: internal error: unwind reached function %u at pc %u, not a handler\n",
          fr->func_index, fr->pc);
  abort();
}

void jit_trap_pop_frame(jello_vm* vm) {
  vm_trace_frames(vm, vm->call_frames_len - 1u);
  vm_pop_frame(vm);
}

void jit_run_interp_top(jello_vm* vm) {
  uint32_t stop = vm->call_frames_len - 1u;
  exec_ctx ctx = {vm, vm->running_module, NULL, stop};
  vm->jit_bridges++;
  if(vm_exec_loop(&ctx) == JELLO_EXEC_OK) return;
  vm_trace_frames(vm, stop);
  while(vm->call_frames_len > stop) vm_pop_frame(vm);
}
