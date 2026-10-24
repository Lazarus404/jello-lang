# jelloc

`jelloc` is the Jello compiler. It compiles a program, the entry module and everything it imports,
into one `.jlo` module that [jello](02-jello.md) runs.

```sh
jelloc main.jello --std /path/to/stdlib --out main.jlo
jello main.jlo
```

In a project, or a directory holding `src/Main.jello`, `jelloc` alone compiles the program into
`build/` (see [Defaults](#defaults)).

## Usage

```text
jelloc [input.jello] [--out path | --lex | --parse | --hir [--spans] | --ir | --jlo [--out path]]
       [--std dir] [--path dir]...
jelloc --test <file.test.jello>... --path dir... (--out path | --hir [--spans] | --ir | --jlo [--out path])
       [--std dir]
jelloc run [--std dir] [--path dir]... [file.jello [args...]]
jelloc test [--std dir] [--path dir]... [dir...] [-- args...]
jelloc deps.get [dir]
jelloc fmt [--check] [--strip-comments] [path...]
jelloc --explain <code>
jelloc --version
jelloc --help
```

Options may come before or after the input file.

## Compiling

| Option | Meaning |
|--------|---------|
| `--out path` | Write the compiled module to `path`, rather than to `build/` (see [Defaults](#defaults)). |
| `--std dir` | Find `std.*` modules under `dir`. Without it, the `JELLO_STD_PATH` environment variable gives the directory. |
| `--path dir` | Find other modules under `dir` too, after the input's own directory. Repeatable; the roots are searched in the order given. |
| `--test` | Compile test files into a test binary instead of a program (see [Testing](04-testing.md)). |
| `--version`, `-v` | Print the compiler's version, such as `jelloc 0.1.1`, and exit 0. It is the release's, which the VM's standard library reports. |
| `--help` | Print the usage and exit 0. |

How imports are resolved against the source roots is described in
[Modules](../05-modules.md#source-roots).

A compile of a named input is self-contained: it reads only the files the program imports, and
searches no project's sources. The `run`, `test` and `deps.get` subcommands, and a compile that
names no input, are the ones that work in a project.

## Defaults

What the command line leaves out, `jelloc` takes from the project at or above the current
directory, the nearest directory holding a `jello.json` (see [Projects](03-projects.md)), or else
from the current directory itself:

- With no input, it compiles the entry module: the file the manifest's `entry` names, or else
  `Main.jello` in its source directory, `src/Main.jello` by default. It searches the project's sources and its packages' sources, as `run` does.
  When that file does not exist, the command line is an error.
- With no `--out` and no listing option, it writes the module to `build/<name>.jlo` there, making
  `build/` if it is missing. The name is the manifest's `name`, or else the directory's own name,
  with each space or `/` made a `-`.

```sh
$ cd "my app"            # holds src/Main.jello, and no jello.json
$ jelloc                 # writes build/my-app.jlo
$ jelloc run             # compiles and runs src/Main.jello
```

`--test` assumes neither: it needs its test files and `--out`, or a listing option.

## Listings

Instead of writing a module, `jelloc` can stop part way and print what the compiler has made, on
standard output. One of these may be given:

| Option | Prints |
|--------|--------|
| `--lex` | the tokens, one a line |
| `--parse` | nothing; it checks the syntax only |
| `--hir` | the typed high-level IR; with `--spans`, each node's line ends with its source byte range, as `@8..9` |
| `--ir` | the control-flow-graph IR, before optimisation |
| `--jlo` | the optimised IR after register allocation, the instructions the module holds; with `--out`, it also writes the module |

These listings are for working on the compiler and reading what it makes of a program. Their
format is not stable.

For `answer.jello` holding `let x = 1;` and then `x`:

```sh
$ jelloc answer.jello --lex
Let
Ident("x")
Equal
IntegerConstant(1, Infer)
Semicolon
Ident("x")
Eof
```

The command line is an error (status 2, with the usage) when:

- no input is given and there is no entry module, or two inputs are given without `--test`;
- two of `--lex`, `--parse`, `--hir`, `--ir` and `--jlo` are given;
- `--test` is given with neither `--out` nor a listing option;
- `--out` is given with `--lex`, `--parse`, `--hir` or `--ir`;
- `--spans` is given without `--hir`, or `--test` with `--lex` or `--parse`;
- an option is unknown, or lacks its value.

An input file that does not exist is status 1: `jelloc: cannot read main.jello`.

## Subcommands

The subcommands compile and run in one step. They find a project, the nearest directory holding a
`jello.json`, and search its sources and its dependencies' sources after the `--path` roots. See
[Projects](03-projects.md).

| Command | Does |
|---------|------|
| `jelloc run [options] [file.jello [args...]]` | Compiles the file, by default the entry module (see [Defaults](#defaults)), into the project's `build/run/` (outside a project, under `jelloc/build/run/` in the [temporary directory](../06-stdlib/03-io.md#stdsysplatform)) and runs it with `jello`, passing it `args`. |
| `jelloc test [options] [dir...] [-- args...]` | Compiles every `*.test.jello` file under the directories, by default the project's test directories, into one test binary, `build/test.jlo`, and runs it with the arguments after `--`. |
| `jelloc deps.get [dir]` | Fetches the dependencies of the project at or above `dir`, or the current directory, into its `packages/`. |
| `jelloc fmt [--check] [--strip-comments] [path...]` | Lays out the files, and the files under the directories, by default the inputs of the nearest `.formatter`. See [Formatting](08-formatting.md). |

`run` and `test` replace the compiler's process with `jello`, so they exit as the program does.
`deps.get` runs `jlo_tool get-deps`. Each tool is found as an environment variable says,
`JELLO_VM` for `jello` and `JELLO_JLO_TOOL` for `jlo_tool`, else beside `jelloc`, else on
`PATH`.

## Explaining an error code

Each compile error ends with a code, such as `[E0202]`. `--explain` prints what it means and how
to fix it:

```sh
$ jelloc --explain E0202
E0202: a module's package does not match its path

A module's `package` declaration must name the directories its import path
gives; a.b.C declares `package a.b;`. Change the declaration or the import.
```

An unknown code is a command-line error, status 2. Every code is listed in
[Compile errors](../08-errors/01-compile-errors.md).

## Diagnostics

A program that does not compile gets one diagnostic for each error, on standard error, and
`jelloc` exits with status 1 without writing a module. Each diagnostic is the place, the message
and its code, then the source line and a caret under the place:

```text
err.jello:1:14: x is annotated I32, but its value is Bytes [E0400]
let x: I32 = "a";
             ^
err.jello:2:9: unknown name: z [E0300]
let y = z;
        ^
```

The place is `path:line:column`. Lines and columns count from 1, and a column counts characters,
not bytes; the caret is placed under the character however wide the characters before it are
shown, and the source line keeps its tabs.

- The path is the module's file as the compiler found it: as given on the command line for the
  entry module, and its root joined with its module path for an imported one.
- An error that relates to another place has a note: a second diagnostic, `path:line:col: note:
  ...`, with its own source line and caret. A module whose package does not match its path, for
  example, is reported at the import, with a note at the `package` declaration. An error in an
  instance of a generic, whose code may be in another module such as a standard one, has a note
  at the program's own code that needs the instance.
- The compiler goes on past an error to the next top-level statement and reports every error it
  finds, in source order, up to 20. After the twentieth it stops, with a last line `stopped
  after 20 errors`. A module with an error is the last one checked, since later modules may use
  what it declares.
- With `--parse`, a failure is labelled with the pass that found it, `LEXER` or `PARSER`, and a
  tab before the diagnostic.

A warning has the same form with `warning:` before its message and no code. Warnings do not stop
the compile; they are printed before the compile's other output, and the exit status is 0.

```text
warn.jello:2:16: warning: this clause cannot fail: use `let`
let sum = with (a, b) <- pair { a + b } else { _ => 0 };
               ^
```

An internal error is a check the compiler makes of its own work that failed. It is reported as
`internal error: ... (please report this compiler bug)`, with status 3, and is never caused by a
mistake in the program.

## Exit status

| Status | Meaning |
|--------|---------|
| 0 | The module was written, or the listing printed. |
| 1 | The program does not compile, or an input cannot be read. |
| 2 | The command line is wrong, or `--explain` was given an unknown code. |
| 3 | Internal error: a compiler bug. |

`jelloc run` and `jelloc test` exit with the status of the program they run. `jelloc fmt` exits
with 1 when a file does not lex, or, with `--check`, when a file would change, and with 3 when
laying out a file would change its tokens.
