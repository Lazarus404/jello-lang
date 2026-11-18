// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static void test_heap_seed_and_object_identities(void) {
  jello_vm* first = jello_vm_create();
  jello_vm* joined = jello_vm_create_joined(first);
  jello_vm* separate = jello_vm_create();
  assert(first && joined && separate);

  assert(first->heap->hash_key[0] == joined->heap->hash_key[0]);
  assert(first->heap->hash_key[1] == joined->heap->hash_key[1]);
  assert(first->heap->hash_key[0] != separate->heap->hash_key[0]
         || first->heap->hash_key[1] != separate->heap->hash_key[1]);

  jello_object* first_object = jello_object_new(first, 0);
  jello_object* second_object = jello_object_new(first, 0);
  jello_object* copied_object = jello_object_copy(first, first_object);
  assert(first_object->identity != second_object->identity);
  assert(first_object->identity != copied_object->identity);
  assert(first_object->identity < second_object->identity);

  jello_vm_destroy(separate);
  jello_vm_destroy(joined);
  jello_vm_destroy(first);
}

static void test_parcel_preserves_object_and_function_identities(void) {
  jello_vm* sender = jello_vm_create();
  jello_vm* receiver = jello_vm_create_joined(sender);
  assert(sender && receiver);

  jello_object* object = jello_object_new(sender, 0);
  vm_parcel parcel;
  assert(vm_parcel_pack(sender, jello_from_ptr(object), 0, &parcel) == JELLO_TRAP_NONE);
  jello_object* received = (jello_object*)jello_as_ptr(vm_parcel_take(receiver, &parcel));
  assert(object != received);
  assert(object->identity == received->identity);

  jello_bc_function functions[1] = {{0}};
  jello_bc_module module = {0};
  module.funcs = functions;
  module.nfuncs = 1;
  sender->running_module = &module;
  jello_function* first = jello_function_new(sender, 0, 0);
  jello_function* second = jello_function_new(sender, 0, 0);
  assert(first->identity != second->identity);
  assert(vm_parcel_pack(sender, jello_from_ptr(first), 0, &parcel) == JELLO_TRAP_NONE);
  jello_function* copied = (jello_function*)jello_as_ptr(vm_parcel_take(receiver, &parcel));
  assert(first != copied);
  assert(first->identity == copied->identity);

  jello_vm_destroy(receiver);
  jello_vm_destroy(sender);
}

int main(void) {
  test_heap_seed_and_object_identities();
  test_parcel_preserves_object_and_function_identities();
  puts("identity_tests: passed");
  return 0;
}
