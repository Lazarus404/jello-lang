# Jello

Jello is a statically typed programming language with its own compiler and virtual machine. A program compiles to a single `.jlo` bytecode module, which the VM runs with a JIT compiler on arm64 and x86_64.

```jello
let greet = fn(name: Bytes): Bytes { "Hello, " + name + "!" };
greet("world")
```

The language has generics, enums with fields, pattern matching, records and byte layouts, closures, exceptions, macros that run while a program compiles, and modules & packages. Its standard library currently covers files, commands, regular expressions, lightweight processes with supervisors, and networking with TLS.

## Where this came from

This project began as the material for a book proposal; a language, its compiler and its virtual machine, built up one chapter at a time. It grew into something more useful. Jello is now a replacement for the original Jello - its compiler, `jelloc`, takes over from the one written in Neko, and its VM, `jello`, replaces the original and slower (roughly half the speed) VM.

## Why Jello?

Jello comes from my name; Jahred Love. The original J-Lo. Jello is also colourful, fun and
comes in several flavours, just like Jello (you can figure out which way around they go).

## The repository

| Directory | What it holds |
|-----------|---------------|
| [`jelloc/`](jelloc) | The compiler, written in Jello itself |
| [`stdlib/`](stdlib) | The standard library's Jello sources |

### jelloc, the compiler

`jelloc` compiles a program, its entry module and everything it imports, into one `.jlo` module. It lexes and parses the sources, type-checks them into a typed intermediate form, lowers that to a control-flow-graph IR, optimises it, allocates registers and emits the bytecode. Errors come with a location, the source line, a caret and a code that `jelloc --explain` describes.

It also works with projects - a directory with a `jello.json` names its sources, tests and dependencies, and `jelloc run`, `jelloc test` and `jelloc deps.get` build, test and fetch them.

The compiler is self-hosting. `jelloc/boot/jelloc.jlo` is a committed build of it, the seed, which compiles the compiler's sources in `jelloc/src/` on a fresh checkout. `jelloc` itself is the compiler as a native program, with that module built in.

## Getting started

### Building from source

You need a C compiler and `make`. Put the VM repo adjacent to this one, then build the VM:

```sh
make -C vm
```

That puts `jello`, `jlo_tool` and the native libraries in `vm/build/`. Then build the compiler: the seed compiles the compiler's sources, that compiler compiles them again, and `make` builds `jelloc` around the result:

```sh
mkdir -p jelloc/build
vm/build/jello jelloc/boot/jelloc.jlo jelloc/src/Main.jello --std stdlib --out jelloc/build/jelloc.jlo
vm/build/jello jelloc/build/jelloc.jlo jelloc/src/Main.jello --std stdlib --out jelloc/build/jelloc1.jlo
make -C vm jelloc
```

Yes, it's long winded, but it's a sanity check I've been doing since forever.

`vm/build/jelloc` is the compiler. Set `JELLO_STD_PATH` to the `stdlib` directory so it finds the standard library, and put `vm/build` on your `PATH`.

### Your first program

Make a project and run it:

```sh
jlo_tool init hello
cd hello
jelloc run
```

`jlo_tool init` writes a `jello.json` and a `src/Main.jello` that says hello. In a project, `jelloc` on its own compiles the program to `build/hello.jlo`, which `jello` runs:

```sh
jelloc
jello build/hello.jlo
```

A single file needs no project:

```sh
jelloc greet.jello --out greet.jlo
jello greet.jlo
```

## Licensing

Jello, meaning its compiler, its virtual machine, its tools and its standard library, is copyright Jahred Love and is released under the BSD 3-Clause licence, in [`LICENSE`](LICENSE), which each source file names in its `SPDX-License-Identifier` header.