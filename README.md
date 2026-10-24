# Jello

Jello is a statically typed programming language with its own compiler and virtual machine. A
program compiles to a single `.jlo` bytecode module, which the VM runs with a JIT compiler on
arm64 and x86_64.

```jello
let greet = fn(name: Bytes): Bytes { "Hello, " + name + "!" };
greet("world")
```

The language has generics, enums with fields, pattern matching, records and byte layouts,
closures, exceptions, macros that run while a program compiles, and modules & packages. Its
standard library currently covers files, commands, regular expressions, lightweight processes
with supervisors, and networking with TLS.

## Where this came from

This project began as the material for a book proposal; a language, its compiler and its virtual
machine, built up one chapter at a time. It grew into something more useful. Jello is now a
replacement for the original Jello - its compiler, `jelloc`, takes over from the one written in
Neko, and its VM, `jello`, replaces the original and slower (roughly half the speed) VM.

## Why Jello?

Jello comes from my name; Jahred Love. The original J-Lo. Jello is also colourful, fun and
comes in several flavours, just like Jello (you can figure out which way around they go).

## The repository

| Directory | What it holds |
|-----------|---------------|
| [`jelloc/`](jelloc) | The compiler, written in Jello itself |
| [`vm/`](vm) | The virtual machine, its native libraries and tools, in C |
| [`stdlib/`](stdlib) | The standard library's Jello sources |
| [`docs/`](docs/README.md) | The language reference |
| [`releases/`](releases) | Prebuilt releases, ready to install |

### jelloc, the compiler

`jelloc` compiles a program, its entry module and everything it imports, into one `.jlo` module.
It lexes and parses the sources, type-checks them into a typed intermediate form, lowers that to
a control-flow-graph IR, optimises it, allocates registers and emits the bytecode. Errors come
with a location, the source line, a caret and a code that `jelloc --explain` describes.

It also works with projects - a directory with a `jello.json` names its sources, tests and
dependencies, and `jelloc run`, `jelloc test` and `jelloc deps.get` build, test and fetch them.

The compiler is self-hosting. `jelloc/boot/jelloc.jlo` is a committed build of it, the seed,
which compiles the compiler's sources in `jelloc/src/` on a fresh checkout. `jelloc` itself is
the compiler as a native program, with that module built in.

### jello, the virtual machine

`vm/` builds these:

- `jello`, which runs a `.jlo` module, with a JIT compiler on arm64 and x86_64 as well as an
  interpreter. It runs a program's lightweight processes, each with its own heap, on one
  scheduler thread per CPU, and `jello --boot` turns a module into a standalone executable.
- `jlo_tool`, which makes projects (`jlo_tool init`), fetches their dependencies, and
  disassembles and inspects modules.
- `std.jdll`, the standard library's native code, and `tls.jdll`, TLS on Mbed TLS, which only a
  program that uses TLS loads.
- `libjello_vm.a` with `jello.h`, for a C program that embeds the VM, and `jello/jdll.h`, for
  writing native libraries of your own.

### docs, the language reference

[`docs/`](docs/README.md) describes the language as the compiler implements it: types,
statements, expressions, modules, the standard library, the tools and every error. Each example
in it is a program the compiler's tests run, so what it says a program prints is what the program
prints.

## Getting started

### Installing a release

On a Mac with Apple silicon, install the release committed in `releases/macos`:

```sh
releases/macos/install.sh
```

It installs `jello`, `jelloc` and `jlo_tool` in `~/.jello`, and sets up your shell so that they
are on `PATH` and `jelloc` finds the standard library. Open a new terminal afterwards. The
[release's README](releases/macos/README.md) lists its options, including `--uninstall`.

### Building from source

You need a C compiler and `make`. Build the VM:

```sh
make -C vm
```

That puts `jello`, `jlo_tool` and the native libraries in `vm/build/`. Then build the compiler:
the seed compiles the compiler's sources, that compiler compiles them again, and `make` builds
`jelloc` around the result:

```sh
mkdir -p jelloc/build
vm/build/jello jelloc/boot/jelloc.jlo jelloc/src/Main.jello --std stdlib --out jelloc/build/jelloc.jlo
vm/build/jello jelloc/build/jelloc.jlo jelloc/src/Main.jello --std stdlib --out jelloc/build/jelloc1.jlo
make -C vm jelloc
```

Yes, it's long winded, but it's a sanity check I've been doing since forever.

`vm/build/jelloc` is the compiler. Set `JELLO_STD_PATH` to the `stdlib` directory so it finds the
standard library, and put `vm/build` on your `PATH`.

### Your first program

Make a project and run it:

```sh
jlo_tool init hello
cd hello
jelloc run
```

`jlo_tool init` writes a `jello.json` and a `src/Main.jello` that says hello. In a project,
`jelloc` on its own compiles the program to `build/hello.jlo`, which `jello` runs:

```sh
jelloc
jello build/hello.jlo
```

A single file needs no project:

```sh
jelloc greet.jello --out greet.jlo
jello greet.jlo
```

From there, the [language reference](docs/README.md) starts with the
[lexical structure](docs/01-lexical.md), and the tooling pages describe
[jelloc](docs/07-tooling/01-jelloc.md), [jello](docs/07-tooling/02-jello.md) and
[projects](docs/07-tooling/03-projects.md).

## Licensing

Jello, meaning its compiler, its virtual machine, its tools and its standard library, is
copyright Jahred Love and is released under the BSD 3-Clause licence, in [`LICENSE`](LICENSE),
which each source file names in its `SPDX-License-Identifier` header.

The VM vendors two libraries, under their own licences:

| Library | Where it goes | Licence |
|---------|---------------|---------|
| [PCRE2](https://www.pcre.org/) 10.30 | `std.jdll`, for regular expressions | BSD 3-Clause, in [`vm/third_party/pcre2-10.30/LICENCE`](vm/third_party/pcre2-10.30/LICENCE) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) 3.5.1 | `tls.jdll`, for TLS | Apache 2.0 or GPL 2.0 or later, in [`vm/third_party/mbedtls-3.5.1/LICENSE`](vm/third_party/mbedtls-3.5.1/LICENSE) |

The compiler's character-width tables are generated from the Unicode Character Database, under
the [Unicode License v3](jelloc/third_party/ucd/LICENSE.txt). A release carries Jello's licence
and the vendored libraries' in its `licenses/` directory.
