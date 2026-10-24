// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * op_dispatch: runs the handler of an opcode the main loop does not run inline, through a table
 * generated from JELLO_OP_LIST; computed goto with JELLO_USE_COMPUTED_GOTO on GCC or Clang,
 * otherwise a switch. JIT code also calls it to run an opcode it does not compile. */

#include <jello/internal.h>
#include <stdio.h>
#include <stdlib.h>

/** Aborts. Reached only for an opcode the main loop runs inline, which is a dispatch bug, or one
 * with no interpreter body, which validation rejects. */
static op_result op_unimplemented(exec_ctx* ctx, const jello_insn* ins) {
  (void)ctx;
  fprintf(stderr,
          "jello_vm: opcode %u has no interpreter handler yet "
          "(either exec_loop.c's own hot switch already claims it -- reaching "
          "here means a dispatch bug -- or it is unimplemented; "
          "see jello_op_table[op].flags & JELLO_OPF_UNIMPLEMENTED)\n",
          (unsigned)ins->op);
  abort();
}

op_result op_dispatch(exec_ctx* ctx, const jello_insn* ins) {
#if defined(JELLO_USE_COMPUTED_GOTO) && (defined(__GNUC__) || defined(__clang__))
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wgnu-label-as-value"
#else
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define OP(id, name, ar, ak, br, bk, cr, ck, ir, fl, h) [id] = &&L_##id,
  static const void* const op_table[JOP_COUNT] = {JELLO_OP_LIST(OP)};
#undef OP

  goto* op_table[ins->op];

#define OP(id, name, ar, ak, br, bk, cr, ck, ir, fl, h) L_##id : return h(ctx, ins);
  JELLO_OP_LIST(OP)
#undef OP
#pragma GCC diagnostic pop
#else
#define OP(id, name, ar, ak, br, bk, cr, ck, ir, fl, h)                                            \
  case id: return h(ctx, ins);
  switch((jello_op)ins->op) {
    JELLO_OP_LIST(OP)
    default: return op_unimplemented(ctx, ins);
  }
#undef OP
#endif
}
