// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * image_run, which runs a module image as a program on a VM of its own and returns the program's
 * Bytes result; the compiler runs macro expanders this way, and itself to compile them. The run
 * is made on a thread of its own, as a scheduler's state is its thread's, and shares nothing with
 * the caller's VM but the process's native libraries. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** The tag of a run's result, of why it failed, and of why it failed when that is that it ran out
 * of fuel. */
enum { IMAGE_RUN_RESULT, IMAGE_RUN_FAILED, IMAGE_RUN_OUT_OF_FUEL };

/** The run's thread's stack; twice the 8 MiB of a main thread, for JIT code's deep recursion. */
#define IMAGE_RUN_STACK (16u << 20)

/** One run: what it is given, then what it gives back. */
typedef struct image_run {
  const uint8_t* image;
  size_t size;
  const char* const* argv;
  int32_t argc;
  uint64_t fuel;
  /** What image_run returns, malloc'd, or NULL when allocation failed: a tag byte (`IMAGE_RUN_*`),
   * then the bytes. */
  uint8_t* out;
  size_t out_len;
} image_run;

/** Makes `r`'s output the `n` bytes at `data`, after `tag`. */
static void give(image_run* r, int tag, const void* data, size_t n) {
  r->out = (uint8_t*)malloc(n + 1u);
  r->out_len = r->out ? n + 1u : 0u;
  if(!r->out) return;
  r->out[0] = (uint8_t)tag;
  memcpy(r->out + 1, data, n);
}

/** Makes `r`'s output the reason `vm`'s run of `m` trapped; what it threw, when that is Bytes, or
 * else the trap's name, where the line table places it, and why a native call failed. */
static void give_trap(image_run* r, jello_vm* vm, const jello_bc_module* m) {
  jello_trap_code tc = jello_vm_last_trap_code(vm);
  jello_value thrown = jello_vm_last_thrown(vm);
  jello_numeric_result shown = {.kind = JELLO_NUM_DYN, .as.dyn = thrown};
  const jello_bytes* text = tc == JELLO_TRAP_UNCAUGHT_THROW ? vm_result_bytes(&shown) : NULL;
  if(text) {
    give(r, IMAGE_RUN_FAILED, text->data, text->length);
    return;
  }
  char why[512];
  const char* file = NULL;
  uint32_t line = 0, col = 0;
  if(jello_bc_source_location(m, jello_vm_last_trap_func_index(vm), jello_vm_last_trap_pc(vm),
                              &file, &line, &col))
    snprintf(why, sizeof why, "%s:%u:%u: trap %s", file, line, col, jello_trap_code_name(tc));
  else
    snprintf(why, sizeof why, "trap %s in function %u at pc %u", jello_trap_code_name(tc),
             jello_vm_last_trap_func_index(vm), jello_vm_last_trap_pc(vm));
  const char* detail = jello_vm_last_trap_detail(vm);
  size_t used = strlen(why);
  if(*detail) snprintf(why + used, sizeof why - used, " (%s)", detail);
  give(r, tc == JELLO_TRAP_FUEL ? IMAGE_RUN_OUT_OF_FUEL : IMAGE_RUN_FAILED, why, strlen(why));
}

static void* run_main(void* arg) {
  image_run* r = (image_run*)arg;
  jello_bc_module* m = NULL;
  jello_bc_result loaded = jello_bc_read(r->image, r->size, &m);
  if(loaded.err) {
    char why[512];
    snprintf(why, sizeof why, "%s (offset %zu)", loaded.msg, loaded.offset);
    give(r, IMAGE_RUN_FAILED, why, strlen(why));
    return NULL;
  }
  jello_vm* vm = jello_vm_create();
  if(!vm) {
    give(r, IMAGE_RUN_FAILED, "out of memory", 13);
    jello_bc_free(m);
    return NULL;
  }
  jello_vm_set_program_args(vm, r->argc, r->argv);
  jello_vm_set_image(vm, r->image, r->size);
  if(r->fuel) jello_vm_set_fuel(vm, r->fuel);
  jello_numeric_result result;
  if(jello_vm_exec_numeric(vm, m, &result) != JELLO_EXEC_OK) {
    give_trap(r, vm, m);
  } else {
    const jello_bytes* b = vm_result_bytes(&result);
    static const char not_bytes[] = "the program's result is not Bytes";
    if(b) give(r, IMAGE_RUN_RESULT, b->data, b->length);
    else give(r, IMAGE_RUN_FAILED, not_bytes, sizeof not_bytes - 1);
  }
  jello_vm_destroy(vm);
  jello_bc_free(m);
  return NULL;
}

/** Splits `args`, NUL-terminated entries, into `r`'s arguments, pointing into `store`, a copy of
 * them; whether they are well formed and there was memory for them. */
static int split_args(image_run* r, const uint8_t* args, uint32_t len, char** store) {
  if(len && args[len - 1u]) return 0;
  int32_t n = 0;
  for(uint32_t i = 0; i < len; i++) n += !args[i];
  *store = (char*)malloc(len ? len : 1u);
  const char** argv = (const char**)malloc(sizeof(char*) * (size_t)(n ? n : 1));
  if(!*store || !argv) {
    free(argv);
    return 0;
  }
  if(len) memcpy(*store, args, len);
  for(uint32_t i = 0, k = 0; k < (uint32_t)n; k++) {
    argv[k] = *store + i;
    i += (uint32_t)strlen(argv[k]) + 1u;
  }
  r->argv = argv;
  r->argc = n;
  return 1;
}

/** Makes the run of image_run's call `c`, on a thread of its own, the caller waiting for it. */
static void image_run_blocking(jdlo_ctx* c) {
  image_run r = {0};
  const jello_heap* heap = c->vm->heap;
  uint32_t image_len = jdl_arg_bytes_len(c, 0);
  uint8_t* image = NULL;
  char* store = NULL;
  if(image_len) {
    image = (uint8_t*)malloc(image_len);
    if(image) memcpy(image, jdl_arg_bytes_data(c, 0), image_len);
    r.image = image;
    r.size = image_len;
  } else {
    r.image = heap->image;
    r.size = heap->image_size;
  }
  r.fuel = (uint64_t)jdl_arg_i64(c, 2);
  if(!r.image) {
    const char* why = image_len ? "out of memory" : "image_run: no image to run";
    give(&r, IMAGE_RUN_FAILED, why, strlen(why));
  } else if(!split_args(&r, jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1), &store)) {
    static const char bad[] = "image_run: arguments must be NUL-terminated entries";
    give(&r, IMAGE_RUN_FAILED, bad, sizeof bad - 1);
  } else {
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, IMAGE_RUN_STACK);
    if(pthread_create(&thread, &attr, run_main, &r) == 0) {
      pthread_join(thread, NULL);
    } else {
      static const char none[] = "image_run: cannot start a thread";
      give(&r, IMAGE_RUN_FAILED, none, sizeof none - 1);
    }
    pthread_attr_destroy(&attr);
  }
  free((void*)r.argv);
  free(store);
  free(image);
  if(r.out) jdl_return_bytes_copy(c, r.out, (uint32_t)r.out_len);
  else jdl_fail(c, "out of memory");
  free(r.out);
}

/** image_run(image, args, fuel): runs module image `image`, or with none the image the caller's
 * VM runs (jello_vm_set_image), as a program on a VM of its own, with `args`, NUL-terminated
 * entries, as its arguments, and with `fuel` (jello_vm_set_fuel) when it is not 0. Returns a tag
 * byte, then the program's Bytes result after 0, or why the run failed after 1, or after 2 when
 * it ran out of fuel; what it threw, when that is Bytes, or else its trap, or why its image would
 * not load. The caller's process parks meanwhile (proc_call_blocking). */
void vm_image_run(jdlo_ctx* c) {
  jello_trap_code stopped = proc_call_blocking(c->vm, image_run_blocking, c);
  if(stopped) {
    jdl_release_ret(c);
    proc_trap(c, stopped);
  }
}
