# jello

`jello` runs a compiled `.jlo` module as a program. It loads and validates the module, runs it,
compiling its functions to machine code where it can, and prints the program's result.

```sh
jelloc main.jello --out main.jlo
jello main.jlo first "second argument"
```

## Usage

```text
jello [--no-jit | --require-jit] [--jit-report] [--schedulers <n>] [--jdll-path <dirs>]
      <program.jlo> [args...]
jello --boot [--bundle] [--jdll-path <dirs>] [-o <executable>] <program.jlo>
jello --jit-available
jello --version
jello --help
```

The options come before the module. Everything after the module is the program's own arguments,
which it reads as `System.args`, a `List<Bytes>`; `jello main.jlo a "b c"` gives the list
`["a", "b c"]`.

| Option | Meaning |
|--------|---------|
| `--no-jit` | Interpret every function; compile none to machine code. |
| `--require-jit` | Fail, with status 1, unless every function of the module compiles to machine code. |
| `--jit-report` | Report on standard error which functions compiled, which were declined and why, and how often compiled code was entered. Not with `--no-jit`. |
| `--schedulers n` | Run the program's processes on `n` OS threads, `n` at least 1. |
| `--jdll-path dirs` | Also look for native libraries in these directories, separated by `:`, or `;` on Windows (see [Native libraries](#native-libraries)). |
| `--jit-available` | Run nothing; exit 0 if this VM was built with a JIT for this machine, else 1. |
| `--version`, `-v` | Run nothing; print the VM's version, such as `jello 0.1.1`, and exit 0. |
| `--help`, `-h` | Print the usage and exit 0. |

By default the VM runs a function as machine code when it can compile it, and interprets it
otherwise; the two give the same results. A VM built without a JIT, or on a machine it has none
for, only interprets, and then `--require-jit` and `--jit-report` fail with `built without a
JIT`.

## The program's result

The entry module's last expression is the program's result (see
[Modules](../05-modules.md#the-entry-module-and-its-result)). When the program finishes,
`jello` prints it on standard output and exits 0:

| Result | Printed |
|--------|---------|
| `Bytes` | its bytes as they are, then a newline |
| `Bool` | `true` or `false` |
| an integer | its decimal value |
| a float | its value to 6 significant digits, as `0.333333` |
| a tuple, record, enum, array, list, atom or `Object` | its text, as `Std.string` shows it |
| `null`, or a program without a result | nothing |

The compiler turns a result of a type the VM keeps nothing of, such as an array, into its text, so
it reads as the program wrote it:

```jello
// expect: ([1, 2], :ok, "three")
([1, 2], :ok, "three")
```

A `Dynamic` result is shown by its kind alone, as `<array>`, and a function by nothing, so a
program that wants their text makes it itself, usually with `Std.string`.

A program can also write as it runs, with `writeText("/dev/stdout", text)` from `std.io.File`.

```jello
// expect: working
// expect: done
import { writeText } from std.io.File;

writeText("/dev/stdout", "working\n");
"done"
```

## Exit status

| Status | Meaning |
|--------|---------|
| 0 | The program finished. |
| `n` | The program called `System.exit(n)`. |
| 1 | The program trapped, or the module could not be read or loaded, or a native library it needs could not be found, or a JIT option could not be honoured. |
| 2 | The command line is wrong. |

```jello
// status: 3
// Nothing is printed: the program exits before it has a result.
System.exit(3);
"unreachable"
```

## Traps

A trap is an error the program cannot recover from, such as an index out of bounds or a throw
nothing catches. The program stops, `jello` writes where and why on standard error, and exits
with status 1. The message starts with the source place, as a compile error does, then
`jello: `:

```text
main.jello:2:1: jello: trap BOUNDS in function 0 at pc 6
```

```jello
// trap: BOUNDS
let xs = [1, 2, 3];
xs[5]
```

For an uncaught throw, a second line shows the thrown value: a scalar, `Bytes` or atom as a result
is shown, and anything else, whose type the VM does not keep, by its kind alone, as
`<enum value>`:

```text
main.jello:1:1: jello: trap UNCAUGHT_THROW in function 0 at pc 2
jello: thrown: boom
```

Then, for a trap inside a function, a `called from` line names each call that led there,
innermost first (see [Runtime traps](../08-errors/02-runtime-traps.md#what-a-trap-looks-like)).

| Trap | Cause |
|------|-------|
| `ARITHMETIC` | integer division or remainder by zero, or a similar arithmetic fault |
| `BOUNDS` | an index or slice outside a value's bounds |
| `NULL_DEREF` | a null value used where a value is needed |
| `TYPE_MISMATCH` | a `Dynamic` value of the wrong kind, or a native call that failed |
| `STACK_OVERFLOW` | calls nested too deeply |
| `UNCAUGHT_THROW` | a thrown value that no `catch` caught |
| `DEADLOCK` | a process waiting for what no process could ever do |
| `UNSENDABLE` | a value leaving its process holding a native handle that cannot be shared |
| `SEALED` | a write that would change a linked prototype's fixed method |
| `STOPPED` | the process was asked to stop |

Each is described in [Runtime traps](../08-errors/02-runtime-traps.md).

## Environment

| Variable | Meaning |
|----------|---------|
| `JELLO_SCHEDULERS` | How many OS threads run the program's processes; `--schedulers` sets it. By default, one for each processor the program may use. |
| `JELLO_JIT_DISABLE` | Any non-empty value turns the JIT off, as `--no-jit` does. |
| `JELLO_JDLL_PATH` | Directories to look for native libraries in, separated by `:`, or `;` on Windows. |

## Native libraries

A program's natives come from libraries named in its `load` statements (see
[Modules](../05-modules.md#native-libraries)). The library `name` is the file `name.jdll`, and the
VM loads each one the program uses before it runs. It looks for the file in these directories, in
order, and takes the first that opens:

1. `jdll/` beside the module;
2. the module's own directory;
3. each `--jdll-path` directory, in order;
4. the current directory;
5. each `JELLO_JDLL_PATH` directory, in order;
6. the directory `jello` is in;
7. `../lib` from there, where an installed VM keeps its libraries.

A file that is there but will not open, such as one built for another machine, is passed over.
When none is found, the VM exits with status 1, naming the file and every directory it tried, or,
when a file was there but would not open, why the last such file would not. A
library built against an older `jello/jdll.h` is refused with a message saying to rebuild it.
[Native libraries](06-native-libraries.md) shows how to write and build one.

The standard library's natives are in `std.jdll`, and TLS (`std.net.Tls`) and cryptography
(`std.crypto.Hash`, `Cipher` and `X509`) are in `tls.jdll`, which only a program using them loads.
An installation keeps both in its `lib/`, which is `../lib` from `jello`.

## Executables

`jello --boot` makes a standalone executable from a module: a copy of the VM with the module
appended, which runs the program when started, every argument the program's own.

```sh
jello --boot main.jlo              # writes ./main
jello --boot -o dist/hello main.jlo
jello --boot --bundle -o dist/hello main.jlo
./dist/hello Ann
```

- Without `-o`, the executable is written at the module's path less `.jlo`, plus `.exe` on
  Windows. With `-o`, it is written at exactly that path, whose directory must already exist.
- An executable looks for native libraries as `jello` does, with its own directory in place of
  the module's: `jdll/` beside itself first, then beside itself, and so on.
- `--bundle` copies each native library the program uses, `std.jdll` among them, into `jdll/`
  beside the executable, so that the directory can be moved or shipped as it is.
- The module is loaded first, which checks it and finds its libraries; a module that does not
  load makes no executable.

The format is the same on every system: the module and a 24-byte footer after the VM's own
bytes.
