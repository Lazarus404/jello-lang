// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Object natives the compiler lowers to; the checks of `as P` and `is P` for a prototype P, and
 * the sealing of a linked prototype's methods. */

#include "std.h"

/** Returns the Object `v` points at, or NULL. */
static const jello_object* object_of(jello_value v) {
  return std_is_heap(v, JELLO_OBJ_OBJECT) ? (const jello_object*)jello_as_ptr(v) : NULL;
}

/** Whether the first argument is the Object that the second is, or delegates to it through its
 * prototype links; false for anything else, and once the links pass about 1024 Objects. */
void std_object_delegates(jdlo_ctx* c) {
  const jello_object* o = object_of(jdl_arg_value(c, 0));
  const jello_object* proto = object_of(jdl_arg_value(c, 1));
  uint32_t steps = 0u;
  while(o && proto && o != proto && steps++ < 1024u) o = o->proto;
  jdl_return_bool(c, o && o == proto);
}

/** Sets the first argument's key, the atom that is the second, to the third and seals it
 * (jello_object_seal); false, setting nothing, when the first is no Object or the second no atom.
 */
void std_object_seal(jdlo_ctx* c) {
  jello_value o = jdl_arg_value(c, 0);
  jello_value key = jdl_arg_value(c, 1);
  int sealed = std_is_heap(o, JELLO_OBJ_OBJECT) && jello_is_atom(key);
  if(sealed)
    jdl_object_seal((jello_object*)jello_as_ptr(o), jello_as_atom(key), jdl_arg_value(c, 2));
  jdl_return_bool(c, sealed);
}
