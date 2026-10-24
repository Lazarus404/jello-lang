/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Jahred Love. All rights reserved. */

/* Switching native stacks between processes, and the first frame of a new one.
 *
 * proc_stack_switch(void** save, void* load) pushes the callee-saved registers, stores the stack
 * pointer in *save, loads `load` as the stack pointer and pops the registers pushed there, so it
 * returns into whichever switch saved `load`. proc_stack_prepare lays out a new stack as if a
 * switch had saved it, returning into proc_stack_start, which calls the entry with its argument;
 * the entry never returns.
 *
 * On Windows a switch also swaps the thread's stack base, limit and deallocation base in its TEB,
 * which the system checks a stack against (as __chkstk's probes do), and the x64 one keeps the
 * Windows convention's callee-saved rdi, rsi and xmm6–xmm15, taking its arguments in rcx and rdx.
 *
 * Floating-point control (FPCR, MXCSR and the x87 control word) belongs to the thread, not the
 * process; nothing in Jello changes it. A switch cannot keep a shadow stack (x86 CET) consistent,
 * so this file carries no CET or BTI property note, and a binary linking it has neither. */

#ifdef __APPLE__
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

  .text

#if defined(_WIN32)
/* The TEB's NT_TIB stack base and limit, and its DeallocationStack. */
#define TEB_STACK_BASE 0x08
#define TEB_STACK_LIMIT 0x10
#define TEB_DEALLOCATION_STACK 0x1478
#endif

#if defined(_WIN32) && defined(__aarch64__)

/* x18 is the TEB, which nothing here changes. */
  .globl SYM(proc_stack_switch)
  .p2align 2
SYM(proc_stack_switch):
  sub sp, sp, #192
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  ldr x9, [x18, #TEB_STACK_BASE]
  ldr x10, [x18, #TEB_STACK_LIMIT]
  stp x9, x10, [sp, #160]
  ldr x9, [x18, #TEB_DEALLOCATION_STACK]
  str x9, [sp, #176]
  mov x9, sp
  str x9, [x0]
  mov sp, x1
  ldp x9, x10, [sp, #160]
  str x9, [x18, #TEB_STACK_BASE]
  str x10, [x18, #TEB_STACK_LIMIT]
  ldr x9, [sp, #176]
  str x9, [x18, #TEB_DEALLOCATION_STACK]
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  add sp, sp, #192
  ret

/* x19 holds the entry and x20 its argument, as proc_stack_prepare saved them. */
  .globl SYM(proc_stack_start)
  .p2align 2
SYM(proc_stack_start):
  mov x0, x20
  blr x19
  brk #0

#elif defined(_WIN32) && defined(__x86_64__)

  .globl SYM(proc_stack_switch)
  .p2align 4
SYM(proc_stack_switch):
  pushq %rbp
  pushq %rbx
  pushq %rdi
  pushq %rsi
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $184, %rsp
  movaps %xmm6, 0(%rsp)
  movaps %xmm7, 16(%rsp)
  movaps %xmm8, 32(%rsp)
  movaps %xmm9, 48(%rsp)
  movaps %xmm10, 64(%rsp)
  movaps %xmm11, 80(%rsp)
  movaps %xmm12, 96(%rsp)
  movaps %xmm13, 112(%rsp)
  movaps %xmm14, 128(%rsp)
  movaps %xmm15, 144(%rsp)
  movq %gs:TEB_STACK_BASE, %rax
  movq %rax, 160(%rsp)
  movq %gs:TEB_STACK_LIMIT, %rax
  movq %rax, 168(%rsp)
  movq %gs:TEB_DEALLOCATION_STACK, %rax
  movq %rax, 176(%rsp)
  movq %rsp, (%rcx)
  movq %rdx, %rsp
  movq 160(%rsp), %rax
  movq %rax, %gs:TEB_STACK_BASE
  movq 168(%rsp), %rax
  movq %rax, %gs:TEB_STACK_LIMIT
  movq 176(%rsp), %rax
  movq %rax, %gs:TEB_DEALLOCATION_STACK
  movaps 0(%rsp), %xmm6
  movaps 16(%rsp), %xmm7
  movaps 32(%rsp), %xmm8
  movaps 48(%rsp), %xmm9
  movaps 64(%rsp), %xmm10
  movaps 80(%rsp), %xmm11
  movaps 96(%rsp), %xmm12
  movaps 112(%rsp), %xmm13
  movaps 128(%rsp), %xmm14
  movaps 144(%rsp), %xmm15
  addq $184, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rsi
  popq %rdi
  popq %rbx
  popq %rbp
  ret

/* r12 holds the entry and r13 its argument, as proc_stack_prepare saved them; the call reserves
 * the convention's 32 bytes of shadow space. */
  .globl SYM(proc_stack_start)
  .p2align 4
SYM(proc_stack_start):
  movq %r13, %rcx
  subq $32, %rsp
  callq *%r12
  ud2

#elif defined(__aarch64__)

  .globl SYM(proc_stack_switch)
  .p2align 2
SYM(proc_stack_switch):
  sub sp, sp, #160
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  mov x9, sp
  str x9, [x0]
  mov sp, x1
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  add sp, sp, #160
  ret

/* x19 holds the entry and x20 its argument, as proc_stack_prepare saved them. */
  .globl SYM(proc_stack_start)
  .p2align 2
SYM(proc_stack_start):
  mov x0, x20
  blr x19
  brk #0

#elif defined(__x86_64__)

  .globl SYM(proc_stack_switch)
  .p2align 4
SYM(proc_stack_switch):
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  movq %rsp, (%rdi)
  movq %rsi, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  ret

/* r12 holds the entry and r13 its argument, as proc_stack_prepare saved them. */
  .globl SYM(proc_stack_start)
  .p2align 4
SYM(proc_stack_start):
  movq %r13, %rdi
  callq *%r12
  ud2

#else
#error "processes need a stack switch for this architecture"
#endif

#if defined(__linux__) && defined(__ELF__)
  .section .note.GNU-stack,"",%progbits
#endif
