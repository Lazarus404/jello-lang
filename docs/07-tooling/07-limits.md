# Limits

How big a Jello program may grow, how deep it may recurse and how much it may hold. Most limits
are set only by the memory the system gives the VM; the fixed ones are below, beside Lua 5.5's
for comparison.

| Limit | Jello | Lua 5.5 |
|-------|-------|---------|
| Calls nested at once, in the main program or any process, with the JIT or without | 1,000,000 | about 500,000 |
| Bytes of registers those calls may take | 1 GiB, beyond what the stack starts with | 1,000,000 stack slots |
| Locals in a function | no fixed limit: past 256 live at once, they spill; 10,000 tested | 200 |
| Parameters of a function | 256 registers, with its other locals | 200 |
| Values a closure captures | no fixed limit; 1,000 tested | 255 |
| Expressions nested in one another, such as parentheses | no fixed limit; 100,000 deep tested | about 200 |
| Statements in a function, functions and constants in a module | no fixed limit; a million statements tested | no fixed limit |
| `try` blocks nested in one function | 16 | about 200 `pcall`s nested at once |
| Heap | the system's memory | the system's memory |
| Elements of one `Array`, bytes of one `Bytes` | 2,147,483,647: indexes are `I32` | a string: the system's memory |
| Processes at once | the system's memory | the system's memory, as coroutines |

## Recursion

Each call takes one frame, and a frame takes 8 bytes a register its function uses, so a million
calls of a small function take a few tens of megabytes. The main program's frame stack starts at
64 MiB and a process's at 256 KiB, in the slot it is given. A stack that fills maps a segment
above it, twice the size of the last, until the segments come to 1 GiB; frames never move, so
growing costs nothing per call. A call in tail position to its own function reuses its frame and
does not count.

JIT code calls on the native stack, which is far smaller than a million frames. When a call
would go below the native stack's limit, the callee runs in the interpreter instead, which nests
no native frames, and so does everything it calls; the program goes on, more slowly, rather
than failing. Past a million calls, or past the frame stack's 1 GiB, the call traps
[STACK_OVERFLOW](../08-errors/02-runtime-traps.md#stack_overflow).

## Memory

The collector counts live bytes in 64 bits, so the heap grows until the system has no memory
left, at which point the VM ends with the message `jello: out of memory: cannot allocate N
bytes`. One object's block may be at most 4 GiB; an `Array` or `Bytes` stops at its `I32` index
first. A program holding 130 million list cells runs in 8.1 GB, where Lua needs 10.4 GB for as
many two-element tables.

## Processes

A process costs its record, its VM and the pages of its stacks it has touched: a process waiting
for a message takes about 19.5 KB on Apple silicon, most of it the one 16 KiB page where both its
stacks start, and less where pages are 4 KiB. A million processes fit in 19 GB, and spawning them
takes about five seconds. A process recurses as deep as the main program; its frame stack grows by segments of its
own, which it frees when it ends.
