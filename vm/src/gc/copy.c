// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Copying a value's object graph, so that a message behaves as a copy: neither its sender nor its
 * receiver sees the other's writes. Sharing and cycles survive. Boxes, buffers and small Bytes are
 * copied too, a slice only for what it holds, so that each process owns what it reaches. Shared
 * are immortal objects, the functions CONST_FUN makes and Bytes literals, which belong to no heap,
 * and the bytes of large Bytes, which live in shared binaries, each held by reference count; no
 * Bytes but a buffer is ever written. A native handle copied to another process is a new handle
 * holding the same payload, by its class's `retain`; one whose class has none cannot leave its
 * process: a message or a new process's body holding one is refused, while a new process's
 * globals and an exit value hold null for it. A message or a new process's body takes the
 * sockets the copier's process owns, for its receiver to own; a new process's globals and an
 * exit value hold one that is not theirs, which reads as closed.
 *
 * Each process has its own globals, and so its own prototypes. A copy for a new process copies an
 * Object's prototype with the globals that hold it; one for another process leaves a prototype
 * one of the copier's globals holds for the receiver to point at its own value of that global,
 * and copies any other.
 *
 * The copy works from a worklist rather than recursing, so a long list needs no deeper stack.
 * Each new object starts as a shallow copy, its references still to the originals, until the
 * worklist reaches it. Every copy goes into a parcel, in no space, so a copy never collects and
 * needs no roots: a message's receiver, a new process and each reader of a reply or an exit
 * value, which many may read, take a copy's objects into their own space whole. */

#include <jello/internal.h>
#include <jello/internal/proc_internal.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** The objects a copy reaches before its copier allocates, as many as most messages hold. Its map,
 * which grows at half full, has twice as many slots inline. */
#define COPY_INLINE 16u

typedef struct copier {
  jello_vm* vm;
  const jello_bc_module* m;
  int out;   /**< Packs for another process, leaving it a prototype one of the copier's globals
              * holds, rather than copying into one, which copies prototypes as any object. */
  int drop;  /**< Copies a handle that cannot leave its process as null, rather than refusing. */
  int moves; /**< Takes the sockets it copies from the copier's process. */
  struct proc_socket_list* sockets; /**< Those it has copied, while it `moves`. */
  int refused;      /**< Met a handle that cannot leave its process, and was not to drop it. */
  vm_protos* fixes; /**< A pack's copies that delegate to a global, or NULL for none. */
  uint32_t fixes_cap;
  struct vm_proto_fix* found; /**< The prototypes global_holding has looked for, and where. */
  uint32_t found_len;
  uint32_t found_cap;
  void** keys; /**< Originals copied, an open-addressing table of cap, NULL for empty. */
  void** vals; /**< Each one's copy. */
  uint32_t cap;
  uint32_t len;
  uint32_t shift; /**< 64 less log2(cap), for slot_of. */
  void** work;    /**< Copies whose references still lead to originals. */
  uint32_t work_len;
  uint32_t work_cap;
  /* `keys`, `vals` and `work` until they outgrow these, last so that only the fields above are
   * cleared at each copy. */
  void* keys_inline[2u * COPY_INLINE];
  void* vals_inline[2u * COPY_INLINE];
  void* work_inline[COPY_INLINE];
} copier;

/** Fibonacci hashing: the top bits of the product, which every bit of the address reaches. */
static uint32_t slot_of(const copier* c, const void* p) {
  return (uint32_t)(((uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull) >> c->shift);
}

static void map_put(copier* c, void* from, void* to);

static void map_grow(copier* c) {
  void** keys = c->keys;
  void** vals = c->vals;
  uint32_t cap = c->cap;
  c->len = 0;
  c->cap = cap * 2u;
  c->shift--;
  c->keys = (void**)calloc(c->cap, sizeof(void*));
  c->vals = (void**)malloc(sizeof(void*) * c->cap);
  if(!c->keys || !c->vals) abort();
  for(uint32_t i = 0; i < cap; i++) {
    if(keys[i]) map_put(c, keys[i], vals[i]);
  }
  if(keys != c->keys_inline) {
    free(keys);
    free(vals);
  }
}

static void map_put(copier* c, void* from, void* to) {
  if(2u * (c->len + 1u) > c->cap) map_grow(c);
  uint32_t i = slot_of(c, from);
  while(c->keys[i]) i = (i + 1u) & (c->cap - 1u);
  c->keys[i] = from;
  c->vals[i] = to;
  c->len++;
}

static void* map_get(const copier* c, const void* from) {
  for(uint32_t i = slot_of(c, from); c->keys[i]; i = (i + 1u) & (c->cap - 1u)) {
    if(c->keys[i] == from) return c->vals[i];
  }
  return NULL;
}

/** `p`, a block of `header` bytes and *cap elements of `elem`, grown to hold `len + 1`; doubling
 * from `first`. */
static void* grown(void* p, uint32_t len, uint32_t* cap, size_t header, size_t elem,
                   uint32_t first) {
  if(p && len < *cap) return p;
  *cap = *cap ? *cap * 2u : first;
  p = realloc(p, header + elem * *cap);
  if(!p) abort();
  return p;
}

static void work_push(copier* c, void* p) {
  if(c->work_len == c->work_cap) {
    void** old = c->work;
    int was_inline = old == c->work_inline;
    c->work =
        (void**)grown(was_inline ? NULL : old, c->work_len, &c->work_cap, 0, sizeof(void*), 0);
    if(was_inline) memcpy(c->work, old, sizeof c->work_inline);
  }
  c->work[c->work_len++] = p;
}

/** Whether object `p` leaves its copy no reference to point at a copy: Bytes, boxes and
 * handles. */
static int leaf(const void* p) {
  switch((jello_obj_kind)((const jello_obj_header*)p)->kind) {
    case JELLO_OBJ_BYTES:
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16:
    case JELLO_OBJ_HANDLE: return 1;
    default: return 0;
  }
}

/** A shallow copy of `p`, or `p` itself when it is shared. */
static void* shallow_copy(copier* c, void* p) {
  if(vm_is_immortal(p)) return p;
  jello_vm* vm = c->vm;
  const jello_bc_module* m = c->m;
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_BYTES: return vm_bytes_clone(vm, (const jello_bytes*)p);
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16: return jello_as_ptr(vm_clone_numbox(vm, jello_from_ptr(p)));
    case JELLO_OBJ_STRUCT: {
      jello_struct* n = jello_struct_new(vm, m, oh->type_id);
      memcpy(n->fields, ((const jello_struct*)p)->fields, vm_struct_fields_bytes(m, oh->type_id));
      return n;
    }
    case JELLO_OBJ_OBJECT: return vm_object_clone(vm, (const jello_object*)p);
    case JELLO_OBJ_ARRAY: {
      const jello_array* a = (const jello_array*)p;
      jello_array* n = jello_array_new(vm, m, oh->type_id, a->length);
      jello_type_kind ek = m->types[m->types[oh->type_id].as.unary.elem].kind;
      if(a->length) memcpy(n->data, a->data, (size_t)a->length * jello_slot_size(ek));
      return n;
    }
    case JELLO_OBJ_LIST: {
      const jello_list* l = (const jello_list*)p;
      return jello_list_cons(vm, oh->type_id, l->head, l->tail);
    }
    case JELLO_OBJ_ENUM: {
      const jello_enum* e = (const jello_enum*)p;
      return jello_enum_new(vm, oh->type_id, e->tag, e->nfields, e->fields);
    }
    case JELLO_OBJ_HANDLE: {
      const jello_handle* hd = (const jello_handle*)p;
      /* One it cannot note stays with the copier's process, and reads as closed in the copy. */
      if(c->moves && hd->cls == &proc_socket_class) proc_socket_note(&c->sockets, hd->payload);
      if(!hd->cls->retain) {
        c->refused |= !c->drop;
        return c->drop ? NULL : p;
      }
      hd->cls->retain(hd->payload);
      return jello_handle_new(vm, hd->cls, hd->payload, hd->native_bytes);
    }
    case JELLO_OBJ_FUNCTION: {
      const jello_function* f = (const jello_function*)p;
      jello_function* n = jello_function_new(vm, f->func_index, f->ncaps);
      n->h.type_id = oh->type_id;
      memcpy(n->captures, f->captures, sizeof(uint64_t) * f->ncaps);
      return n;
    }
    default: abort(); /* Shared, it could outlive its space while the copy reaches it. */
  }
}

/** The copy of object `p`, made when it has none yet. */
static void* copy_ptr(copier* c, void* p) {
  if(!p) return NULL;
  void* n = map_get(c, p);
  if(n) return n;
  n = shallow_copy(c, p);
  if(n == p || !n) return n; /* Shared, or dropped; shallow_copy decides again at each visit. */
  map_put(c, p, n);
  if(!leaf(n)) work_push(c, n);
  return n;
}

/** Points the reference at `slot`, a pointer or, when `dynamic`, a Dynamic word, at its copy. */
static void copy_slot(copier* c, void* slot, int dynamic) {
  if(dynamic) {
    jello_value v;
    memcpy(&v, slot, sizeof v);
    if(!jello_is_ptr(v)) return;
    void* n = copy_ptr(c, jello_as_ptr(v));
    v = n ? jello_from_ptr(n) : jello_make_null(); /* A handle dropped is null. */
    memcpy(slot, &v, sizeof v);
  } else {
    void* p;
    memcpy(&p, slot, sizeof p);
    p = copy_ptr(c, p);
    memcpy(slot, &p, sizeof p);
  }
}

/** `o`'s own `__proto__` slot, which holds its prototype while it has one, or NULL. */
static jello_value* proto_slot(jello_object* o) {
  for(uint32_t i = 0; i < o->cap; i++) {
    if(vm_obj_slot_live(o->states[i]) && o->keys[i] == JELLO_ATOM___PROTO__) return &o->vals[i];
  }
  return NULL;
}

/** Links `o` to `proto`, or to nothing when it is NULL, in `slot`, its `__proto__` slot or NULL
 * when it has none, as well. */
static void set_proto(jello_object* o, jello_value* slot, jello_object* proto) {
  if(slot) *slot = proto ? jello_from_ptr(proto) : jello_make_null();
  o->proto = proto;
}

/** The first of the copier's globals holding `proto`, or UINT32_MAX for none; each prototype is
 * looked for once a copy. */
static uint32_t global_holding(copier* c, jello_object* proto) {
  for(uint32_t i = 0; i < c->found_len; i++) {
    if(c->found[i].object == proto) return c->found[i].global;
  }
  const jello_bc_module* m = c->m;
  uint32_t g = UINT32_MAX;
  for(uint32_t j = 0; m && j < m->nptr_globals && g == UINT32_MAX; j++) {
    if(vm_global_ptr(m, c->vm->globals, m->ptr_globals[j]) == proto) g = m->ptr_globals[j];
  }
  c->found = (struct vm_proto_fix*)grown(c->found, c->found_len, &c->found_cap, 0,
                                         sizeof(struct vm_proto_fix), 4u);
  c->found[c->found_len++] = (struct vm_proto_fix){proto, g};
  return g;
}

/** What copy `o` delegates to, by where the copy goes; NULL, recorded, for the receiver to set. */
static jello_object* copy_proto(copier* c, jello_object* o) {
  uint32_t g = c->out ? global_holding(c, o->proto) : UINT32_MAX;
  if(g == UINT32_MAX) return (jello_object*)copy_ptr(c, o->proto);
  uint32_t len = c->fixes ? c->fixes->len : 0u;
  c->fixes = (vm_protos*)grown(c->fixes, len, &c->fixes_cap, sizeof(vm_protos),
                               sizeof(struct vm_proto_fix), 8u);
  c->fixes->at[len] = (struct vm_proto_fix){o, g};
  c->fixes->len = len + 1u;
  return NULL;
}

/** Points the references of copy `p` at copies of what they lead to. */
static void copy_refs(copier* c, void* p) {
  const jello_bc_module* m = c->m;
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_STRUCT: {
      jello_struct* s = (jello_struct*)p;
      const jello_struct_sig* sig = &m->struct_sigs[oh->type_id];
      for(uint16_t i = 0; i < sig->nfields; i++) {
        jello_type_kind fk = m->types[sig->field_type[i]].kind;
        if(!vm_kind_needs_gc_scan(fk)) continue;
        copy_slot(c, s->fields + sig->field_offset[i], fk == JELLO_T_DYNAMIC);
      }
      return;
    }
    case JELLO_OBJ_OBJECT: {
      jello_object* o = (jello_object*)p;
      /* The prototype's slot, which holds its link, follows the link rather than being copied. */
      jello_value* link = NULL;
      for(uint32_t i = 0; i < o->cap; i++) {
        if(!vm_obj_slot_live(o->states[i])) continue;
        if(o->proto && o->keys[i] == JELLO_ATOM___PROTO__) link = &o->vals[i];
        else copy_slot(c, &o->vals[i], 1);
      }
      if(o->proto) set_proto(o, link, copy_proto(c, o));
      return;
    }
    case JELLO_OBJ_ARRAY: {
      jello_array* a = (jello_array*)p;
      jello_type_kind ek = m->types[m->types[oh->type_id].as.unary.elem].kind;
      if(!vm_kind_needs_gc_scan(ek)) return;
      size_t size = jello_slot_size(ek);
      for(uint32_t i = 0; i < a->length; i++) {
        copy_slot(c, (uint8_t*)a->data + i * size, ek == JELLO_T_DYNAMIC);
      }
      return;
    }
    case JELLO_OBJ_LIST: {
      jello_list* l = (jello_list*)p;
      copy_slot(c, &l->head, 1);
      copy_slot(c, &l->tail, 0);
      return;
    }
    case JELLO_OBJ_ENUM: {
      jello_enum* e = (jello_enum*)p;
      for(uint32_t i = 0; i < e->nfields; i++) copy_slot(c, &e->fields[i], 1);
      return;
    }
    case JELLO_OBJ_FUNCTION: {
      jello_function* f = (jello_function*)p;
      const jello_bc_function* callee = &m->funcs[f->func_index];
      for(uint32_t i = 0; i < f->ncaps; i++) {
        jello_type_kind ck = vm_reg_kind(m, callee, callee->cap_start + i);
        if(vm_kind_needs_gc_scan(ck)) copy_slot(c, &f->captures[i], ck == JELLO_T_DYNAMIC);
      }
      return;
    }
    default: return;
  }
}

static void copier_free(copier* c) {
  if(c->keys != c->keys_inline) {
    free(c->keys);
    free(c->vals);
  }
  if(c->work != c->work_inline) free(c->work);
  free(c->fixes);
  free(c->found);
}

/** Copies `v` for where `c` sends it, leaving the copier to free. */
static jello_value copy_root(copier* c, jello_value v) {
  if(!jello_is_ptr(v) || !jello_as_ptr(v)) return v;
  void* root = copy_ptr(c, jello_as_ptr(v));
  if(!root) return jello_make_null(); /* A handle dropped. */
  while(c->work_len) copy_refs(c, c->work[--c->work_len]);
  return jello_from_ptr(root);
}

/** Starts `c`'s copy, for `vm`, into `out`, emptied, where `vm` allocates until copy_end. `pack`
 * and `drop` are its `out` and `drop`. */
static void copy_begin(copier* c, jello_vm* vm, int pack, int drop, vm_parcel* out) {
  memset(c, 0, offsetof(copier, keys_inline));
  c->vm = vm;
  c->m = vm->running_module;
  c->out = pack;
  c->drop = drop;
  memset(c->keys_inline, 0, sizeof c->keys_inline);
  c->keys = c->keys_inline;
  c->vals = c->vals_inline;
  c->cap = 2u * COPY_INLINE;
  c->shift = 64u - (uint32_t)__builtin_ctz(2u * COPY_INLINE);
  c->work = c->work_inline;
  c->work_cap = COPY_INLINE;
  *out = vm_parcel_empty();
  vm->parcel = out;
}

/** Ends `c`'s copy into `out`, handing it the prototypes to fix and freeing the copier.
 * @return JELLO_TRAP_UNSENDABLE, freeing `out`, when the copy met what cannot leave its process;
 * else JELLO_TRAP_NONE */
static jello_trap_code copy_end(copier* c, vm_parcel* out) {
  c->vm->parcel = NULL;
  if(c->sockets) proc_sockets_pack(c->refused ? NULL : c->vm, &c->sockets);
  out->sockets = c->sockets;
  out->protos = c->fixes;
  c->fixes = NULL;
  copier_free(c);
  if(!c->refused) return JELLO_TRAP_NONE;
  vm_parcel_free(out);
  return JELLO_TRAP_UNSENDABLE;
}

/** Links `o` to what global `g` holds in `vm`'s process, or to nothing when that is no Object. */
static void fix_proto(const jello_vm* vm, jello_object* o, uint32_t g) {
  void* p = vm_global_ptr(vm->running_module, vm->globals, g);
  jello_object* proto = p && ((const jello_obj_header*)p)->kind == JELLO_OBJ_OBJECT ? p : NULL;
  set_proto(o, proto_slot(o), proto);
}

jello_trap_code vm_parcel_pack(jello_vm* vm, jello_value v, int drop, vm_parcel* out) {
  if(!jello_is_ptr(v) || !jello_as_ptr(v)) { /* Spares a scalar setting up a copier. */
    *out = vm_parcel_empty();
    out->value = v;
    return JELLO_TRAP_NONE;
  }
  copier c;
  copy_begin(&c, vm, 1, drop, out);
  c.moves = !drop;
  out->value = copy_root(&c, v);
  return copy_end(&c, out);
}

/** Points each Object `protos` lists, or its copy by `c` when that is not NULL, at `vm`'s value
 * of its global. */
static void fix_protos(jello_vm* vm, const vm_protos* protos, const copier* c) {
  for(uint32_t i = 0; protos && i < protos->len; i++) {
    jello_object* o = protos->at[i].object;
    if(c) o = (jello_object*)map_get(c, o);
    if(o) fix_proto(vm, o, protos->at[i].global);
  }
}

jello_value vm_parcel_take(jello_vm* vm, vm_parcel* pc) {
  vm_parcel_splice(vm, vm->space, pc);
  fix_protos(vm, pc->protos, NULL);
  jello_value v = pc->value;
  if(pc->sockets) proc_sockets_take(vm->proc, &pc->sockets);
  vm_parcel_free(pc); /* Its objects are the space's now; this frees the prototype list. */
  return v;
}

jello_value vm_parcel_read(jello_vm* vm, vm_parcel* pc) {
  copier c;
  vm_parcel copy;
  copy_begin(&c, vm, 0, 0, &copy);
  copy.value = copy_root(&c, pc->value);
  fix_protos(vm, pc->protos, &c);
  copy_end(&c, &copy); /* Never refuses: a parcel holds only what may leave. */
  vm_parcel_splice(vm, vm->space, &copy);
  /* Unconditionally, as other reads may race for them: the take swaps them out atomically. */
  proc_sockets_take(vm->proc, &pc->sockets);
  return copy.value;
}

jello_trap_code vm_copy_start(jello_vm* vm, jello_space* into, void** root, uint64_t* globals,
                              struct proc_socket_list** sockets) {
  copier c;
  vm_parcel copy;
  copy_begin(&c, vm, 0, 0, &copy);
  /* The body first, refusing what it captures, then the globals, which it may never read. */
  c.moves = 1;
  copy_slot(&c, root, 0);
  while(c.work_len) copy_refs(&c, c.work[--c.work_len]);
  if(c.sockets) proc_sockets_pack(c.refused ? NULL : vm, &c.sockets);
  *sockets = c.sockets;
  c.sockets = NULL;
  c.moves = 0;
  c.drop = 1;
  for(uint32_t j = 0; !c.refused && c.m && j < c.m->nptr_globals; j++) {
    uint32_t i = c.m->ptr_globals[j];
    copy_slot(&c, &globals[i], vm_global_kind(c.m, i) == JELLO_T_DYNAMIC);
  }
  while(c.work_len) copy_refs(&c, c.work[--c.work_len]);
  jello_trap_code trap = copy_end(&c, &copy);
  if(!trap) vm_parcel_splice(vm, into, &copy);
  return trap;
}
