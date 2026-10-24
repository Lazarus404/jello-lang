// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The garbage collector; a non-moving mark-and-sweep collector over the spaces of a heap shared
 * by threads, stopping the world only for the heap's own space, with size-class free lists, plus
 * the constructors for Bytes, numeric boxes, arrays, functions, structs and native handles. */

#include <jello/internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/jdll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

static const uint32_t GC_SIZE_CLASSES[GC_NUM_SIZE_CLASSES] = {
    32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

static jello_gc_hdr* hdr_of(void* obj) {
  return (jello_gc_hdr*)((uint8_t*)obj - sizeof(jello_gc_hdr));
}
static void* obj_of(jello_gc_hdr* h) { return (void*)((uint8_t*)h + sizeof(jello_gc_hdr)); }

static int size_class_index(uint32_t total) {
  for(int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
    if(total <= GC_SIZE_CLASSES[i]) return i;
  }
  return -1;
}

static void destroy_object(jello_space* space, void* p);

/** What comes before a shared binary in its block: the Bytes holding it, then the allocator's
 * header, as for any object. */
typedef struct shared_head {
  jdl_refs refs;
  jello_gc_hdr hdr;
} shared_head;

static _Atomic uint32_t shared_live;

uint32_t vm_shared_binaries_live(void) {
  return atomic_load_explicit(&shared_live, memory_order_relaxed);
}

/** The bytes a shared binary of `length` bytes takes, its block's headers included. */
static size_t shared_size(uint32_t length) {
  return sizeof(shared_head) + sizeof(jello_bytes) + (size_t)length;
}

static shared_head* shared_head_of(jello_bytes* bin) {
  return (shared_head*)((uint8_t*)hdr_of(bin) - offsetof(shared_head, hdr));
}

/** What a Bytes holding shared binary `bin` counts live for it: the binary's whole block. */
static uint32_t shared_native(const jello_bytes* bin) {
  size_t n = shared_size(bin->length);
  return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/** Drops a reference to shared binary `bin`, freeing it with the last. One that sees itself the
 * only holder frees it without the atomic update, as nobody else can take a reference. */
static void shared_release(jello_bytes* bin) {
  shared_head* s = shared_head_of(bin);
  if(__atomic_load_n(&s->refs, __ATOMIC_ACQUIRE) != 1u && !jdl_release(s)) return;
  atomic_fetch_sub_explicit(&shared_live, 1u, memory_order_relaxed);
  free(s);
}

/** What object `oh` counts live beyond its block: a handle's native bytes, or the shared binary a
 * Bytes holds. */
static uint32_t native_of(const jello_obj_header* oh) {
  if(oh->kind == (uint8_t)JELLO_OBJ_HANDLE) return ((const jello_handle*)oh)->native_bytes;
  const jello_bytes* b = (const jello_bytes*)oh;
  return oh->kind == (uint8_t)JELLO_OBJ_BYTES && b->shared ? shared_native(b->owner) : 0;
}

/** Live bytes before the first collection, and the least the threshold ever falls to. */
#define GC_MIN_THRESHOLD (4u * 1024u * 1024u)
/** The same for a process's space, smaller, as there may be many. */
#define GC_PROCESS_MIN_THRESHOLD (64u * 1024u)

/** The least `vm`'s space's threshold ever falls to. */
static uint32_t min_threshold(const jello_vm* vm) {
  return vm_owns_space(vm) ? GC_PROCESS_MIN_THRESHOLD : GC_MIN_THRESHOLD;
}

void jello_gc_init(jello_vm* vm) {
  if(!vm || !vm->space) return;
  if(vm->space->next_collect == 0) vm->space->next_collect = min_threshold(vm);
}

/** Frees the objects of list `h`, releasing what each owns beyond its block, as a sweep would,
 * counted in `space` unless it is NULL. */
static void free_objects(jello_space* space, jello_gc_hdr* h) {
  while(h) {
    jello_gc_hdr* next = h->next;
    destroy_object(space, obj_of(h));
    free(h);
    h = next;
  }
}

void jello_gc_shutdown(jello_vm* vm) {
  if(vm && vm->space) vm_space_free(vm->space);
}

void vm_space_free(jello_space* space) {
  free_objects(space, space->objects);
  space->objects = NULL;
  space->bytes_live = 0;
  for(int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
    jello_gc_hdr* fh = space->free_classes[i];
    while(fh) {
      jello_gc_hdr* next = fh->next;
      free(fh);
      fh = next;
    }
    space->free_classes[i] = NULL;
  }
}

/* A VM's roots are its own, changed only while it runs, so a collection, which waits for every
 * running VM, never sees them change. */
void jello_gc_push_root(jello_vm* vm, void* obj) {
  if(!vm) abort();
  if(vm->gc_roots_len == vm->gc_roots_cap) {
    uint32_t ncap = vm->gc_roots_cap ? vm->gc_roots_cap * 2u : 32u;
    void** nv = (void**)realloc(vm->gc_roots, sizeof(void*) * (size_t)ncap);
    if(!nv) abort();
    vm->gc_roots = nv;
    vm->gc_roots_cap = ncap;
  }
  vm->gc_roots[vm->gc_roots_len++] = obj;
}

void jello_gc_pop_roots(jello_vm* vm, uint32_t n) {
  if(!vm) abort();
  if(n > vm->gc_roots_len) {
    fprintf(stderr, "DIAG: gc_pop_roots underflow n=%u len=%u\n", n, vm->gc_roots_len);
    fflush(stderr);
    abort();
  }
  vm->gc_roots_len -= n;
}

void jello_vm_gc_stats(const jello_vm* vm, uint64_t* out_live_bytes, uint32_t* out_collections,
                       double* out_pause_ms) {
  if(!vm || !vm->space) return;
  if(out_live_bytes) *out_live_bytes = vm->space->bytes_live;
  if(out_collections) *out_collections = vm->space->collections;
  if(out_pause_ms) *out_pause_ms = vm->space->pause_ms;
}

/** Ends the program, saying why, when a block of `size` bytes cannot be had: the system has no
 * memory left for it, or it is past the 4 GiB a block may take. */
static void out_of_memory(size_t size) {
  fprintf(stderr, "jello: out of memory: cannot allocate %zu bytes\n", size);
  abort();
}

/** A block `space`'s collections freed, of size class `ci`, taken off its free list; NULL when
 * there is none or `ci` is -1, for a block of no class. */
static jello_gc_hdr* free_class_pop(jello_space* space, int ci) {
  jello_gc_hdr* h = ci >= 0 ? space->free_classes[ci] : NULL;
  if(h) space->free_classes[ci] = h->next;
  return h;
}

/** A block of `total` bytes, of size class `ci`, in the parcel `vm` packs, counting `native` more
 * live with it; one its own space's collections freed, when only `vm` touches that space. Nothing
 * else reaches a parcel while it is packed, so this takes no lock, and is no safepoint. */
static void* parcel_alloc(jello_vm* vm, int ci, uint32_t total, uint32_t native) {
  vm_parcel* pc = vm->parcel;
  jello_gc_hdr* h = vm_owns_space(vm) ? free_class_pop(vm->space, ci) : NULL;
  if(!h) {
    h = (jello_gc_hdr*)malloc(total);
    if(!h) out_of_memory(total);
  }
  h->total_size = total;
  h->marked = 0;
  h->next = pc->objects;
  if(!pc->objects) pc->tail = h;
  pc->objects = h;
  pc->bytes += (uint64_t)total + native;
  return obj_of(h);
}

/** Takes the heap's lock for `vm` when `shared`, the space it touches being the heap's, which VMs
 * on other threads share. A process's own space needs none: only one thread at a time ever
 * touches it, its spawner before it starts, then whichever scheduler runs it. */
static void space_lock(jello_vm* vm, int shared) {
  if(shared) vm_heap_lock(vm);
}

/** Releases what space_lock took. */
static void space_unlock(jello_vm* vm, int shared) {
  if(shared) pthread_mutex_unlock(&vm->heap->alloc_lock);
}

/** Allocates `size` bytes, counting `native` more bytes live with them, as a handle's payload;
 * into `vm`'s parcel while it packs one. */
static void* alloc(jello_vm* vm, size_t size, uint32_t native) {
  if(!vm) abort();
  if(size > (size_t)UINT32_MAX - sizeof(jello_gc_hdr)) out_of_memory(size);
  jello_heap* heap = vm->heap;
  jello_space* space = vm->space;
  uint32_t requested = (uint32_t)(sizeof(jello_gc_hdr) + size);

  int ci = size_class_index(requested);
  uint32_t total = (ci >= 0) ? GC_SIZE_CLASSES[ci] : requested;
  if(vm->parcel) return parcel_alloc(vm, ci, total, native);
  if(atomic_load(&heap->stop_requested)) vm_safepoint_check(vm);

  /* The lock is held only around bookkeeping, never across jello_gc_collect nor malloc. */
  int shared = !vm_owns_space(vm);
  space_lock(vm, shared);
  if(space->next_collect && space->bytes_live >= space->next_collect) {
    space_unlock(vm, shared);
    jello_gc_collect(vm);
    space_lock(vm, shared);
  }
  jello_gc_hdr* h = free_class_pop(space, ci);
  if(!h) {
    space_unlock(vm, shared);
    h = (jello_gc_hdr*)malloc(total);
    if(!h) out_of_memory(total);
    space_lock(vm, shared);
  }
  h->total_size = total;
  h->marked = 0;
  /* A thread outside a run, which no stop of the world waits for, may still be filling in a block
   * another thread's sweep reaches, perhaps one that held a reference; until then it owns nothing.
   */
  ((jello_obj_header*)obj_of(h))->kind = 0;
  h->next = space->objects;
  space->objects = h;
  space->bytes_live += (uint64_t)total + native;
  space_unlock(vm, shared);

  return obj_of(h);
}

void vm_parcel_splice(jello_vm* vm, jello_space* space, vm_parcel* pc) {
  if(!pc->objects) return;
  int shared = vm_space_shared(vm->heap, space);
  space_lock(vm, shared);
  pc->tail->next = space->objects;
  space->objects = pc->objects;
  space->bytes_live += pc->bytes;
  space_unlock(vm, shared);
  pc->objects = pc->tail = NULL;
  pc->bytes = 0;
}

void vm_parcel_join(vm_parcel* into, vm_parcel* from) {
  if(from->objects) {
    from->tail->next = into->objects;
    if(!into->objects) into->tail = from->tail;
    into->objects = from->objects;
    into->bytes += from->bytes;
  }
  if(from->sockets) proc_sockets_join(&into->sockets, &from->sockets);
  free(from->protos);
  *from = vm_parcel_empty();
}

void vm_parcel_free(vm_parcel* pc) {
  if(pc->sockets) proc_sockets_take(NULL, &pc->sockets);
  free_objects(NULL, pc->objects);
  free(pc->protos);
  *pc = vm_parcel_empty();
}

void* jello_gc_alloc(jello_vm* vm, size_t size) { return alloc(vm, size, 0); }

typedef struct mark_stack {
  void** items;
  uint32_t len;
  uint32_t cap;
  uint32_t marked; /**< The objects it has marked, every one of which the sweep must find. */
} mark_stack;

static void ms_push(mark_stack* ms, void* p) {
  if(ms->len == ms->cap) {
    uint32_t ncap = ms->cap ? ms->cap * 2u : 64u;
    void** ni = (void**)realloc(ms->items, sizeof(void*) * (size_t)ncap);
    if(!ni) abort();
    ms->items = ni;
    ms->cap = ncap;
  }
  ms->items[ms->len++] = p;
}

static void* ms_pop(mark_stack* ms) {
  if(ms->len == 0) return NULL;
  return ms->items[--ms->len];
}

static void mark_ptr(mark_stack* ms, void* p) {
  if(!p) return;
  jello_gc_hdr* h = hdr_of(p);
  if(h->marked) return;
  h->marked = 1;
  ms->marked++;
  ms_push(ms, p);
}

/** Marks every object that the live object `p` references. Struct, array and capture slots are
 * read by their declared kinds, resolved through `m`; a slice references its owner. */
static void trace_object(mark_stack* ms, const jello_bc_module* m, void* p) {
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_BYTES: mark_ptr(ms, ((const jello_bytes*)p)->owner); return;
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16: return;
    case JELLO_OBJ_STRUCT: {
      const jello_struct* s = (const jello_struct*)p;
      const jello_struct_sig* sig = &m->struct_sigs[oh->type_id];
      for(uint16_t i = 0; i < sig->nfields; i++) {
        jello_type_kind fk = m->types[sig->field_type[i]].kind;
        if(fk == JELLO_T_DYNAMIC) {
          jello_value v;
          memcpy(&v, s->fields + sig->field_offset[i], sizeof(jello_value));
          if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
        } else if(vm_kind_is_gc_pointer(fk)) {
          void* fp;
          memcpy(&fp, s->fields + sig->field_offset[i], sizeof(void*));
          mark_ptr(ms, fp);
        }
      }
      return;
    }
    case JELLO_OBJ_OBJECT: {
      const jello_object* o = (const jello_object*)p;
      if(o->proto) mark_ptr(ms, o->proto);
      for(uint32_t i = 0; i < o->cap; i++) {
        if(!vm_obj_slot_live(o->states[i])) continue;
        if(jello_is_ptr(o->vals[i])) mark_ptr(ms, jello_as_ptr(o->vals[i]));
      }
      return;
    }
    case JELLO_OBJ_ARRAY: {
      const jello_array* a = (const jello_array*)p;
      jello_type_id elem_tid = m->types[a->h.type_id].as.unary.elem;
      jello_type_kind ek = m->types[elem_tid].kind;
      if(ek == JELLO_T_DYNAMIC) {
        const jello_value* vals = (const jello_value*)a->data;
        for(uint32_t i = 0; i < a->length; i++) {
          if(jello_is_ptr(vals[i])) mark_ptr(ms, jello_as_ptr(vals[i]));
        }
      } else if(vm_kind_is_gc_pointer(ek)) {
        void* const* ptrs = (void* const*)a->data;
        for(uint32_t i = 0; i < a->length; i++) { mark_ptr(ms, ptrs[i]); }
      }
      return;
    }
    case JELLO_OBJ_LIST: {
      const jello_list* l = (const jello_list*)p;
      if(jello_is_ptr(l->head)) mark_ptr(ms, jello_as_ptr(l->head));
      if(l->tail) mark_ptr(ms, l->tail);
      return;
    }
    case JELLO_OBJ_ENUM: {
      const jello_enum* e = (const jello_enum*)p;
      for(uint32_t i = 0; i < e->nfields; i++) {
        if(jello_is_ptr(e->fields[i])) mark_ptr(ms, jello_as_ptr(e->fields[i]));
      }
      return;
    }
    /* Capture i has the kind of the function's register cap_start + i. */
    case JELLO_OBJ_FUNCTION: {
      const jello_function* fn = (const jello_function*)p;
      const jello_bc_function* callee = &m->funcs[fn->func_index];
      for(uint32_t i = 0; i < fn->ncaps; i++) {
        jello_type_kind ck = vm_reg_kind(m, callee, callee->cap_start + i);
        if(ck == JELLO_T_DYNAMIC) {
          jello_value v;
          memcpy(&v, &fn->captures[i], sizeof(jello_value));
          if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
        } else if(vm_kind_is_gc_pointer(ck)) {
          void* fp;
          memcpy(&fp, &fn->captures[i], sizeof(void*));
          mark_ptr(ms, fp);
        }
      }
      return;
    }
    default: return;
  }
}

/** Releases what a dead object owns outside its block: an Object's table, an Array's data, a
 * Bytes's reference to a shared binary, or a native handle's payload through its class
 * finaliser, counted in `space` unless it is NULL, for a parcel's. During a collection of the
 * heap's space this runs with alloc_lock held, so a finaliser must not allocate; otherwise it runs
 * with no lock held, a scheduler's lock included. It reads no other object but one the dead object
 * holds a reference to, as another dead object it reaches may already be freed. */
static void destroy_object(jello_space* space, void* p) {
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_OBJECT: {
      jello_object* o = (jello_object*)p;
      free(o->keys); /* keys is the malloc base for the whole keys+vals+states block */
      return;
    }
    case JELLO_OBJ_ARRAY: {
      jello_array* a = (jello_array*)p;
      free(a->data);
      return;
    }
    case JELLO_OBJ_HANDLE: {
      jello_handle* hd = (jello_handle*)p;
      if(hd->cls->finalize) hd->cls->finalize(hd->payload);
      if(space) space->finalized_handles++;
      return;
    }
    case JELLO_OBJ_BYTES: {
      jello_bytes* b = (jello_bytes*)p;
      if(b->shared) shared_release(b->owner);
      return;
    }
    default: return;
  }
}

/** Marks the roots in `vm`'s call frames; every spill slot, and each register whose declared
 * kind holds a pointer. A Dynamic register is marked only when it holds a pointer. */
static void scan_typed_frames(jello_vm* vm, mark_stack* ms) {
  const jello_bc_module* m = vm->running_module;
  if(!m) return;
  for(uint32_t fi = 0; fi < vm->call_frames_len; fi++) {
    call_frame* fr = &vm->call_frames[fi];
    /* Spill slots hold boxed values whatever the registers' kinds; unwritten ones are null. */
    for(uint32_t s = 0; s < fr->spill_cap; s++) {
      jello_value v = fr->spill_slots[s];
      if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
    }
    if(!fr->has_pointer) continue;
    const jello_bc_function* f = fr->f;
    reg_frame* rf = &fr->rf;
    for(uint32_t r = 0; r < f->nregs; r++) {
      jello_type_kind k = vm_reg_kind_fast(rf, r);
      if(vm_kind_is_gc_pointer(k)) {
        void* p = NULL;
        memcpy(&p, vm_reg_ptr(rf, r), sizeof(void*));
        mark_ptr(ms, p);
      } else if(k == JELLO_T_DYNAMIC) {
        jello_value v;
        memcpy(&v, vm_reg_ptr(rf, r), sizeof(jello_value));
        if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
      }
    }
  }
}

/** Marks the roots in `globals`, a table of `m`'s globals, by their declared kinds. */
static void scan_globals(const jello_bc_module* m, const uint64_t* globals, mark_stack* ms) {
  for(uint32_t j = 0; j < m->nptr_globals; j++) {
    mark_ptr(ms, vm_global_ptr(m, globals, m->ptr_globals[j]));
  }
}

/** Marks the roots of `t`, a VM allocating in the space collected: its own globals, when it has
 * them, laid out as `m`'s, its frames, its pushed roots and its process's. */
static void scan_vm(jello_vm* t, const jello_bc_module* m, mark_stack* ms) {
  if(m && t->owns_globals) scan_globals(m, t->globals, ms);
  mark_ptr(ms, t->gc_transient_root);
  for(uint32_t j = 0; j < t->gc_roots_len; j++) mark_ptr(ms, t->gc_roots[j]);
  scan_typed_frames(t, ms);
  const jello_process* p = t->proc;
  for(uint32_t j = 0; p && j < p->ncleanups; j++) mark_ptr(ms, jello_as_ptr(p->cleanups[j]));
  if(p && jello_is_ptr(p->exit_value)) mark_ptr(ms, jello_as_ptr(p->exit_value));
}

void jello_gc_collect(jello_vm* vm) {
  if(!vm) abort();
  jello_heap* heap = vm->heap;
  jello_space* space = vm->space;
  /* Globals and object layouts are resolved against this VM's module; every VM on a heap must
   * run it. A process's own space is collected with nothing stopped, as space_lock says; the
   * heap's once every other thread is quiescent or idle and alloc_lock is held. */
  int own = vm_owns_space(vm);
  if(!own) {
    /* Read while this VM runs, so no collection, which waits for it, changes it meanwhile. */
    uint32_t seen = space->collections;
    while(!vm_heap_stop_the_world(vm)) {
      if(space->collections != seen) return; /* Another thread collected this space meanwhile. */
    }
  }

  space->collections++;
  atomic_fetch_add_explicit(&heap->collections, 1u, memory_order_relaxed);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  mark_stack ms = {0};

  /* The roots of the VMs allocating in this space only. No object of another space is reachable
   * from them, so no other space's objects are marked, and every mark is one the sweep clears. */
  const jello_bc_module* m = vm->running_module;
  if(own) {
    scan_vm(vm, m, &ms);
  } else {
    int module_globals = 0;
    for(uint32_t i = 0; i < heap->threads_len; i++) {
      jello_vm* t = heap->threads[i];
      if(vm_owns_space(t)) continue;
      module_globals |= !t->owns_globals;
      scan_vm(t, m, &ms);
    }
    if(m && module_globals) scan_globals(m, m->globals, &ms);
  }

  for(;;) {
    void* p = ms_pop(&ms);
    if(!p) break;
    trace_object(&ms, vm->running_module, p);
  }
  free(ms.items);

  jello_gc_hdr** prev = &space->objects;
  jello_gc_hdr* h = space->objects;
  uint64_t live_bytes = 0;
  while(h) {
    jello_gc_hdr* next = h->next;
    if(!h->marked) {
      void* obj = obj_of(h);
      space->freed_objects++;
      *prev = next;
      destroy_object(space, obj);
      int ci = size_class_index(h->total_size);
      if(ci >= 0 && h->total_size == GC_SIZE_CLASSES[ci]) {
        h->next = space->free_classes[ci];
        space->free_classes[ci] = h;
      } else {
        free(h);
      }
    } else {
      h->marked = 0;
      ms.marked--;
      live_bytes += (uint64_t)h->total_size + native_of((const jello_obj_header*)obj_of(h));
      prev = &h->next;
    }
    h = next;
  }

  /* A mark the sweep did not clear fell outside this space, where its owner's collection would
   * take it for its own and skip what it references. */
  if(ms.marked) {
    fprintf(stderr, "jello: collection marked %u objects outside its space\n", ms.marked);
    abort();
  }
  space->bytes_live = live_bytes;
  uint64_t least = min_threshold(vm);
  space->next_collect = live_bytes < least / 2u ? least : live_bytes * 2u;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  space->pause_ms +=
      (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

  if(!own) vm_heap_resume_the_world(vm);
}

jello_handle* jello_handle_new(jello_vm* vm, const struct jello_jdll_handle_class* cls,
                               void* payload, uint32_t native_bytes) {
  jello_handle* hd = (jello_handle*)alloc(vm, sizeof(jello_handle), native_bytes);
  hd->h.kind = (uint8_t)JELLO_OBJ_HANDLE;
  hd->h.type_id = 0;
  hd->cls = cls;
  hd->payload = payload;
  hd->native_bytes = native_bytes;
  return hd;
}

/** Fills in binary `b`'s header and fields, for `length` bytes in its own storage. */
static jello_bytes* bytes_init(jello_bytes* b, uint32_t length) {
  b->h.kind = (uint8_t)JELLO_OBJ_BYTES;
  b->h.type_id = 0; /* Never written; vm_buffer_new marks a binary that may be. */
  b->length = length;
  b->shared = 0;
  b->data = b->storage;
  b->owner = NULL;
  return b;
}

/** A Bytes of the `length` bytes at `data`, which `owner` holds: a shared binary when `shared`,
 * whose reference the caller has taken for it, else a binary the new Bytes keeps alive. */
static jello_bytes* bytes_ref(jello_vm* vm, jello_bytes* owner, uint8_t* data, uint32_t length,
                              int shared) {
  void* block = alloc(vm, sizeof(jello_bytes), shared ? shared_native(owner) : 0);
  jello_bytes* b = bytes_init((jello_bytes*)block, length);
  b->data = data;
  b->owner = owner;
  b->shared = shared ? 1u : 0u;
  return b;
}

/* A large binary's bytes live in no space, so that a copy for another process can share them. */
jello_bytes* jello_bytes_new(jello_vm* vm, uint32_t length) {
  if(length < VM_BYTES_SHARED_MIN) {
    return bytes_init((jello_bytes*)jello_gc_alloc(vm, sizeof(jello_bytes) + (size_t)length),
                      length);
  }
  shared_head* s = (shared_head*)malloc(shared_size(length));
  if(!s) abort();
  s->refs = 1u;
  s->hdr.marked = GC_SHARED;
  atomic_fetch_add_explicit(&shared_live, 1u, memory_order_relaxed);
  jello_bytes* bin = bytes_init((jello_bytes*)obj_of(&s->hdr), length);
  return bytes_ref(vm, bin, bin->data, length, 1);
}

jello_bytes* jello_bytes_new_zeroed(jello_vm* vm, uint32_t length) {
  jello_bytes* b = jello_bytes_new(vm, length);
  if(length > 0) memset(b->data, 0, length);
  return b;
}

jello_bytes* vm_buffer_new(jello_vm* vm, uint32_t length) {
  jello_bytes* b = jello_bytes_new_zeroed(vm, length);
  b->h.type_id = JELLO_BYTES_BUFFER;
  return b;
}

jello_bytes* jello_bytes_join(jello_vm* vm, jello_bytes* const* parts, uint32_t n,
                              const jello_bytes* separator) {
  uint32_t gap = separator ? separator->length : 0;
  uint64_t total = n ? (uint64_t)gap * (n - 1) : 0;
  /* Stopping once it is too long keeps the sum from wrapping. */
  for(uint32_t i = 0; i < n && total <= (uint64_t)UINT32_MAX; i++) total += parts[i]->length;
  if(total > (uint64_t)UINT32_MAX) {
    vm->trap_code = JELLO_TRAP_BOUNDS;
    return NULL;
  }
  jello_bytes* dst = jello_bytes_new(vm, (uint32_t)total);
  uint32_t at = 0;
  for(uint32_t i = 0; i < n; i++) {
    if(i && gap) {
      memcpy(dst->data + at, separator->data, gap);
      at += gap;
    }
    if(parts[i]->length) memcpy(dst->data + at, parts[i]->data, parts[i]->length);
    at += parts[i]->length;
  }
  return dst;
}

/** A Bytes of the `length` bytes of `b` from `start`, of type `type_id`, in bytes of its own. */
static jello_bytes* bytes_dup(jello_vm* vm, const jello_bytes* b, uint32_t start, uint32_t length,
                              uint32_t type_id) {
  jello_bytes* n = jello_bytes_new(vm, length);
  n->h.type_id = type_id;
  if(length) memcpy(n->data, b->data + start, length);
  return n;
}

/* The collector never moves objects, so a slice may point into its owner: the binary holding its
 * bytes, or the Bytes in its space holding a reference to a shared one, so that slicing takes no
 * reference of its own. A buffer is copied instead, so that only a buffer is ever written and no
 * other Bytes sees it change. */
jello_bytes* jello_bytes_slice(jello_vm* vm, jello_bytes* src, uint32_t start, uint32_t length) {
  if(vm_bytes_is_buffer(src)) return bytes_dup(vm, src, start, length, 0);
  jello_bytes* owner = src->owner && !src->shared ? src->owner : src;
  /* The owner is reached through src. */
  return bytes_ref(vm, owner, src->data + start, length, 0);
}

jello_bytes* vm_bytes_clone(jello_vm* vm, const jello_bytes* b) {
  if(vm_bytes_is_buffer(b)) return bytes_dup(vm, b, 0, b->length, JELLO_BYTES_BUFFER);
  /* A slice's owner is the binary, or the Bytes holding the shared binary, it reaches. */
  jello_bytes* holder = b->shared ? (jello_bytes*)b : b->owner;
  if(b->length >= VM_BYTES_SHARED_MIN && holder) {
    if(holder->shared) {
      jdl_retain(shared_head_of(holder->owner));
      return bytes_ref(vm, holder->owner, b->data, b->length, 1);
    }
    if(vm_is_immortal(holder)) return bytes_ref(vm, holder, b->data, b->length, 0);
  }
  return bytes_dup(vm, b, 0, b->length, 0);
}

jello_box_i64* jello_box_i64_new(jello_vm* vm, int64_t v) {
  jello_box_i64* b = (jello_box_i64*)jello_gc_alloc(vm, sizeof(jello_box_i64));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_I64;
  b->h.type_id = 0;
  b->value = v;
  return b;
}
jello_box_f64* jello_box_f64_new(jello_vm* vm, double v) {
  jello_box_f64* b = (jello_box_f64*)jello_gc_alloc(vm, sizeof(jello_box_f64));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_F64;
  b->h.type_id = 0;
  b->value = v;
  return b;
}
jello_box_f32* jello_box_f32_new(jello_vm* vm, float v) {
  jello_box_f32* b = (jello_box_f32*)jello_gc_alloc(vm, sizeof(jello_box_f32));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_F32;
  b->h.type_id = 0;
  b->value = v;
  return b;
}
jello_box_f16* jello_box_f16_new(jello_vm* vm, uint16_t bits) {
  jello_box_f16* b = (jello_box_f16*)jello_gc_alloc(vm, sizeof(jello_box_f16));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_F16;
  b->h.type_id = 0;
  b->value = bits;
  return b;
}

jello_value vm_clone_numbox(jello_vm* vm, jello_value v) {
  if(!vm || !jello_is_ptr(v) || jello_as_ptr(v) == NULL) return v;
  switch(((const jello_obj_header*)jello_as_ptr(v))->kind) {
    case JELLO_OBJ_BOX_F64:
      return jello_from_ptr(jello_box_f64_new(vm, ((const jello_box_f64*)jello_as_ptr(v))->value));
    case JELLO_OBJ_BOX_F32:
      return jello_from_ptr(jello_box_f32_new(vm, ((const jello_box_f32*)jello_as_ptr(v))->value));
    case JELLO_OBJ_BOX_F16:
      return jello_from_ptr(jello_box_f16_new(vm, ((const jello_box_f16*)jello_as_ptr(v))->value));
    case JELLO_OBJ_BOX_I64:
      return jello_from_ptr(jello_box_i64_new(vm, ((const jello_box_i64*)jello_as_ptr(v))->value));
    default: return v;
  }
}

static uint32_t array_elem_size(const jello_bc_module* m, uint32_t array_type_id) {
  jello_type_id elem_tid = m->types[array_type_id].as.unary.elem;
  return (uint32_t)jello_slot_size(m->types[elem_tid].kind);
}

jello_array* jello_array_new(jello_vm* vm, const jello_bc_module* m, uint32_t array_type_id,
                             uint32_t length) {
  jello_array* a = (jello_array*)jello_gc_alloc(vm, sizeof(jello_array));
  a->h.kind = (uint8_t)JELLO_OBJ_ARRAY;
  a->h.type_id = array_type_id;
  a->length = 0;
  a->cap = 0;
  a->data = NULL;
  jello_array_resize(vm, m, a, length);
  return a;
}

void jello_array_resize(jello_vm* vm, const jello_bc_module* m, jello_array* a,
                        uint32_t new_length) {
  (void)vm;
  uint32_t elem_size = array_elem_size(m, a->h.type_id);
  /* Capacity doubles from 8, so growing one element at a time is amortised O(1). */
  if(new_length > a->cap) {
    uint32_t new_cap = a->cap ? a->cap : 8u;
    while(new_cap < new_length) {
      if(new_cap > UINT32_MAX / 2u) {
        new_cap = new_length;
        break;
      }
      new_cap *= 2u;
    }
    if(elem_size != 0u && (size_t)new_cap > SIZE_MAX / (size_t)elem_size) abort();
    void* grown = realloc(a->data, (size_t)new_cap * (size_t)elem_size);
    if(!grown) abort();
    a->data = grown;
    a->cap = new_cap;
  }
  /* New elements are zero, or null for a Dynamic element. */
  if(new_length > a->length) {
    memset((uint8_t*)a->data + (size_t)a->length * elem_size, 0,
           (size_t)(new_length - a->length) * elem_size);
    if(m->types[m->types[a->h.type_id].as.unary.elem].kind == JELLO_T_DYNAMIC) {
      jello_value* vals = (jello_value*)a->data;
      for(uint32_t i = a->length; i < new_length; i++) vals[i] = jello_make_null();
    }
  }
  a->length = new_length;
}

/** Fills in function `fn`'s header and fields, its `ncaps` captures zero. */
static jello_function* function_init(jello_function* fn, uint32_t func_index, uint32_t ncaps,
                                     jello_type_id type) {
  fn->h.kind = (uint8_t)JELLO_OBJ_FUNCTION;
  fn->h.type_id = type;
  fn->func_index = func_index;
  fn->ncaps = ncaps;
  for(uint32_t i = 0; i < ncaps; i++) fn->captures[i] = 0;
  return fn;
}

jello_function* jello_function_new(jello_vm* vm, uint32_t func_index, uint32_t ncaps) {
  /* Only a size_t no wider than the count can overflow. */
#if SIZE_MAX <= UINT32_MAX
  if((size_t)ncaps > (SIZE_MAX - sizeof(jello_function)) / sizeof(uint64_t)) abort();
#endif
  void* p = jello_gc_alloc(vm, sizeof(jello_function) + (size_t)ncaps * sizeof(uint64_t));
  /* CONST_FUN and CLOSURE set the Function type they store it as. */
  return function_init((jello_function*)p, func_index, ncaps, 0);
}

void* vm_immortal_alloc(size_t size) {
  if(size > SIZE_MAX - sizeof(jello_gc_hdr)) abort();
  jello_gc_hdr* h = (jello_gc_hdr*)calloc(1, sizeof(jello_gc_hdr) + size);
  if(!h) abort();
  h->marked = GC_IMMORTAL;
  return obj_of(h);
}

jello_function* vm_function_immortal(uint32_t func_index, jello_type_id type) {
  return function_init((jello_function*)vm_immortal_alloc(sizeof(jello_function)), func_index, 0,
                       type);
}

size_t vm_bytes_immortal_size(uint32_t length) {
  return (sizeof(jello_gc_hdr) + sizeof(jello_bytes) + (size_t)length + 7u) & ~(size_t)7u;
}

jello_bytes* vm_bytes_immortal_at(void* at, uint32_t length) {
  jello_gc_hdr* h = (jello_gc_hdr*)at;
  h->marked = GC_IMMORTAL;
  return bytes_init((jello_bytes*)obj_of(h), length);
}

void vm_immortal_free(void* p) {
  if(p) free(hdr_of(p));
}

uint32_t vm_struct_fields_bytes(const jello_bc_module* m, uint32_t struct_sig_id) {
  const jello_struct_sig* sig = &m->struct_sigs[struct_sig_id];
  if(sig->nfields == 0) return 0;
  uint32_t last = sig->nfields - 1u;
  jello_type_kind lk = m->types[sig->field_type[last]].kind;
  return (sig->field_offset[last] + (uint32_t)jello_slot_size(lk) + 7u) & ~7u;
}

jello_struct* jello_struct_new(jello_vm* vm, const jello_bc_module* m, uint32_t struct_sig_id) {
  const jello_struct_sig* sig = &m->struct_sigs[struct_sig_id];
  uint32_t total = vm_struct_fields_bytes(m, struct_sig_id);
  jello_struct* s = (jello_struct*)jello_gc_alloc(vm, sizeof(jello_struct) + (size_t)total);
  s->h.kind = (uint8_t)JELLO_OBJ_STRUCT;
  s->h.type_id = struct_sig_id;
  /* Fields start zeroed, and a Dynamic field null, whose bits are not zero. */
  if(total > 0) memset(s->fields, 0, total);
  for(uint16_t i = 0; i < sig->nfields; i++) {
    if(m->types[sig->field_type[i]].kind == JELLO_T_DYNAMIC) {
      jello_value nullv = jello_make_null();
      memcpy(s->fields + sig->field_offset[i], &nullv, sizeof(jello_value));
    }
  }
  return s;
}
