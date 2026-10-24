// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* trap_code_name(jello_trap_code tc) {
  switch(tc) {
    case JELLO_TRAP_NONE: return "NONE";
    case JELLO_TRAP_ARITHMETIC: return "ARITHMETIC";
    case JELLO_TRAP_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case JELLO_TRAP_NULL_DEREF: return "NULL_DEREF";
    case JELLO_TRAP_BOUNDS: return "BOUNDS";
    case JELLO_TRAP_TYPE_MISMATCH: return "TYPE_MISMATCH";
    case JELLO_TRAP_FUEL: return "FUEL";
    case JELLO_TRAP_LIMIT: return "LIMIT";
    case JELLO_TRAP_UNCAUGHT_THROW: return "UNCAUGHT_THROW";
    default: return "UNKNOWN";
  }
}

/* See jlo_tool.c's own copy of this printer for why PTR/DYN are printed as
 * comparable observations (NULL-vs-non-NULL, decoded tag) rather than raw
 * heap identity, which is never comparable across two separate process
 * runs. */
static void print_numeric_result(const jello_numeric_result* r) {
  switch(r->kind) {
    case JELLO_NUM_VOID: printf("RESULT kind=VOID\n"); break;
    case JELLO_NUM_BOOL: printf("RESULT kind=BOOL value=%d\n", r->as.b); break;
    case JELLO_NUM_I32: printf("RESULT kind=I32 value=%d\n", r->as.i32); break;
    case JELLO_NUM_I64: printf("RESULT kind=I64 value=%lld\n", (long long)r->as.i64); break;
    case JELLO_NUM_F32: printf("RESULT kind=F32 value=%.9g\n", (double)r->as.f32); break;
    case JELLO_NUM_F64: printf("RESULT kind=F64 value=%.17g\n", r->as.f64); break;
    case JELLO_NUM_PTR:
      printf("RESULT kind=PTR value=%s\n", r->as.ptr ? "NON_NULL" : "NULL");
      break;
    case JELLO_NUM_DYN: {
      jello_value v = r->as.dyn;
      if(jello_is_null(v)) printf("RESULT kind=DYN tag=NULL\n");
      else if(jello_is_i32(v)) printf("RESULT kind=DYN tag=I32 value=%d\n", jello_as_i32(v));
      else if(jello_is_bool(v)) printf("RESULT kind=DYN tag=BOOL value=%d\n", jello_as_bool(v));
      else if(jello_is_atom(v)) printf("RESULT kind=DYN tag=ATOM value=%u\n", jello_as_atom(v));
      else printf("RESULT kind=DYN tag=PTR value=%s\n", jello_as_ptr(v) ? "NON_NULL" : "NULL");
      break;
    }
  }
}

/* Runs a module with the JIT. By default every function must compile
 * (JIT_SKIP otherwise); --mixed runs whatever compiled, the rest
 * interpreted. */
int main(int argc, char** argv) {
  int mixed = argc == 3 && strcmp(argv[1], "--mixed") == 0;
  if(argc != 2 && !mixed) {
    fprintf(stderr, "usage: jit_numeric_run [--mixed] <path.jlo>\n");
    return 1;
  }

  const char* in_path = argv[argc - 1];
  FILE* f = fopen(in_path, "rb");
  if(!f) {
    fprintf(stderr, "%s: cannot open\n", in_path);
    return 1;
  }
  if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
  long size = ftell(f);
  if(size < 0) { fclose(f); return 1; }
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
  if(!data) { fclose(f); return 1; }
  size_t n = fread(data, 1, (size_t)size, f);
  fclose(f);

  jello_bc_module* m = NULL;
  jello_bc_result rr = jello_bc_read(data, n, &m);
  free(data);
  if(rr.err) {
    fprintf(stderr, "%s: %s (offset %zu)\n", in_path, rr.msg, rr.offset);
    return 1;
  }

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  if(!jello_jit_module_prepare(m, &jm) || (!mixed && jm.ncompiled != jm.nfuncs)) {
    jello_jit_module_free(&jm);
    jello_bc_free(m);
    printf("JIT_SKIP\n");
    return 0;
  }

  jello_vm* vm = jello_vm_create();
  if(!vm) {
    jello_jit_module_free(&jm);
    jello_bc_free(m);
    fprintf(stderr, "%s: out of memory creating VM\n", in_path);
    return 1;
  }

  jello_numeric_result result;
  memset(&result, 0, sizeof(result));
  jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, &jm, m, &result);

  if(st == JELLO_EXEC_OK) {
    print_numeric_result(&result);
  } else {
    printf("TRAP code=%s\n", trap_code_name(jello_vm_last_trap_code(vm)));
  }

  jello_jit_module_free(&jm);
  jello_vm_destroy(vm);
  jello_bc_free(m);
  return 0;
}
