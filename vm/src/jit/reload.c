// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Hot reload; compares an old and a new module function by function, then swaps the new
 * module's code into a JIT module while every thread is paused. */

#include <jello/internal/jit_internal.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

/** FNV-1a, as a change detector, not a security boundary. */
static uint64_t fnv1a_update(uint64_t h, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for(size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/** Folds the full shape of type `tid` into `h`; its kind and, for a struct, its field count,
 * mutability, and each field's type and name. Names are hashed as strings and types by shape,
 * since atom and type ids differ between modules. `depth` bounds the recursion on a corrupt
 * module. */
static uint64_t hash_type(const jello_bc_module* m, jello_type_id tid, uint64_t h, int depth) {
  if(tid >= m->ntypes || depth > 8) return fnv1a_update(h, &tid, sizeof(tid));
  jello_type_kind k = m->types[tid].kind;
  h = fnv1a_update(h, &k, sizeof(k));
  if(k != JELLO_T_STRUCT) return h;
  uint32_t sig_id = m->types[tid].as.strct.struct_sig_id;
  if(sig_id >= m->nstruct_sigs) return h;
  const jello_struct_sig* sig = &m->struct_sigs[sig_id];
  h = fnv1a_update(h, &sig->nfields, sizeof(sig->nfields));
  h = fnv1a_update(h, &sig->is_mutable, sizeof(sig->is_mutable));
  for(uint32_t i = 0; i < sig->nfields; i++) {
    h = hash_type(m, sig->field_type[i], h, depth + 1);
    if(sig->named && sig->field_name_atom) {
      uint32_t atom = sig->field_name_atom[i];
      if(atom < m->natoms) h = fnv1a_update(h, m->atoms[atom], strlen(m->atoms[atom]));
    }
  }
  return h;
}

typedef struct jello_reload_signature {
  uint64_t ret_hash;
  uint32_t nargs;
  uint64_t arg_hash[256]; /**< A function has at most 256 registers. */
} jello_reload_signature;

/** Hashes the shapes of `f`'s return type, taken from its RET instructions, and argument types.
 * Returns 0 when two RETs return different types or a RET names a register out of range. */
static int compute_signature(const jello_bc_module* m, const jello_bc_function* f,
                             jello_reload_signature* out) {
  out->ret_hash = 0;
  int saw_ret = 0;
  for(uint32_t pc = 0; pc < f->ninsns; pc++) {
    if((jello_op)f->insns[pc].op != JOP_RET) continue;
    uint32_t reg = f->insns[pc].a;
    if(reg >= f->nregs) return 0;
    uint64_t rh = hash_type(m, f->reg_types[reg], 1469598103934665603ULL, 0);
    if(saw_ret && rh != out->ret_hash) return 0;
    out->ret_hash = rh;
    saw_ret = 1;
  }
  out->nargs = f->nargs;
  for(uint32_t i = 0; i < f->nargs; i++) {
    out->arg_hash[i] = hash_type(m, f->reg_types[i], 1469598103934665603ULL, 0);
  }
  return 1;
}

static int signatures_match(const jello_reload_signature* a, const jello_reload_signature* b) {
  if(a->ret_hash != b->ret_hash) return 0;
  if(a->nargs != b->nargs) return 0;
  for(uint32_t i = 0; i < a->nargs; i++) {
    if(a->arg_hash[i] != b->arg_hash[i]) return 0;
  }
  return 1;
}

/** Hashes every register's type shape, so a local whose struct type changed counts too, and the
 * raw instructions; jello_insn has no padding, so its bytes hash deterministically. */
static uint64_t compute_body_hash(const jello_bc_module* m, const jello_bc_function* f) {
  uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
  for(uint32_t r = 0; r < f->nregs; r++) h = hash_type(m, f->reg_types[r], h, 0);
  if(f->ninsns > 0) h = fnv1a_update(h, f->insns, (size_t)f->ninsns * sizeof(jello_insn));
  return h;
}

jello_reload_fn_status jello_reload_analyze_function(const jello_bc_module* old_m,
                                                     const jello_bc_function* old_f,
                                                     const jello_bc_module* new_m,
                                                     const jello_bc_function* new_f) {
  jello_reload_signature sig_old, sig_new;
  if(!compute_signature(old_m, old_f, &sig_old)) return JELLO_RELOAD_FN_SIGNATURE_CONFLICT;
  if(!compute_signature(new_m, new_f, &sig_new)) return JELLO_RELOAD_FN_SIGNATURE_CONFLICT;
  if(!signatures_match(&sig_old, &sig_new)) return JELLO_RELOAD_FN_SIGNATURE_CONFLICT;

  uint64_t hash_old = compute_body_hash(old_m, old_f);
  uint64_t hash_new = compute_body_hash(new_m, new_f);
  return (hash_old == hash_new) ? JELLO_RELOAD_FN_UNCHANGED : JELLO_RELOAD_FN_CHANGED;
}

int jello_reload_struct_sig_changed(const jello_bc_module* old_m, const jello_bc_module* new_m,
                                    uint32_t struct_sig_id) {
  if(struct_sig_id >= old_m->nstruct_sigs || struct_sig_id >= new_m->nstruct_sigs) return 1;
  const jello_struct_sig* a = &old_m->struct_sigs[struct_sig_id];
  const jello_struct_sig* b = &new_m->struct_sigs[struct_sig_id];
  if(a->nfields != b->nfields || a->named != b->named || a->is_mutable != b->is_mutable) return 1;
  for(uint32_t i = 0; i < a->nfields; i++) {
    uint64_t ha = hash_type(old_m, a->field_type[i], 1469598103934665603ULL, 0);
    uint64_t hb = hash_type(new_m, b->field_type[i], 1469598103934665603ULL, 0);
    if(ha != hb) return 1;
    if(a->named) {
      uint32_t aa = a->field_name_atom[i], ab = b->field_name_atom[i];
      const char* na = aa < old_m->natoms ? old_m->atoms[aa] : "";
      const char* nb = ab < new_m->natoms ? new_m->atoms[ab] : "";
      if(strcmp(na, nb) != 0) return 1;
    }
  }
  return 0;
}

int jello_reload_function_uses_struct_sig(const jello_bc_module* m, const jello_bc_function* f,
                                          uint32_t struct_sig_id) {
  for(uint32_t r = 0; r < f->nregs; r++) {
    jello_type_id tid = f->reg_types[r];
    if(tid >= m->ntypes) continue;
    if(m->types[tid].kind != JELLO_T_STRUCT) continue;
    if(m->types[tid].as.strct.struct_sig_id == struct_sig_id) return 1;
  }
  return 0;
}

int jello_jit_module_reload(jello_vm* vm, jello_jit_module* jm, const jello_bc_module* old_m,
                            const jello_bc_module* new_m, int allow_conflicts,
                            jello_jit_reload_report* out_report) {
  if(!out_report) return 0;
  memset(out_report, 0, sizeof(*out_report));
  if(!vm || !jm || !jm->prepared || !old_m || !new_m || new_m->nfuncs == 0) return 0;

  out_report->fn_status =
      (jello_reload_fn_status*)calloc(new_m->nfuncs, sizeof(jello_reload_fn_status));
  if(!out_report->fn_status) return 0;

  uint32_t min_sigs =
      old_m->nstruct_sigs < new_m->nstruct_sigs ? old_m->nstruct_sigs : new_m->nstruct_sigs;
  uint8_t* struct_conflict = min_sigs ? (uint8_t*)calloc(min_sigs, 1) : NULL;
  if(min_sigs && !struct_conflict) {
    free(out_report->fn_status);
    out_report->fn_status = NULL;
    return 0;
  }
  for(uint32_t i = 0; i < min_sigs; i++) {
    if(jello_reload_struct_sig_changed(old_m, new_m, i)) struct_conflict[i] = 1;
  }

  uint8_t* old_matched = old_m->nfuncs ? (uint8_t*)calloc(old_m->nfuncs, 1) : NULL;
  if(old_m->nfuncs && !old_matched) {
    free(struct_conflict);
    free(out_report->fn_status);
    out_report->fn_status = NULL;
    return 0;
  }

  for(uint32_t nfi = 0; nfi < new_m->nfuncs; nfi++) {
    const jello_bc_function* new_f = &new_m->funcs[nfi];
    jello_reload_fn_status status = JELLO_RELOAD_FN_NEW;
    uint32_t old_fi = 0;
    int found = 0;

    if(new_f->name_atom != JELLO_BC_NO_NAME && new_f->name_atom < new_m->natoms) {
      const char* new_name = new_m->atoms[new_f->name_atom];
      for(uint32_t ofi = 0; ofi < old_m->nfuncs; ofi++) {
        const jello_bc_function* old_f = &old_m->funcs[ofi];
        if(old_f->name_atom == JELLO_BC_NO_NAME || old_f->name_atom >= old_m->natoms) continue;
        if(strcmp(old_m->atoms[old_f->name_atom], new_name) == 0) {
          old_fi = ofi;
          found = 1;
          old_matched[ofi] = 1;
          break;
        }
      }
    }

    int is_type_conflict = 0;
    if(found) {
      const jello_bc_function* old_f = &old_m->funcs[old_fi];
      status = jello_reload_analyze_function(old_m, old_f, new_m, new_f);
      /* A function using a struct whose shape changed conflicts even when nothing else about it
       * did. */
      for(uint32_t sid = 0; sid < min_sigs; sid++) {
        if(!struct_conflict[sid]) continue;
        if(jello_reload_function_uses_struct_sig(old_m, old_f, sid) ||
           jello_reload_function_uses_struct_sig(new_m, new_f, sid)) {
          status = JELLO_RELOAD_FN_SIGNATURE_CONFLICT;
          is_type_conflict = 1;
        }
      }
    }

    out_report->fn_status[nfi] = status;
    switch(status) {
      case JELLO_RELOAD_FN_UNCHANGED: out_report->n_unchanged++; break;
      case JELLO_RELOAD_FN_CHANGED: out_report->n_changed++; break;
      case JELLO_RELOAD_FN_NEW: out_report->n_new++; break;
      case JELLO_RELOAD_FN_SIGNATURE_CONFLICT:
        if(is_type_conflict) out_report->n_type_conflicts++;
        else out_report->n_signature_conflicts++;
        break;
      default: break;
    }
  }

  for(uint32_t ofi = 0; ofi < old_m->nfuncs; ofi++) {
    if(!old_matched[ofi]) out_report->n_removed++;
  }
  free(old_matched);
  free(struct_conflict);

  // jm keeps its old code.
  if((out_report->n_signature_conflicts > 0 || out_report->n_type_conflicts > 0) &&
     !allow_conflicts) {
    return 0;
  }

  /* Emitted code embeds pointers into the module it was compiled from, and jm indexes its code
   * by function index, which a removal shifts. So unchanged functions are recompiled too, unless
   * jm was already compiled from new_m. */
  if(new_m == jm->m) return 1;

  // Compile before pausing; nothing here is visible to other threads.
  jello_jit_module new_jm;
  memset(&new_jm, 0, sizeof(new_jm));
  if(!jello_jit_module_prepare(new_m, &new_jm)) return 0;

  /* Pausing fails while another stop-the-world, such as a collection, is in flight or a thread
   * is executing, so retry a bounded number of times. */
  {
    int paused = 0;
    for(int tries = 0; tries < 64; tries++) {
      if(jello_jit_reload_pause_all(vm)) {
        paused = 1;
        break;
      }
      sched_yield();
    }
    if(!paused) {
      jello_jit_module_free(&new_jm);
      return 0;
    }
  }

  /* No thread is executing, so no JIT frame, return address or entries-table pointer refers to
   * the old blob, and it can be freed. */
  jello_jit_module old_jm = *jm;
  *jm = new_jm;

  jello_jit_reload_resume_all(vm);

  jello_jit_module_free(&old_jm);
  return 1;
}

void jello_jit_reload_report_free(jello_jit_reload_report* report) {
  if(!report) return;
  free(report->fn_status);
  report->fn_status = NULL;
}

int jello_jit_reload_pause_all(jello_vm* vm) {
  if(!vm_heap_stop_the_world(vm)) return 0;
  jello_heap* heap = vm->heap;
  for(uint32_t i = 0; i < heap->threads_len; i++) {
    int in_exec = atomic_load(&heap->threads[i]->in_exec);
    if(in_exec != 0 && in_exec != VM_RUNNING_OUTSIDE) {
      vm_heap_resume_the_world(vm);
      return 0;
    }
  }
  return 1;
}

void jello_jit_reload_resume_all(jello_vm* vm) { vm_heap_resume_the_world(vm); }
