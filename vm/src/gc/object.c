// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Object tables; open addressing with linear probing and tombstones, keyed by atom id, with a
 * power-of-two capacity. */

#include <jello/internal.h>
#include <stdlib.h>
#include <string.h>

/** Bytes in a table block of `cap` slots; keys, padding to align the values, values, states. */
static size_t table_bytes(uint32_t cap) {
  /* Only a size_t no wider than the count can overflow. */
#if SIZE_MAX <= UINT32_MAX
  if((size_t)cap > SIZE_MAX / sizeof(jello_value)) abort();
#endif
  size_t keys = (size_t)cap * sizeof(uint32_t);
  size_t align = sizeof(jello_value);
  size_t pad = (align - (keys % align)) % align;
  size_t vals = (size_t)cap * sizeof(jello_value);
  size_t states = (size_t)cap;
  return keys + pad + vals + states;
}

static void bind_table(jello_object* o, uint8_t* base, uint32_t cap) {
  size_t keys = (size_t)cap * sizeof(uint32_t);
  size_t align = sizeof(jello_value);
  size_t pad = (align - (keys % align)) % align;
  o->keys = (uint32_t*)base;
  o->vals = (jello_value*)(base + keys + pad);
  o->states = base + keys + pad + (size_t)cap * sizeof(jello_value);
  o->cap = cap;
}

jello_object* jello_object_new(jello_vm* vm, uint32_t type_id) {
  jello_object* o = (jello_object*)jello_gc_alloc(vm, sizeof(jello_object));
  o->h.kind = (uint8_t)JELLO_OBJ_OBJECT;
  o->h.type_id = type_id;
  o->proto = NULL;
  o->len = 0;
  o->tombstones = 0;
  o->sealed = 0;
  uint8_t* block = (uint8_t*)malloc(table_bytes(JELLO_OBJECT_INIT_CAP));
  if(!block) abort();
  bind_table(o, block, JELLO_OBJECT_INIT_CAP);
  memset(o->states, 0, o->cap);
  return o;
}

/** Finds `atom_id`'s slot, setting `*found`. When it is absent, returns the slot an insert
 * should use; the first tombstone on the probe path, else the empty slot that ended it. With no
 * live entries the home slot is empty or a tombstone, so it is returned without probing. */
static uint32_t find_slot(jello_object* o, uint32_t atom_id, int* found) {
  if(o->len == 0u) {
    *found = 0;
    return vm_atom_hash(atom_id) & (o->cap - 1u);
  }
  uint32_t mask = o->cap - 1u;
  uint32_t i = vm_atom_hash(atom_id) & mask;
  uint32_t first_tomb = UINT32_MAX;
  for(;;) {
    uint8_t st = o->states[i];
    if(st == JELLO_OBJ_SLOT_EMPTY) {
      *found = 0;
      return (first_tomb != UINT32_MAX) ? first_tomb : i;
    }
    if(vm_obj_slot_live(st) && o->keys[i] == atom_id) {
      *found = 1;
      return i;
    }
    if(st == JELLO_OBJ_SLOT_TOMB && first_tomb == UINT32_MAX) first_tomb = i;
    i = (i + 1u) & mask;
  }
}

/** Finds `atom_id`'s occupied slot for a lookup, setting `*found`. */
static uint32_t find_occupied_slot(const jello_object* o, uint32_t atom_id, int* found) {
  if(o->len == 0u) {
    *found = 0;
    return 0;
  }
  uint32_t mask = o->cap - 1u;
  uint32_t i = vm_atom_hash(atom_id) & mask;
  for(;;) {
    uint8_t st = o->states[i];
    if(st == JELLO_OBJ_SLOT_EMPTY) {
      *found = 0;
      return 0;
    }
    if(vm_obj_slot_live(st) && o->keys[i] == atom_id) {
      *found = 1;
      return i;
    }
    i = (i + 1u) & mask;
  }
}

/** Rebuilds the table with `new_cap` slots, re-inserting the live entries and dropping every
 * tombstone. */
static void rehash(jello_object* o, uint32_t new_cap) {
  uint32_t* old_keys = o->keys;
  jello_value* old_vals = o->vals;
  uint8_t* old_states = o->states;
  uint32_t old_cap = o->cap;

  uint8_t* block = (uint8_t*)malloc(table_bytes(new_cap));
  if(!block) abort();
  bind_table(o, block, new_cap);
  memset(o->states, 0, new_cap);
  o->len = 0;
  o->tombstones = 0;

  for(uint32_t i = 0; i < old_cap; i++) {
    if(!vm_obj_slot_live(old_states[i])) continue;
    int found = 0;
    uint32_t slot = find_slot(o, old_keys[i], &found);
    o->keys[slot] = old_keys[i];
    o->states[slot] = old_states[i];
    o->vals[slot] = old_vals[i];
    o->len++;
  }
  free(old_keys);
}

/** Makes room for one more entry, keeping the load under 70%. Doubles the capacity when live
 * entries alone would pass it; otherwise, when live entries and tombstones together would, rehashes
 * at the same capacity to clear the tombstones, so a probe always reaches an empty slot. */
static void ensure_cap(jello_object* o) {
  if(((uint64_t)o->len + 1u) * JELLO_OBJECT_LOAD_NUM >= (uint64_t)o->cap * JELLO_OBJECT_LOAD_DEN) {
    if(o->cap > UINT32_MAX / 2u) abort();
    rehash(o, o->cap * 2u);
    return;
  }
  if(((uint64_t)o->len + o->tombstones + 1u) * JELLO_OBJECT_LOAD_NUM >=
     (uint64_t)o->cap * JELLO_OBJECT_LOAD_DEN) {
    rehash(o, o->cap);
  }
}

/* The states live in the table block, not in the object, so a const object can give up its box. */
int vm_object_find(const jello_object* o, uint32_t atom_id, int keep, jello_value* out) {
  int found = 0;
  uint32_t i = find_occupied_slot(o, atom_id, &found);
  if(!found) return 0;
  *out = o->vals[i];
  if(keep && o->states[i] == JELLO_OBJ_SLOT_OWNED) o->states[i] = JELLO_OBJ_SLOT_OCCUPIED;
  return 1;
}

int jello_object_has(const jello_object* o, uint32_t atom_id) {
  jello_value v;
  return vm_object_find(o, atom_id, 0, &v);
}

jello_value jello_object_get(const jello_object* o, uint32_t atom_id) {
  jello_value v = jello_make_null();
  (void)vm_object_find(o, atom_id, 1, &v);
  return v;
}

uint8_t vm_object_state(const jello_object* o, uint32_t atom_id) {
  int found = 0;
  uint32_t i = find_occupied_slot(o, atom_id, &found);
  return found ? o->states[i] : (uint8_t)JELLO_OBJ_SLOT_EMPTY;
}

/** Sets `atom_id` to `v` in a slot of `state`. */
static void object_put(jello_object* o, uint32_t atom_id, jello_value v, uint8_t state) {
  ensure_cap(o);
  int found = 0;
  uint32_t i = find_slot(o, atom_id, &found);
  if(!found) {
    o->keys[i] = atom_id;
    o->len++;
  }
  o->sealed += (uint32_t)(state == JELLO_OBJ_SLOT_SEALED) -
               (uint32_t)(found && o->states[i] == JELLO_OBJ_SLOT_SEALED);
  o->states[i] = state;
  o->vals[i] = v;
}

void jello_object_set(jello_object* o, uint32_t atom_id, jello_value v) {
  object_put(o, atom_id, v, JELLO_OBJ_SLOT_OCCUPIED);
}

void jello_object_seal(jello_object* o, uint32_t atom_id, jello_value v) {
  object_put(o, atom_id, v, JELLO_OBJ_SLOT_SEALED);
}

void vm_object_set_f64(jello_vm* vm, jello_object* o, uint32_t atom_id, double d) {
  int found = 0;
  uint32_t i = find_occupied_slot(o, atom_id, &found);
  if(found && o->states[i] == JELLO_OBJ_SLOT_OWNED) {
    ((jello_box_f64*)jello_as_ptr(o->vals[i]))->value = d;
    return;
  }
  /* A collection neither moves objects nor touches tables, so slot `i` survives the allocation;
   * nothing allocates between the box and its store, so the box needs no root. */
  jello_value b = jello_from_ptr(jello_box_f64_new(vm, d));
  if(!found) {
    object_put(o, atom_id, b, JELLO_OBJ_SLOT_OWNED);
    return;
  }
  o->sealed -= o->states[i] == JELLO_OBJ_SLOT_SEALED;
  o->vals[i] = b;
  o->states[i] = JELLO_OBJ_SLOT_OWNED;
}

int jello_object_remove(jello_object* o, uint32_t atom_id) {
  int found = 0;
  uint32_t i = find_slot(o, atom_id, &found);
  if(!found) return 0;
  o->sealed -= o->states[i] == JELLO_OBJ_SLOT_SEALED;
  o->states[i] = JELLO_OBJ_SLOT_TOMB;
  o->len--;
  o->tombstones++;
  return 1;
}

void jello_object_clear(jello_object* o) {
  memset(o->states, 0, o->cap);
  o->len = 0;
  o->tombstones = 0;
  o->sealed = 0;
}

/** A copy of `src` referencing what it does; when `shared`, `src` gives up its boxes, which both
 * now reference, so neither may write into them, and the copy's slots are unsealed, as a new
 * Object's are, since no call is compiled against it; otherwise the copy, the same Object in
 * another process, keeps its seals. */
static jello_object* copy_object(jello_vm* vm, const jello_object* src, int shared) {
  jello_object* dst = jello_object_new(vm, src->h.type_id);
  for(uint32_t i = 0; i < src->cap; i++) {
    if(!vm_obj_slot_live(src->states[i])) continue;
    if(shared && src->states[i] == JELLO_OBJ_SLOT_OWNED) src->states[i] = JELLO_OBJ_SLOT_OCCUPIED;
    int sealed = !shared && src->states[i] == JELLO_OBJ_SLOT_SEALED;
    object_put(dst, src->keys[i], src->vals[i],
               sealed ? JELLO_OBJ_SLOT_SEALED : JELLO_OBJ_SLOT_OCCUPIED);
  }
  dst->proto = src->proto;
  return dst;
}

jello_object* jello_object_copy(jello_vm* vm, const jello_object* src) {
  return copy_object(vm, src, 1);
}

jello_object* vm_object_clone(jello_vm* vm, const jello_object* src) {
  return copy_object(vm, src, 0);
}

static int atom_order(const void* a, const void* b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

uint32_t jello_object_keys(const jello_object* o, int skip_proto, uint32_t* out) {
  uint32_t n = 0;
  for(uint32_t i = 0; i < o->cap; i++) {
    if(!vm_obj_slot_live(o->states[i])) continue;
    if(skip_proto && o->keys[i] == 0u) continue;
    out[n++] = o->keys[i];
  }
  qsort(out, n, sizeof *out, atom_order);
  return n;
}
