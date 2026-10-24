/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Jahred Love. All rights reserved. */

/* The compiler module jelloc embeds (jelloc.c); the file JELLOC_IMAGE names,
 * a string the build defines. */
#ifdef __APPLE__
#define SYMBOL(name) _##name
  .section __TEXT,__const
#elif defined(_WIN32)
#define SYMBOL(name) name
  .section .rdata,"dr"
#else
#define SYMBOL(name) name
  .section .rodata
#endif
  .globl SYMBOL(jello_jelloc_image)
  .globl SYMBOL(jello_jelloc_image_end)
  .p2align 3
SYMBOL(jello_jelloc_image):
  .incbin JELLOC_IMAGE
SYMBOL(jello_jelloc_image_end):
#if !defined(__APPLE__) && !defined(_WIN32)
  .section .note.GNU-stack,"",%progbits
#endif
