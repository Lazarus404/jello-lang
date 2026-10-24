// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Running a module as a program, for jello and jelloc. */

#include <jello/internal.h>
#include "host.h"

#include <stdio.h>

/** Reports on stderr, for each of `m`'s functions, whether `jm` compiled it, declined it (and
 * why) or never tried. */
static void report_functions(const jello_bc_module* m, const jello_jit_module* jm) {
  for(uint32_t fi = 0; fi < m->nfuncs; fi++) {
    const jello_jit_func_code* fc = jm ? &jm->funcs[fi] : NULL;
    if(fc && fc->compiled) {
      fprintf(stderr, "jit: function %u compiled\n", fi);
    } else if(fc && !fc->tried) {
      fprintf(stderr, "jit: function %u not compiled\n", fi);
    } else if(!fc || fc->decline_reason == JIT_DECLINE_NONE) {
      fprintf(stderr, "jit: function %u declined\n", fi);
    } else if(fc->decline_pc == UINT32_MAX) {
      fprintf(stderr, "jit: function %u declined %s\n", fi,
              jello_jit_decline_name((jello_jit_decline)fc->decline_reason));
    } else {
      fprintf(stderr, "jit: function %u declined %s op=%s pc=%u\n", fi,
              jello_jit_decline_name((jello_jit_decline)fc->decline_reason),
              fc->decline_op < JOP_COUNT ? jello_op_table[fc->decline_op].name : "?",
              fc->decline_pc);
    }
  }
  fprintf(stderr, "jit: %u of %u functions compiled\n", jm ? jm->ncompiled : 0u, m->nfuncs);
}

int jello_host_run(const jello_host_options* options, const char* name, const uint8_t* data,
                   size_t size, int nargs, const char* const* args) {
  const char* tool = options->tool;
  if((options->require_jit || options->jit_report) && !jello_jit_available()) {
    fprintf(stderr, "%s: built without a JIT\n", tool);
    return 1;
  }
  jello_bc_module* m = NULL;
  jello_bc_result loaded = jello_bc_read(data, size, &m);
  if(loaded.err) {
    fprintf(stderr, "%s: %s: %s (offset %zu)\n", tool, name, loaded.msg, loaded.offset);
    return 1;
  }
  jello_vm* vm = jello_vm_create();
  if(!vm) {
    fprintf(stderr, "%s: out of memory\n", tool);
    jello_bc_free(m);
    return 1;
  }
  jello_vm_set_program_args(vm, nargs, args);
  jello_vm_set_image(vm, data, size);
  jello_jit_module* jm = options->jit ? jit_module_create(m, options->require_jit) : NULL;
  if(options->jit_report && options->require_jit) report_functions(m, jm);
  if(options->require_jit && !(jm && jm->ncompiled == jm->nfuncs)) {
    fprintf(stderr, "%s: %s: the JIT cannot compile this module\n", tool, name);
    jello_vm_destroy(vm);
    jello_bc_free(m);
    return 1;
  }
  jello_numeric_result result;
  jello_exec_status status = jello_vm_exec_numeric_with_jit(vm, jm, m, &result);
  if(options->jit_report && !options->require_jit) report_functions(m, jm);
  if(options->jit_report)
    fprintf(stderr, "jit: entered %llu times, bridged %llu times\n",
            (unsigned long long)vm->jit_enters, (unsigned long long)vm->jit_bridges);
  if(status == JELLO_EXEC_OK) {
    jello_print_host_result(m, &result, stdout, !options->raw_bytes);
  } else {
    /* With a line table, the trapping instruction's source position comes first, as in a compiler
     * diagnostic. */
    const char* file = NULL;
    uint32_t line = 0, col = 0;
    if(jello_bc_source_location(m, jello_vm_last_trap_func_index(vm), jello_vm_last_trap_pc(vm),
                                &file, &line, &col))
      fprintf(stderr, "%s:%u:%u: ", file, line, col);
    fprintf(stderr, "%s: trap %s in function %u at pc %u\n", tool,
            jello_trap_code_name(jello_vm_last_trap_code(vm)), jello_vm_last_trap_func_index(vm),
            jello_vm_last_trap_pc(vm));
    const char* detail = jello_vm_last_trap_detail(vm);
    if(*detail) fprintf(stderr, "%s: %s\n", tool, detail);
    jello_value thrown = jello_vm_last_thrown(vm);
    if(jello_vm_last_trap_code(vm) == JELLO_TRAP_UNCAUGHT_THROW && !jello_is_null(thrown)) {
      jello_numeric_result shown = {.kind = JELLO_NUM_DYN, .as.dyn = thrown};
      fprintf(stderr, "%s: thrown: ", tool);
      jello_print_host_result(m, &shown, stderr, 1);
    }
    // Then the calls that led there, innermost first.
    uint32_t callers[JELLO_TRAP_CALLERS_MAX], pcs[JELLO_TRAP_CALLERS_MAX];
    uint8_t repeated[JELLO_TRAP_CALLERS_MAX];
    uint32_t ncallers =
        jello_vm_last_trap_callers(vm, callers, pcs, repeated, JELLO_TRAP_CALLERS_MAX);
    for(uint32_t i = 0; i < ncallers; i++) {
      if(jello_bc_source_location(m, callers[i], pcs[i], &file, &line, &col))
        fprintf(stderr, "  called from %s:%u:%u", file, line, col);
      else fprintf(stderr, "  called from function %u at pc %u", callers[i], pcs[i]);
      fprintf(stderr, "%s\n", repeated[i] ? ", repeatedly" : "");
    }
  }
  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  jello_bc_free(m);
  return status == JELLO_EXEC_OK ? 0 : 1;
}
