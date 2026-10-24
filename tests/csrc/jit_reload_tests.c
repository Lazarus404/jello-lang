// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* fib(n) = n<2 ? n : fib(n-1)+fib(n-2) -- the same shape the survive fixtures
 * in jit_calls_tests.c use, reused here because it is call-heavy enough to
 * make "did this actually skip recompiling" an observable difference, not
 * just a trivial one-instruction function that would compile equally fast
 * either way. */
static void build_fib_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 1; r1[2] = 0; r1[3] = 0; r1[4] = 0; r1[5] = 0; r1[6] = 0;

  memset(e0, 0, 3 * sizeof(jello_insn));
  memset(e1, 0, 9 * sizeof(jello_insn));

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = n;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_LT_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 2;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1;
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5;
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_RET; e1[8].a = 0;

  funcs[0].name_atom = JELLO_BC_NO_NAME;
  funcs[0].nargs = 0; funcs[0].nregs = 2; funcs[0].cap_start = 2; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].name_atom = JELLO_BC_NO_NAME;
  funcs[1].nargs = 1; funcs[1].nregs = 7; funcs[1].cap_start = 7; funcs[1].reg_types = r1; funcs[1].ninsns = 9; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2; m->types = types; m->nfuncs = 2; m->funcs = funcs; m->entry = 0;
}

static void test_module_create_destroy_roundtrip(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 10);

  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);
  jello_jit_module_destroy(jm);
  jello_jit_module_destroy(NULL); /* must be a safe no-op */
  printf("test_module_create_destroy_roundtrip: PASS\n");
}

/* The signature/body-hash analysis is standalone and compile-free --
 * unit-testable against hand-built old/new function pairs directly, no
 * jello_jit_module or W^X state involved at all. */
static void build_one_func_module(jello_type_entry* types, jello_type_id* r,
    jello_insn* insns, uint32_t ninsns, uint32_t nregs, uint32_t nargs,
    jello_bc_function* f, jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  f->name_atom = JELLO_BC_NO_NAME;
  f->nargs = nargs;
  f->nregs = nregs;
  f->cap_start = nregs;
  f->reg_types = r;
  f->ninsns = ninsns;
  f->insns = insns;
  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 1; m->funcs = f; m->entry = 0;
}

static void test_reload_analyze_unchanged(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[2] = {0, 0}, r_b[2] = {0, 0};
  jello_insn insns_a[2], insns_b[2];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = insns_b[0].op = (uint8_t)JOP_ADD_I32_IMM;
  insns_a[0].a = insns_b[0].a = 1; insns_a[0].b = insns_b[0].b = 0;
  insns_a[0].imm = insns_b[0].imm = 5;
  insns_a[1].op = insns_b[1].op = (uint8_t)JOP_RET;
  insns_a[1].a = insns_b[1].a = 1;

  jello_bc_function fa = {0}, fb = {0};
  jello_bc_module ma = {0}, mb = {0};
  build_one_func_module(types_a, r_a, insns_a, 2, 2, 1, &fa, &ma);
  build_one_func_module(types_b, r_b, insns_b, 2, 2, 1, &fb, &mb);

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_UNCHANGED);
  printf("test_reload_analyze_unchanged: PASS\n");
}

static void test_reload_analyze_changed(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[2] = {0, 0}, r_b[2] = {0, 0};
  jello_insn insns_a[2], insns_b[2];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = insns_b[0].op = (uint8_t)JOP_ADD_I32_IMM;
  insns_a[0].a = insns_b[0].a = 1; insns_a[0].b = insns_b[0].b = 0;
  insns_a[0].imm = 5;
  insns_b[0].imm = 6; /* the only difference -- same signature, different body */
  insns_a[1].op = insns_b[1].op = (uint8_t)JOP_RET;
  insns_a[1].a = insns_b[1].a = 1;

  jello_bc_function fa = {0}, fb = {0};
  jello_bc_module ma = {0}, mb = {0};
  build_one_func_module(types_a, r_a, insns_a, 2, 2, 1, &fa, &ma);
  build_one_func_module(types_b, r_b, insns_b, 2, 2, 1, &fb, &mb);

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_CHANGED);
  printf("test_reload_analyze_changed: PASS\n");
}

static void test_reload_analyze_signature_conflict_arg_count(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[2] = {0, 0}, r_b[3] = {0, 0, 0};
  jello_insn insns_a[2], insns_b[2];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = (uint8_t)JOP_ADD_I32_IMM; insns_a[0].a = 1; insns_a[0].b = 0; insns_a[0].imm = 5;
  insns_a[1].op = (uint8_t)JOP_RET; insns_a[1].a = 1;
  insns_b[0].op = (uint8_t)JOP_ADD_I32; insns_b[0].a = 2; insns_b[0].b = 0; insns_b[0].c = 1;
  insns_b[1].op = (uint8_t)JOP_RET; insns_b[1].a = 2;

  jello_bc_function fa = {0}, fb = {0};
  jello_bc_module ma = {0}, mb = {0};
  build_one_func_module(types_a, r_a, insns_a, 2, 2, 1, &fa, &ma);
  build_one_func_module(types_b, r_b, insns_b, 2, 3, 2, &fb, &mb); /* nargs 1 -> 2 */

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);
  printf("test_reload_analyze_signature_conflict_arg_count: PASS\n");
}

static void test_reload_analyze_signature_conflict_ret_kind(void) {
  jello_type_entry types_a[2], types_b[2];
  jello_type_id r_a[1] = {0}, r_b[1] = {1};
  jello_insn insns_a[1], insns_b[1];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = insns_b[0].op = (uint8_t)JOP_RET;
  insns_a[0].a = insns_b[0].a = 0;

  types_a[0].kind = JELLO_T_I32; memset(&types_a[0].as, 0, sizeof(types_a[0].as));
  types_a[1].kind = JELLO_T_BOOL; memset(&types_a[1].as, 0, sizeof(types_a[1].as));
  types_b[0].kind = JELLO_T_I32; memset(&types_b[0].as, 0, sizeof(types_b[0].as));
  types_b[1].kind = JELLO_T_BOOL; memset(&types_b[1].as, 0, sizeof(types_b[1].as));

  jello_bc_function fa = {0}, fb = {0};
  jello_bc_module ma = {0}, mb = {0};
  fa.name_atom = fb.name_atom = JELLO_BC_NO_NAME;
  fa.nargs = fb.nargs = 0;
  fa.nregs = fb.nregs = 1;
  fa.cap_start = fb.cap_start = 1;
  fa.reg_types = r_a; fb.reg_types = r_b; /* I32 vs BOOL return */
  fa.ninsns = fb.ninsns = 1;
  fa.insns = insns_a; fb.insns = insns_b;
  memset(&ma, 0, sizeof(ma)); ma.ntypes = 2; ma.types = types_a; ma.nfuncs = 1; ma.funcs = &fa;
  memset(&mb, 0, sizeof(mb)); mb.ntypes = 2; mb.types = types_b; mb.nfuncs = 1; mb.funcs = &fb;

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);
  printf("test_reload_analyze_signature_conflict_ret_kind: PASS\n");
}

static void test_module_create_rejects_empty_module(void) {
  jello_bc_module m = {0};
  m.nfuncs = 0;
  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm == NULL);
  printf("test_module_create_rejects_empty_module: PASS\n");
}

/* jello_jit_module_reload end to end. Two named functions ("main" calls
 * "helper"); variant B changes only helper's own constant. Not
 * architecture-gated: the tests using it compile code but never execute it. */
static void build_named_two_func_module(jello_type_entry* types, jello_type_id* r0,
    jello_type_id* r1, jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
    jello_bc_module* m, uint32_t main_name_atom, uint32_t helper_name_atom, int32_t helper_const) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  r0[0] = 0; r1[0] = 0;
  memset(e0, 0, 2 * sizeof(jello_insn));
  memset(e1, 0, 2 * sizeof(jello_insn));

  e0[0].op = (uint8_t)JOP_CALL; e0[0].a = 0; e0[0].b = 0; e0[0].c = 0; e0[0].imm = 1;
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 0;
  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = (uint32_t)helper_const;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 0;

  funcs[0].name_atom = main_name_atom;
  funcs[0].nargs = 0; funcs[0].nregs = 1; funcs[0].cap_start = 1; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].name_atom = helper_name_atom;
  funcs[1].nargs = 0; funcs[1].nregs = 1; funcs[1].cap_start = 1; funcs[1].reg_types = r1; funcs[1].ninsns = 2; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 2; m->funcs = funcs; m->entry = 0;
}

static const char* g_two_func_atoms[2] = {"main", "helper"};

/* An unchanged module that is a different object is still recompiled, since the old code
 * points into old_m; reloading jm's own module again is a no-op. */
static void test_reload_unchanged_succeeds(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[1], r1_b[1];
  jello_insn e0_a[2], e1_a[2], e0_b[2], e1_b[2];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0};
  jello_bc_module old_m = {0}, new_m = {0};
  build_named_two_func_module(types_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &old_m, 0, 1, 100);
  old_m.atoms = g_two_func_atoms; old_m.natoms = 2; /* build_* memsets m -- set atoms after */
  build_named_two_func_module(types_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &new_m, 0, 1, 100);
  new_m.atoms = g_two_func_atoms; new_m.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_jit_module_prepare_count = 0;
  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, 0, &report);
  assert(rc == 1);
  assert(report.n_unchanged == 2);
  assert(report.n_changed == 0);
  assert(report.n_new == 0);
  assert(report.n_removed == 0);
  assert(report.n_signature_conflicts == 0);
  assert(report.n_type_conflicts == 0);
  assert(jello_jit_module_prepare_count == 1);
  assert(jm->m == &new_m);
  jello_jit_reload_report_free(&report);

  rc = jello_jit_module_reload(vm, jm, &new_m, &new_m, 0, &report);
  assert(rc == 1);
  assert(report.n_unchanged == 2);
  assert(jello_jit_module_prepare_count == 1);
  jello_jit_reload_report_free(&report);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_unchanged_succeeds: PASS\n");
}

/* The struct type-conflict cascade, end to end through
 * jello_jit_module_reload itself (not just the standalone
 * jello_reload_struct_sig_changed/jello_reload_function_uses_struct_sig unit
 * calls) -- deliberately not architecture-gated: the
 * refuse-by-default path returns before calling
 * jello_jit_module_prepare, so nothing here executes JIT'd code. A single named function "main" declares one register of struct type;
 * the struct's own field count changes shape between old_m and new_m (1 field
 * -> 2), while main's own instructions are untouched -- proving the cascade
 * is what forces the conflict, not a body-hash difference that would have
 * been caught anyway. */
static void build_struct_using_module(jello_type_entry* types, jello_struct_sig* sig,
    uint32_t* field_type, uint32_t* field_offset, uint32_t nfields, jello_type_id* r,
    jello_insn* insns, jello_bc_function* f, jello_bc_module* m, uint32_t name_atom) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  types[1].kind = JELLO_T_STRUCT;
  types[1].as.strct.struct_sig_id = 0;

  for(uint32_t i = 0; i < nfields; i++) { field_type[i] = 0; field_offset[i] = i * 4; }
  sig->nfields = (uint16_t)nfields;
  sig->named = 0;
  sig->is_mutable = 0;
  sig->field_type = field_type;
  sig->field_name_atom = NULL;
  sig->field_offset = field_offset;

  r[0] = 1; /* STRUCT-typed register, never assigned -- static analysis only, never executed */
  r[1] = 0; /* I32 -- the one actually returned */
  memset(insns, 0, 1 * sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_RET; insns[0].a = 1;

  f->name_atom = name_atom;
  f->nargs = 0; f->nregs = 2; f->cap_start = 2; f->reg_types = r; f->ninsns = 1; f->insns = insns;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2; m->types = types; m->nstruct_sigs = 1; m->struct_sigs = sig;
  m->nfuncs = 1; m->funcs = f; m->entry = 0;
}

static const char* g_one_func_atom[1] = {"main"};

static void test_reload_refuses_struct_type_conflict_by_default(void) {
  jello_type_entry types_a[2], types_b[2];
  jello_struct_sig sig_a, sig_b;
  uint32_t field_type_a[1], field_offset_a[1];
  uint32_t field_type_b[2], field_offset_b[2];
  jello_type_id r_a[2], r_b[2];
  jello_insn insns_a[1], insns_b[1];
  jello_bc_function f_a = {0}, f_b = {0};
  jello_bc_module old_m = {0}, new_m = {0};

  build_struct_using_module(types_a, &sig_a, field_type_a, field_offset_a, 1, r_a, insns_a, &f_a, &old_m, 0);
  old_m.atoms = g_one_func_atom; old_m.natoms = 1;
  build_struct_using_module(types_b, &sig_b, field_type_b, field_offset_b, 2, r_b, insns_b, &f_b, &new_m, 0);
  new_m.atoms = g_one_func_atom; new_m.natoms = 1;

  /* The cascade's own two preconditions, confirmed directly rather than
   * only implied by the end-to-end report below. Note the plain
   * signature/body analysis alone already calls this pair CHANGED here
   * (register 0's own type hash folds in the struct's shape, and that
   * shape moved) -- the cascade's real job, proven by the report below
   * rather than this call, is reclassifying it as a *type* conflict
   * specifically (n_type_conflicts, not n_changed) so an embedder can
   * tell "an ordinary edit" apart from "a struct shape moved under you." */
  assert(jello_reload_struct_sig_changed(&old_m, &new_m, 0) == 1);
  assert(jello_reload_function_uses_struct_sig(&old_m, &f_a, 0) == 1);
  assert(jello_reload_analyze_function(&old_m, &f_a, &new_m, &f_b) == JELLO_RELOAD_FN_CHANGED);

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, /* allow_conflicts */ 0, &report);
  assert(rc == 0); /* refused -- the struct shape changed under a live function */
  assert(report.n_type_conflicts == 1);
  assert(report.n_signature_conflicts == 0);
  assert(report.fn_status[0] == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);
  jello_jit_reload_report_free(&report);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_refuses_struct_type_conflict_by_default: PASS\n");
}

/* A signature-conflicting function whose *body bytes* did not change (only
 * its declared nargs did) gives n_changed == 0, yet allow_conflicts asked for
 * the change to be applied, so it must still be compiled.
 * Host-independent, like the fixtures above: jello_jit_module_prepare emits
 * real x64 bytes on any host (only *executing* them needs one), so
 * jello_jit_module_prepare_count (module.c, same reset-and-assert idiom as
 * test_persistent_handle_no_per_call_recompile) proves a real recompile
 * happened without needing Rosetta. */
static void build_one_named_func_module(jello_type_entry* types, jello_type_id* r,
    jello_insn* insns, jello_bc_function* f, jello_bc_module* m, uint32_t nargs, uint32_t name_atom) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  r[0] = 0;
  memset(insns, 0, 1 * sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_RET; insns[0].a = 0;

  f->name_atom = name_atom;
  f->nargs = nargs; f->nregs = 1; f->cap_start = 1; f->reg_types = r; f->ninsns = 1; f->insns = insns;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 1; m->funcs = f; m->entry = 0;
}

static void test_reload_allow_conflicts_recompiles_even_if_body_unchanged(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[1], r_b[1];
  jello_insn insns_a[1], insns_b[1];
  jello_bc_function f_a = {0}, f_b = {0};
  jello_bc_module old_m = {0}, new_m = {0};
  build_one_named_func_module(types_a, r_a, insns_a, &f_a, &old_m, 0, 0); /* nargs 0 */
  old_m.atoms = g_one_func_atom; old_m.natoms = 1;
  build_one_named_func_module(types_b, r_b, insns_b, &f_b, &new_m, 1, 0); /* nargs 1 -- only difference */
  new_m.atoms = g_one_func_atom; new_m.natoms = 1;

  /* Confirms the premise directly: same body bytes, signature conflict. */
  assert(jello_reload_analyze_function(&old_m, &f_a, &new_m, &f_b) == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_jit_module_prepare_count = 0;
  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, /* allow_conflicts */ 1, &report);
  assert(rc == 1);
  assert(report.n_changed == 0);
  assert(report.n_signature_conflicts == 1);
  /* jello_jit_module_prepare ran for new_m. */
  assert(jello_jit_module_prepare_count == 1);
  jello_jit_reload_report_free(&report);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_allow_conflicts_recompiles_even_if_body_unchanged: PASS\n");
}

/* A brand-new function (n_new > 0) with every existing function unchanged is
 * compiled too: jm's funcs[] is sized to the old module, so without a
 * recompile the new function would only ever run in the interpreter. */
static void test_reload_new_function_recompiles_even_if_nothing_changed(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[1];
  jello_insn insns_a[1];
  jello_bc_function f_a = {0};
  jello_bc_module old_m = {0};
  build_one_named_func_module(types_a, r_a, insns_a, &f_a, &old_m, 0, 0);
  old_m.atoms = g_one_func_atom; old_m.natoms = 1;

  jello_type_id r0_b[1], r1_b[1];
  jello_insn e0_b[1], e1_b[1];
  jello_bc_function funcs_b[2] = {0};
  jello_bc_module new_m = {0};
  types_b[0].kind = JELLO_T_I32;
  memset(&types_b[0].as, 0, sizeof(types_b[0].as));
  r0_b[0] = 0; r1_b[0] = 0;
  memset(e0_b, 0, sizeof(e0_b)); e0_b[0].op = (uint8_t)JOP_RET; e0_b[0].a = 0; /* byte-identical to f_a */
  memset(e1_b, 0, sizeof(e1_b)); e1_b[0].op = (uint8_t)JOP_RET; e1_b[0].a = 0;
  funcs_b[0].name_atom = 0; funcs_b[0].nargs = 0; funcs_b[0].nregs = 1; funcs_b[0].cap_start = 1;
  funcs_b[0].reg_types = r0_b; funcs_b[0].ninsns = 1; funcs_b[0].insns = e0_b;
  funcs_b[1].name_atom = JELLO_BC_NO_NAME; funcs_b[1].nargs = 0; funcs_b[1].nregs = 1; funcs_b[1].cap_start = 1;
  funcs_b[1].reg_types = r1_b; funcs_b[1].ninsns = 1; funcs_b[1].insns = e1_b;
  memset(&new_m, 0, sizeof(new_m));
  new_m.ntypes = 1; new_m.types = types_b; new_m.nfuncs = 2; new_m.funcs = funcs_b; new_m.entry = 0;
  new_m.atoms = g_one_func_atom; new_m.natoms = 1;

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  assert(jm->nfuncs == 1);
  jello_vm* vm = jello_vm_create();

  jello_jit_module_prepare_count = 0;
  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, 0, &report);
  assert(rc == 1);
  assert(report.n_unchanged == 1);
  assert(report.n_new == 1);
  assert(report.n_changed == 0);
  assert(report.n_signature_conflicts == 0);
  assert(report.n_type_conflicts == 0);
  /* The actual proof: a real recompile ran, so jm now actually covers the
   * new function too, rather than silently staying sized to old_m. */
  assert(jello_jit_module_prepare_count == 1);
  assert(jm->nfuncs == 2);
  jello_jit_reload_report_free(&report);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_new_function_recompiles_even_if_nothing_changed: PASS\n");
}

#if defined(__x86_64__) || defined(__aarch64__)

/* A signature-conflicting "helper" (nargs 0 -> 1) must refuse the *entire*
 * reload by default, leaving jm still fully, correctly running old_m -- not
 * silently swap in new_m's own code the way an unconditional swap would.
 * "main" is rewritten consistently with new_m's own new "helper" (passes it
 * one argument); if this reload were wrongly allowed to apply anyway, new_m
 * would still run correctly on its own terms, which is exactly why the
 * refusal has to come from reload's own conflict check, not from new_m being
 * broken on its own. Architecture-gated like the other fixtures in this
 * block: unlike test_reload_unchanged_succeeds, which executes nothing,
 * this one actually executes JIT'd x64 code via
 * jello_vm_exec_numeric_with_jit both before and after the refused/allowed
 * reload. */
static void build_two_func_module_arity_variant(jello_type_entry* types, jello_type_id* r0,
    jello_type_id* r1, jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
    jello_bc_module* m, uint32_t main_name_atom, uint32_t helper_name_atom, int32_t arg_const) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  r0[0] = 0; r0[1] = 0;
  r1[0] = 0;
  memset(e0, 0, 3 * sizeof(jello_insn));
  memset(e1, 0, 1 * sizeof(jello_insn));

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = (uint32_t)arg_const;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1; /* helper(arg) */
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;
  e1[0].op = (uint8_t)JOP_RET; e1[0].a = 0; /* helper(x) = x */

  funcs[0].name_atom = main_name_atom;
  funcs[0].nargs = 0; funcs[0].nregs = 2; funcs[0].cap_start = 2; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].name_atom = helper_name_atom;
  funcs[1].nargs = 1; funcs[1].nregs = 1; funcs[1].cap_start = 1; funcs[1].reg_types = r1; funcs[1].ninsns = 1; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 2; m->funcs = funcs; m->entry = 0;
}

static void test_reload_refuses_signature_conflict_by_default(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[2], r1_b[1];
  jello_insn e0_a[2], e1_a[2], e0_b[3], e1_b[1];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0};
  jello_bc_module old_m = {0}, new_m = {0};
  build_named_two_func_module(types_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &old_m, 0, 1, 100);
  old_m.atoms = g_two_func_atoms; old_m.natoms = 2;
  build_two_func_module_arity_variant(types_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &new_m, 0, 1, 777);
  new_m.atoms = g_two_func_atoms; new_m.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &old_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 100);

  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, /* allow_conflicts */ 0, &report);
  assert(rc == 0); /* refused -- must not silently apply a conflicting reload */
  assert(report.n_signature_conflicts == 1);
  assert(report.n_type_conflicts == 0);
  assert(report.fn_status[1] == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);
  jello_jit_reload_report_free(&report);

  /* jm must still be exactly as it was: old_m's own code, unpatched. */
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &old_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 100);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_refuses_signature_conflict_by_default: PASS\n");
}

static void test_reload_allow_conflicts_overrides_refusal(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[2], r1_b[1];
  jello_insn e0_a[2], e1_a[2], e0_b[3], e1_b[1];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0};
  jello_bc_module old_m = {0}, new_m = {0};
  build_named_two_func_module(types_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &old_m, 0, 1, 100);
  old_m.atoms = g_two_func_atoms; old_m.natoms = 2;
  build_two_func_module_arity_variant(types_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &new_m, 0, 1, 777);
  new_m.atoms = g_two_func_atoms; new_m.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, /* allow_conflicts */ 1, &report);
  assert(rc == 1); /* explicit override proceeds despite the conflict */
  assert(report.n_signature_conflicts == 1);
  jello_jit_reload_report_free(&report);

  /* new_m is internally self-consistent (main's own new call site matches
   * helper's own new arity) -- jm now runs it correctly. */
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &new_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 777);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_allow_conflicts_overrides_refusal: PASS\n");
}

static void build_const_func(jello_type_id* r, jello_insn* insns, jello_bc_function* f,
    uint32_t name_atom, int32_t value) {
  r[0] = 0;
  memset(insns, 0, 2 * sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = (uint32_t)value;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;
  memset(f, 0, sizeof(*f));
  f->name_atom = name_atom;
  f->nregs = 1; f->cap_start = 1; f->reg_types = r; f->ninsns = 2; f->insns = insns;
}

static const char* g_removal_atoms[2] = {"a", "b"};

/* Removing "a" moves the unchanged "b" from index 1 to 0, so code kept from the old module
 * would run "a" for new_m's entry. Every function is unchanged or removed, so the per-function
 * analysis alone finds nothing to compile. */
static void test_reload_removal_runs_shifted_function(void) {
  jello_type_entry types[1];
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  jello_type_id ra[1], rb_old[1], rb_new[1];
  jello_insn ea[2], eb_old[2], eb_new[2];
  jello_bc_function old_funcs[2] = {0}, new_funcs[1] = {0};
  build_const_func(ra, ea, &old_funcs[0], 0, 111);
  build_const_func(rb_old, eb_old, &old_funcs[1], 1, 222);
  build_const_func(rb_new, eb_new, &new_funcs[0], 1, 222);

  jello_bc_module old_m = {0}, new_m = {0};
  old_m.ntypes = 1; old_m.types = types; old_m.nfuncs = 2; old_m.funcs = old_funcs;
  old_m.atoms = g_removal_atoms; old_m.natoms = 2;
  new_m.ntypes = 1; new_m.types = types; new_m.nfuncs = 1; new_m.funcs = new_funcs;
  new_m.atoms = g_removal_atoms; new_m.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, 0, &report);
  assert(rc == 1);
  assert(report.n_unchanged == 1 && report.n_removed == 1);
  assert(report.n_changed == 0 && report.n_new == 0);
  jello_jit_reload_report_free(&report);

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &new_m, &out) == JELLO_EXEC_OK);
  if(out.kind != JELLO_NUM_I32 || out.as.i32 != 222)
    fprintf(stderr, "test_reload_removal_runs_shifted_function: got %d\n", out.as.i32);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 222);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_removal_runs_shifted_function: PASS\n");
}

#endif

#if defined(__x86_64__)
static void test_reload_changed_function_end_to_end(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[1], r1_b[1];
  jello_insn e0_a[2], e1_a[2], e0_b[2], e1_b[2];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0};
  jello_bc_module old_m = {0}, new_m = {0};
  build_named_two_func_module(types_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &old_m, 0, 1, 100);
  old_m.atoms = g_two_func_atoms; old_m.natoms = 2;
  build_named_two_func_module(types_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &new_m, 0, 1, 200);
  new_m.atoms = g_two_func_atoms; new_m.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&old_m);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &old_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 100);

  jello_jit_reload_report report;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, 0, &report);
  assert(rc == 1);
  assert(report.n_unchanged == 1); /* "main" itself did not change */
  assert(report.n_changed == 1);   /* "helper" did */
  assert(report.n_new == 0);
  assert(report.n_removed == 0);
  assert(report.n_signature_conflicts == 0);
  assert(report.n_type_conflicts == 0);
  assert(report.fn_status[1] == JELLO_RELOAD_FN_CHANGED);
  jello_jit_reload_report_free(&report);

  /* The new behaviour must be visible immediately through the *same*
   * persistent handle -- no re-create, no re-prepare by the caller. */
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &new_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 200);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_changed_function_end_to_end: PASS\n");
}

static int32_t fib_ref_reload(int32_t n) {
  return (n < 2) ? n : fib_ref_reload(n - 1) + fib_ref_reload(n - 2);
}

/* Reload interacting with survive. Reuses the `fib`-shaped, survive-armed
 * recursive function (jit_calls_tests.c's `build_fib_survive_module` /
 * `test_call_self_survive_fib_correct`, same instruction shape reproduced
 * here since that builder is `static` in a different translation unit) inside
 * a three-function module: "main" calls "fib"; "unrelated" is a third
 * function that changes body between old_m and new_m while "fib" and "main"
 * stay byte-identical. This proves empirically that nothing survive-specific
 * is needed for reload: `jello_jit_module_prepare` unconditionally recompiles
 * *every* function in new_m regardless of its own reload status -- "fib"
 * being reported UNCHANGED does not mean its machine code is untouched, it
 * means a *fresh* compile happened to produce byte-identical semantics. If
 * anything about survive's own reentrancy discipline (the function-scoped
 * push/pop r15) depended on surviving *across* a reload rather than being
 * re-established by each fresh compile, fib(15) -- which recurses to depth 15
 * with many R15 holds live on the native call stack at once -- would be the
 * shape most likely to expose it. */
static void build_fib_survive_reload_module(jello_type_entry* types, jello_type_id* r0,
    jello_type_id* r1, jello_type_id* r2, jello_insn* e0, jello_insn* e1, jello_insn* e2,
    jello_bc_function* funcs, jello_bc_module* m, int32_t unrelated_const) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 1; r1[2] = 0; r1[3] = 0; r1[4] = 0; r1[5] = 0; r1[6] = 0;
  r2[0] = 0;

  /* Zeroed first, unlike jit_calls_tests.c's own build_fib_survive_module --
   * that builder only ever builds *one* copy of fib per test, so an
   * uninitialized field an op never sets (e.g. LT_I32_IMM's own unused `.c`)
   * never has anything to differ *from*. This fixture builds fib twice, into
   * two separate stack arrays, and needs their raw insns bytes to compare
   * bit-for-bit equal (reload's own body-hash comparison hashes the whole
   * jello_insn, not just the fields a given op reads) -- without this, two
   * semantically-identical bodies would hash unequal on whatever garbage
   * happened to be sitting in each array's own unused bytes, and this fixture
   * would spuriously report CHANGED for fib/main instead of proving what it
   * claims to. */
  memset(e0, 0, 3 * sizeof(jello_insn));
  memset(e1, 0, 9 * sizeof(jello_insn));
  memset(e2, 0, 2 * sizeof(jello_insn));

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 15;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1; /* -> fib */
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  /* Byte-identical to jit_calls_tests.c's own build_fib_survive_module,
   * across both old_m and new_m -- this function's own reload status must
   * come out UNCHANGED for the fixture to prove what it claims. */
  e1[0].op = (uint8_t)JOP_LT_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 2;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1; /* survive candidate */
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1;
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5; /* consumer */
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_RET; e1[8].a = 0;

  /* The one function this fixture actually varies between old_m/new_m. */
  e2[0].op = (uint8_t)JOP_CONST_I32; e2[0].a = 0; e2[0].imm = (uint32_t)unrelated_const;
  e2[1].op = (uint8_t)JOP_RET; e2[1].a = 0;

  funcs[0].name_atom = 0; /* "main" */
  funcs[0].nargs = 0; funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].name_atom = 1; /* "fib" */
  funcs[1].nargs = 0; funcs[1].nregs = 7; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 9; funcs[1].insns = e1;
  funcs[2].name_atom = 2; /* "unrelated" */
  funcs[2].nargs = 0; funcs[2].nregs = 1; funcs[2].cap_start = 1; funcs[2].reg_types = r2; funcs[2].ninsns = 2; funcs[2].insns = e2;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2; m->types = types; m->nfuncs = 3; m->funcs = funcs; m->entry = 0;
}

static const char* g_fib_survive_reload_atoms[3] = {"main", "fib", "unrelated"};

static void test_reload_survive_interaction(void) {
  jello_type_entry types_a[2], types_b[2];
  jello_type_id r0_a[2], r1_a[7], r2_a[1], r0_b[2], r1_b[7], r2_b[1];
  jello_insn e0_a[3], e1_a[9], e2_a[2], e0_b[3], e1_b[9], e2_b[2];
  jello_bc_function funcs_a[3] = {0}, funcs_b[3] = {0};
  jello_bc_module old_m = {0}, new_m = {0};
  build_fib_survive_reload_module(types_a, r0_a, r1_a, r2_a, e0_a, e1_a, e2_a, funcs_a, &old_m, 111);
  old_m.atoms = g_fib_survive_reload_atoms; old_m.natoms = 3;
  build_fib_survive_reload_module(types_b, r0_b, r1_b, r2_b, e0_b, e1_b, e2_b, funcs_b, &new_m, 222);
  new_m.atoms = g_fib_survive_reload_atoms; new_m.natoms = 3;

  /* jit_call_survive_armed_count increments at compile time. The eager create below compiles
   * every function of old_m (a lazy one would compile only when called), so the reset must happen
   * before it, not before the exec call. */
  jit_call_survive_armed_count = 0;
  jello_jit_module* jm = jit_module_create(&old_m, 1);
  assert(jm != NULL);
  assert(jit_call_survive_armed_count > 0);
  jello_vm* vm = jello_vm_create();

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &old_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == fib_ref_reload(15));

  jello_jit_reload_report report;
  /* Reset immediately before the reload call, not before the exec call
   * after it -- jello_jit_module_reload is what actually recompiles new_m
   * (module.c's own jello_jit_module_prepare, called from within reload.c),
   * so this is where the fresh compile's own armed count must be observed. */
  jit_call_survive_armed_count = 0;
  int rc = jello_jit_module_reload(vm, jm, &old_m, &new_m, 0, &report);
  assert(rc == 1);
  assert(report.n_unchanged == 2); /* "main" and "fib" -- byte-identical */
  assert(report.n_changed == 1);   /* "unrelated" */
  assert(report.n_new == 0);
  assert(report.n_removed == 0);
  assert(report.n_signature_conflicts == 0);
  assert(report.n_type_conflicts == 0);
  assert(report.fn_status[1] == JELLO_RELOAD_FN_UNCHANGED); /* fib itself */
  jello_jit_reload_report_free(&report);
  /* Confirms the reload's own fresh compile re-ran survive's own pattern
   * match for fib, even though fib's own reload status came out UNCHANGED --
   * the whole module is recompiled wholesale, proven here rather than only
   * argued. */
  assert(jit_call_survive_armed_count > 0);
  uint32_t armed_after_reload = jit_call_survive_armed_count;

  /* The actual proof: fib(15), through the *freshly recompiled* jm, still
   * produces the correct answer -- reentrancy across many co-live R15
   * holds on the native call stack is re-established correctly by the new
   * compile, with nothing reload-specific required to make that true. */
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &new_m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == fib_ref_reload(15));

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_survive_interaction: PASS (fib(15)=%d, armed_after_reload=%u)\n",
      out.as.i32, armed_after_reload);
}

#endif

static int32_t fib_ref(int32_t n) {
  return (n < 2) ? n : fib_ref(n - 1) + fib_ref(n - 2);
}

/* The persistent-handle path must not recompile the module on every call the
 * way jello_vm_exec_numeric's own prepare-fresh-every-call path still does.
 * jello_jit_module_prepare_count is the same reset-and-assert idiom
 * jit_try_enter_count/jit_reg_discard_count already use elsewhere in this
 * test suite. */
static void test_persistent_handle_no_per_call_recompile(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 15);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);

  jello_jit_module_prepare_count = 0;
  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);
  assert(jello_jit_module_prepare_count == 1);

  int32_t expected = fib_ref(15);
  for(int i = 0; i < 20; i++) {
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32);
    assert(out.as.i32 == expected);
  }
  /* The whole point: 20 calls through the persistent handle, exactly one
   * jello_jit_module_prepare (the jello_jit_module_create above), not 20. */
  assert(jello_jit_module_prepare_count == 1);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_persistent_handle_no_per_call_recompile: PASS (prepare_count=%u)\n",
      jello_jit_module_prepare_count);
}

/* The persistent-handle path's results must match the prepare-fresh-every-call
 * path bit for bit, not merely "look plausible." */
#if defined(__x86_64__)
static void test_persistent_handle_matches_fresh_path(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};

  for(uint32_t n = 0; n <= 12; n++) {
    build_fib_module(types, r0, r1, e0, e1, funcs, &m, n);

    jello_vm* vm_fresh = jello_vm_create();
    jello_numeric_result out_fresh;
    memset(&out_fresh, 0, sizeof(out_fresh));
    jello_exec_status st_fresh = jello_vm_exec_numeric(vm_fresh, &m, &out_fresh);
    jello_vm_destroy(vm_fresh);

    jello_vm* vm_handle = jello_vm_create();
    jello_jit_module* jm = jello_jit_module_create(&m);
    assert(jm != NULL);
    jello_numeric_result out_handle;
    memset(&out_handle, 0, sizeof(out_handle));
    jello_exec_status st_handle = jello_vm_exec_numeric_with_jit(vm_handle, jm, &m, &out_handle);
    jello_jit_module_destroy(jm);
    jello_vm_destroy(vm_handle);

    assert(st_fresh == st_handle);
    assert(out_fresh.kind == out_handle.kind);
    assert(memcmp(&out_fresh.as, &out_handle.as, sizeof(out_fresh.as)) == 0);
  }
  printf("test_persistent_handle_matches_fresh_path: PASS\n");
}
#endif

/* The NULL-handle fallback contract: jello_vm_exec_numeric_with_jit must
 * still produce correct results with jm == NULL (falls back to the plain
 * interpreter, per jello.h's own doc comment), not crash or trap. */
static void test_persistent_handle_null_falls_back_correctly(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 8);

  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, NULL, &m, &out);
  jello_vm_destroy(vm);

  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == fib_ref(8));
  printf("test_persistent_handle_null_falls_back_correctly: PASS\n");
}

/* W^X icache coherency -- repeated create/destroy cycles
 * maximise VA reuse before exercising a persistent handle so stale
 * instruction bytes cannot hide behind test order. */
#if defined(__x86_64__) || defined(__aarch64__)
static void test_wx_icache_repeated_create_destroy_then_persistent(void) {
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  for(int iter = 0; iter < 50; iter++) {
    jello_type_entry types[2];
    jello_type_id r0[2], r1[7];
    jello_insn e0[3], e1[9];
    jello_bc_function funcs[2] = {0};
    jello_bc_module m = {0};
    build_fib_module(types, r0, r1, e0, e1, funcs, &m, 10);

    jello_jit_module* jm = jello_jit_module_create(&m);
    assert(jm != NULL);

    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32);
    assert(out.as.i32 == fib_ref(10));

    jello_jit_module_destroy(jm);
  }
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2] = {0};
  jello_bc_module m = {0};
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 15);

  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);

  int32_t expected = fib_ref(15);
  for(int i = 0; i < 20; i++) {
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32);
    assert(out.as.i32 == expected);
  }

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_wx_icache_repeated_create_destroy_then_persistent: PASS\n");
}

static void test_reload_cycling_stress(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[1], r1_b[1];
  jello_insn e0_a[2], e1_a[2], e0_b[2], e1_b[2];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0};
  jello_bc_module ma = {0}, mb = {0};
  build_named_two_func_module(types_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &ma, 0, 1, 100);
  ma.atoms = g_two_func_atoms; ma.natoms = 2;
  build_named_two_func_module(types_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &mb, 0, 1, 200);
  mb.atoms = g_two_func_atoms; mb.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&ma);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric_with_jit(vm, jm, &ma, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 100);

  jello_jit_module_prepare_count = 0;
  const jello_bc_module* cur = &ma;
  int32_t expect = 100;
  for(int i = 0; i < 50; i++) {
    const jello_bc_module* nxt = (cur == &ma) ? &mb : &ma;
    int32_t nxt_expect = (cur == &ma) ? 200 : 100;
    jello_jit_reload_report report;
    int rc = jello_jit_module_reload(vm, jm, cur, nxt, 0, &report);
    assert(rc == 1);
    assert(report.n_changed == 1);
    jello_jit_reload_report_free(&report);
    memset(&out, 0, sizeof(out));
    assert(jello_vm_exec_numeric_with_jit(vm, jm, nxt, &out) == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32 && out.as.i32 == nxt_expect);
    cur = nxt;
    expect = nxt_expect;
  }
  assert(jello_jit_module_prepare_count == 50);
  (void)expect;

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_cycling_stress: PASS\n");
}

static void test_reload_cycling_stress_with_conflicts(void) {
  jello_type_entry types_ok_a[1], types_ok_b[1], types_bad[1];
  jello_type_id r0_a[1], r1_a[1], r0_b[1], r1_b[1], r0_bad[2], r1_bad[1];
  jello_insn e0_a[2], e1_a[2], e0_b[2], e1_b[2], e0_bad[3], e1_bad[1];
  jello_bc_function funcs_a[2] = {0}, funcs_b[2] = {0}, funcs_bad[2] = {0};
  jello_bc_module ma = {0}, mb = {0}, mbad = {0};
  build_named_two_func_module(types_ok_a, r0_a, r1_a, e0_a, e1_a, funcs_a, &ma, 0, 1, 100);
  ma.atoms = g_two_func_atoms; ma.natoms = 2;
  build_named_two_func_module(types_ok_b, r0_b, r1_b, e0_b, e1_b, funcs_b, &mb, 0, 1, 200);
  mb.atoms = g_two_func_atoms; mb.natoms = 2;
  build_two_func_module_arity_variant(types_bad, r0_bad, r1_bad, e0_bad, e1_bad, funcs_bad, &mbad, 0, 1, 777);
  mbad.atoms = g_two_func_atoms; mbad.natoms = 2;

  jello_jit_module* jm = jello_jit_module_create(&ma);
  assert(jm != NULL);
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);

  const jello_bc_module* cur = &ma;
  int32_t expect = 100;
  for(int i = 0; i < 50; i++) {
    jello_jit_reload_report report;
    jello_numeric_result out;
    if((i % 2) == 0) {
      int rc = jello_jit_module_reload(vm, jm, cur, &mbad, 0, &report);
      assert(rc == 0);
      assert(report.n_signature_conflicts == 1);
      jello_jit_reload_report_free(&report);
      memset(&out, 0, sizeof(out));
      assert(jello_vm_exec_numeric_with_jit(vm, jm, cur, &out) == JELLO_EXEC_OK);
      assert(out.kind == JELLO_NUM_I32 && out.as.i32 == expect);
    } else {
      const jello_bc_module* nxt = (cur == &ma) ? &mb : &ma;
      int32_t nxt_expect = (cur == &ma) ? 200 : 100;
      int rc = jello_jit_module_reload(vm, jm, cur, nxt, 1, &report);
      assert(rc == 1);
      jello_jit_reload_report_free(&report);
      memset(&out, 0, sizeof(out));
      assert(jello_vm_exec_numeric_with_jit(vm, jm, nxt, &out) == JELLO_EXEC_OK);
      assert(out.kind == JELLO_NUM_I32 && out.as.i32 == nxt_expect);
      cur = nxt;
      expect = nxt_expect;
    }
  }

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_reload_cycling_stress_with_conflicts: PASS\n");
}
#endif

/* Single-threaded baseline -- pause_all succeeds when idle, resume_all leaves
 * the heap usable again. */
static void test_pause_resume_all_idle_succeeds(void) {
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  assert(jello_jit_reload_pause_all(vm) == 1);
  jello_jit_reload_resume_all(vm);

  /* The heap must still work normally afterward -- not just "did not
   * crash," a real exec_numeric call through it. */
  jello_type_entry types[1];
  jello_type_id r[1] = {0};
  jello_insn insns[2];
  memset(insns, 0, sizeof(insns));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 7;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;
  jello_bc_function f = {0};
  f.name_atom = JELLO_BC_NO_NAME; f.nargs = 0; f.nregs = 1; f.cap_start = 1;
  f.reg_types = r; f.ninsns = 2; f.insns = insns;
  types[0].kind = JELLO_T_I32; memset(&types[0].as, 0, sizeof(types[0].as));
  jello_bc_module m = {0};
  m.ntypes = 1; m.types = types; m.nfuncs = 1; m.funcs = &f; m.entry = 0;

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(jello_vm_exec_numeric(vm, &m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32 && out.as.i32 == 7);

  jello_vm_destroy(vm);
  printf("test_pause_resume_all_idle_succeeds: PASS\n");
}

/* Threaded variant: thread A pauses the world for "reload" while thread B is
 * idle at the exact moment of the scan, then thread B calls
 * jello_vm_exec_numeric -- it must block in its own new vm_safepoint_check
 * call (exec_loop.c) rather than proceeding, and only resume once thread A
 * calls jello_jit_reload_resume_all. Proven by timing: thread A holds the
 * pause for a measurable duration before resuming, and thread B's own call
 * must take at least close to that long, not return near-instantly. */
enum { PAUSE_HOLD_MS = 150 };

typedef struct pause_thread_arg {
  jello_vm* vm;
  atomic_int* paused_flag;
  int ok;
} pause_thread_arg;

typedef struct waiter_thread_arg {
  jello_vm* vm;
  jello_bc_module* m;
  atomic_int* paused_flag;
  double elapsed_ms;
  int ok;
} waiter_thread_arg;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void* pause_thread_main(void* arg_) {
  pause_thread_arg* arg = (pause_thread_arg*)arg_;
  arg->ok = jello_jit_reload_pause_all(arg->vm);
  atomic_store(arg->paused_flag, 1);
  struct timespec req;
  req.tv_sec = 0;
  req.tv_nsec = PAUSE_HOLD_MS * 1000000L;
  nanosleep(&req, NULL);
  jello_jit_reload_resume_all(arg->vm);
  return NULL;
}

static void* waiter_thread_main(void* arg_) {
  waiter_thread_arg* arg = (waiter_thread_arg*)arg_;
  while(!atomic_load(arg->paused_flag)) { /* spin until thread A has actually paused the world */ }

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  double t0 = now_ms();
  jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
  arg->elapsed_ms = now_ms() - t0;
  arg->ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 == 7);
  return NULL;
}

static void test_reload_pause_blocks_fresh_call_on_other_thread(void) {
  jello_vm* vm_a = jello_vm_create();
  assert(vm_a != NULL);
  jello_vm* vm_b = jello_vm_create_joined(vm_a);
  assert(vm_b != NULL);

  jello_type_entry types[1];
  jello_type_id r[1] = {0};
  jello_insn insns[2];
  memset(insns, 0, sizeof(insns));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 7;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;
  jello_bc_function f = {0};
  f.name_atom = JELLO_BC_NO_NAME; f.nargs = 0; f.nregs = 1; f.cap_start = 1;
  f.reg_types = r; f.ninsns = 2; f.insns = insns;
  types[0].kind = JELLO_T_I32; memset(&types[0].as, 0, sizeof(types[0].as));
  jello_bc_module m = {0};
  m.ntypes = 1; m.types = types; m.nfuncs = 1; m.funcs = &f; m.entry = 0;

  atomic_int paused_flag;
  atomic_init(&paused_flag, 0);
  pause_thread_arg a_arg = {vm_a, &paused_flag, 0};
  waiter_thread_arg b_arg = {vm_b, &m, &paused_flag, 0.0, 0};

  pthread_t ta, tb;
  assert(pthread_create(&ta, NULL, pause_thread_main, &a_arg) == 0);
  assert(pthread_create(&tb, NULL, waiter_thread_main, &b_arg) == 0);
  assert(pthread_join(ta, NULL) == 0);
  assert(pthread_join(tb, NULL) == 0);

  printf("test_reload_pause_blocks_fresh_call_on_other_thread: PASS (paused=%d, waiter_ok=%d, waiter_elapsed_ms=%.1f)\n",
      a_arg.ok, b_arg.ok, b_arg.elapsed_ms);
  assert(a_arg.ok && "thread A's own pause_all must have succeeded (heap was idle)");
  assert(b_arg.ok && "thread B's own call must still produce the correct result once resumed");
  /* The actual proof: thread B's own call could not have returned near-
   * instantly if it was really blocked in vm_safepoint_check for the whole
   * hold duration -- a generous margin below PAUSE_HOLD_MS to absorb
   * scheduler noise without making this fixture flaky. */
  assert(b_arg.elapsed_ms >= (double)PAUSE_HOLD_MS * 0.5 &&
      "thread B's own call returned too quickly -- it did not actually block on thread A's pause");

  jello_vm_destroy(vm_b);
  jello_vm_destroy(vm_a);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  test_reload_analyze_unchanged();
  test_reload_analyze_changed();
  test_reload_analyze_signature_conflict_arg_count();
  test_reload_analyze_signature_conflict_ret_kind();
  test_module_create_destroy_roundtrip();
  test_module_create_rejects_empty_module();
  test_pause_resume_all_idle_succeeds();
  test_reload_pause_blocks_fresh_call_on_other_thread();
  test_reload_unchanged_succeeds();
  test_reload_refuses_struct_type_conflict_by_default();
  test_reload_allow_conflicts_recompiles_even_if_body_unchanged();
  test_reload_new_function_recompiles_even_if_nothing_changed();
#if defined(__x86_64__) || defined(__aarch64__)
  test_reload_refuses_signature_conflict_by_default();
  test_reload_allow_conflicts_overrides_refusal();
  test_reload_removal_runs_shifted_function();
  test_persistent_handle_no_per_call_recompile();
  test_persistent_handle_null_falls_back_correctly();
#endif
#if defined(__x86_64__)
  test_persistent_handle_matches_fresh_path();
  test_reload_changed_function_end_to_end();
  test_reload_survive_interaction();
#else
  printf("test_persistent_handle_matches_fresh_path: SKIPPED (host is not x86_64)\n");
  printf("test_reload_changed_function_end_to_end: SKIPPED (host is not x86_64)\n");
  printf("test_reload_survive_interaction: SKIPPED (host is not x86_64)\n");
#endif
#if defined(__x86_64__) || defined(__aarch64__)
  test_wx_icache_repeated_create_destroy_then_persistent();
  test_reload_cycling_stress();
  test_reload_cycling_stress_with_conflicts();
#endif
  printf("jit_reload_tests: all tests passed\n");
  return 0;
}
