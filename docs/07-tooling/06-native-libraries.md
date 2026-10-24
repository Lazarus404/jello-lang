# Native libraries

Jello is a comfortable place to write programs, but sometimes a program needs something only C
has: a library someone wrote twenty years ago, a system call, or a loop where every nanosecond
counts. A *native library*, a `.jdll` file, is how Jello reaches out to C. You write a few C
functions, list them in a table, compile the file as a shared library, and Jello code calls them
as if they were its own.

This page walks through building one from nothing. It assumes you can read a little C and have
written a little Jello, and nothing more: no linker wizardry, no knowledge of the VM's insides.
By the end you will have built seven small libraries, each showing one idea, and you will know
the rules that keep a native library from taking the VM down with it.

Every example on this page is real. The test suite builds each library from the C shown here,
runs each program against it, and checks it prints what the page says it prints.

## Before you start: do you need one?

A native library is the right tool when:

- you want to use an existing C library, such as a compression, image or database library;
- you need something of the operating system that the [standard library](../06-stdlib/README.md)
  does not offer;
- you have measured a hot loop, and it is still too slow in Jello.

It is the wrong tool when the standard library already does the job, or when the work is mostly
moving data around: every call crosses a border, and C code can crash in ways Jello code cannot.
A bug in a native library is a bug in the VM. With that warning on the label, let's cook.

## You will need

- **A C compiler.** `cc` on macOS (Xcode's command-line tools) and Linux (clang or gcc), or
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)'s `clang` on Windows.
- **Two headers**, `jello.h` and `jello/jdll.h`. An installed Jello keeps them in `include/`
  under its root, `$JELLO_ROOT/include`; in a checkout of the repository they are in
  `vm/src/include`. The commands below write `$JELLO_ROOT/include`.
- **Jello itself**, `jelloc` and `jello` (see [Platforms](05-platforms.md)).

## Hello from C, in five minutes

Make a directory for the project, and a `jdll/` directory inside it, where the VM will look for
libraries:

```text
hello/
  hello.c         the library's C
  main.jello      a program that uses it
  jdll/
    hello.jdll    the library, once built
```

Here is the whole library. It exports two functions: `greet`, which takes nothing and returns
some text, and `add`, which adds two numbers.

```c native=hello
#include <jello/jdll.h>

/* Returns a greeting. */
static void greet(jdlo_ctx* c) {
  static const char text[] = "hello from C";
  jdl_return_bytes_copy(c, (const uint8_t*)text, sizeof text - 1);
}

/* Returns the sum of two I32s. */
static void add(jdlo_ctx* c) {
  int32_t a = jdl_arg_i32(c, 0);
  int32_t b = jdl_arg_i32(c, 1);
  jdl_return_i32(c, a + b);
}

/* What the library exports: each function's name, its C function, its result and its
 * arguments. */
static const jello_jdll_export EXPORTS[] = {
  {"greet", greet, JELLO_T_BYTES, 0, {0}, 0},
  {"add", add, JELLO_T_I32, 2, {JELLO_T_I32, JELLO_T_I32}, 0},
};

JELLO_JDLL_LIBRARY(hello, EXPORTS);
```

Build it. The command differs by system, because each system has its own idea of what a shared
library is:

```sh
cc -std=c11 -I"$JELLO_ROOT/include" -bundle hello.c -o jdll/hello.jdll          # macOS
cc -std=c11 -I"$JELLO_ROOT/include" -fPIC -shared hello.c -o jdll/hello.jdll    # Linux
clang -std=c11 -I"$JELLO_ROOT/include" -shared hello.c -o jdll/hello.jdll       # Windows
```

Whatever the system, the file is called `hello.jdll`: the name the program uses, plus `.jdll`.

Now the Jello side. `load` names the functions the program wants from the library, and a typed
`let` gives each one its Jello type:

```jello native=hello
// expect: hello from C, and 2 + 3 = 5
load { greet, add } from hello;

let greeting: () -> Bytes = greet;
let plus: (I32, I32) -> I32 = add;

greeting() + ", and 2 + 3 = " + Std.string(plus(2, 3))
```

Run it:

```sh
jelloc run main.jello
```

```text
hello from C, and 2 + 3 = 5
```

That's it: a C function, called from Jello. The rest of this page is about doing it well.

## What just happened

It is worth knowing the journey a call makes, because each error you might meet comes from one
step of it.

```text
 main.jello                       the compiler              the VM, when the program starts
 ----------                       ------------              -------------------------------
 load { add } from hello;     --> records an import:    --> 1. finds hello.jdll
 let plus: (I32, I32) -> I32       "add from hello,         2. opens it, and reads its
     = add;                         (I32, I32) -> I32"         manifest
                                                            3. checks the manifest's ABI
                                                               version and name
                                                            4. hands the library the VM's
                                                               function table
                                                            5. finds "add" in the library's
                                                               export table, and checks its
                                                               types match the import's

 plus(2, 3)                   --> a native call         --> the VM puts 2 and 3 in a call
                                                            context, calls add(c), and takes
                                                            the result add returned through c
```

A few things follow from this:

- **The compiler never looks at the library.** It trusts the type you wrote in the `let`. The
  check happens when the program starts: a program whose types do not match the library's does
  not run at all, rather than running and doing something strange.
- **Every library the program loads is opened before the first line runs.** A missing library
  fails at once, not halfway through.
- **The library reaches the VM only through the function table it is handed in step 4.** It has
  no other link to the program that loaded it, so the same `.jdll` works with `jello`, with
  `jelloc`, and with an executable made by `jello --boot`.

Here is the start-up check catching a `let` that disagrees with the library. Nothing runs, and
the VM says exactly what disagrees:

```jello native=hello
// stderr: native add from hello is bound as (I64, I64) -> I64, but the library exports (I32, I32) -> I32
load { add } from hello;

let plus: (I64, I64) -> I64 = add;

plus(2i64, 3i64)
```

And a name the library does not export:

```jello native=hello
// stderr: hello.jdll exports no function greeet
load { greeet } from hello;

let greeting: () -> Bytes = greeet;

greeting()
```

## The anatomy of a library

Every library has the same three parts.

### 1. The header

```c
#include <jello/jdll.h>
```

It brings in `jello.h` too, which describes Jello's values. You never link against anything: a
library calls the VM only through the `jdl_*` functions this header declares, and those go
through the table the VM hands over.

### 2. The exports

An export is a C function of one shape:

```c
static void name(jdlo_ctx* c);
```

`jdlo_ctx` is the call in progress. The function reads its arguments from it with `jdl_arg_*`,
does its work, and gives back its result with one `jdl_return_*`. It returns `void` in C: the
result goes through `c`, not through C's `return`.

Make the functions `static`. Only the manifest needs to be visible outside the file, and keeping
the rest hidden means two libraries can both have a function called `add` without a fight.

### 3. The table and the manifest

The table lists what the library offers. Each entry is a `jello_jdll_export`:

| Field | Meaning | In `add`'s entry |
|-------|---------|------------------|
| `name` | the name a program's `load` uses | `"add"` |
| `fn` | the C function | `add` |
| `ret_kind` | the result's kind, a `JELLO_T_*` | `JELLO_T_I32` |
| `arity` | how many arguments, at most 8 | `2` |
| `arg_kinds` | each argument's kind | `{JELLO_T_I32, JELLO_T_I32}` |
| `flags` | `0`, or `JELLO_JDLL_BLOCKING` (see [blocking calls](#blocking-calls)) | `0` |

`JELLO_JDLL_LIBRARY(hello, EXPORTS)` then writes the manifest, the one thing the VM looks for
when it opens the file. Its first argument is the library's name, which must be:

- a C identifier, such as `hello` or `image_tools`, since it becomes part of a symbol's name;
- the file's name less `.jdll`, so `hello` lives in `hello.jdll`;
- the name programs use in `load ... from hello`.

Use the macro once per library. `std` and `tls` are taken: they are the standard library's own.

## Types across the border

A native's arguments and result are each one of seven Jello types, or an abstract type the
program names for a library's handles (see [Handles](#handles-keeping-c-things-alive)). Each
has a kind for the export table, an accessor to read it, and a function to return it:

| Jello type | Kind | Read with | C type | Return with |
|------------|------|-----------|--------|-------------|
| `Bool` | `JELLO_T_BOOL` | `jdl_arg_bool` | `int`, 0 or 1 | `jdl_return_bool` |
| `I32` | `JELLO_T_I32` | `jdl_arg_i32` | `int32_t` | `jdl_return_i32` |
| `I64` | `JELLO_T_I64` | `jdl_arg_i64` | `int64_t` | `jdl_return_i64` |
| `F32` | `JELLO_T_F32` | `jdl_arg_f32` | `float` | `jdl_return_f32` |
| `F64` | `JELLO_T_F64` | `jdl_arg_f64` | `double` | `jdl_return_f64` |
| `Bytes` | `JELLO_T_BYTES` | `jdl_arg_bytes_data`, `jdl_arg_bytes_len` | `const uint8_t*`, `uint32_t` | `jdl_return_bytes_copy` |
| `Dynamic` | `JELLO_T_DYNAMIC` | `jdl_arg_value` | `jello_value` | `jdl_return_value`, `jdl_return_null`, `jdl_return_handle` |
| an abstract type | `JELLO_T_DYNAMIC` | `jdl_arg_handle` | your payload's pointer | `jdl_return_handle`, and `jdl_return_null` for `?Name` |

Anything else, such as an array, a record or an enum, crosses inside a `Dynamic`, or better,
encoded as `Bytes` (see [Dynamic values](#dynamic-values)). A native with nothing to give back
has the result `Void`, kind `JELLO_T_VOID`: it simply returns, and a call of it is `null` to
Jello. (It may call `jdl_return_void`, which says the same thing.)

Arguments are numbered from 0, in the order the Jello type lists them. Each accessor trusts the
table, so call the one that matches the kind you declared: `jdl_arg_i64` on an `I32` argument
reads nonsense.

Here is a library of number functions, one for each scalar type:

```c native=numbers
#include <jello/jdll.h>

#include <math.h>

/* The length of the hypotenuse of a right triangle with sides a and b. */
static void hypotenuse(jdlo_ctx* c) {
  jdl_return_f64(c, sqrt(jdl_arg_f64(c, 0) * jdl_arg_f64(c, 0) +
                         jdl_arg_f64(c, 1) * jdl_arg_f64(c, 1)));
}

/* How many bits of an I64 are set. */
static void bits_set(jdlo_ctx* c) {
  uint64_t x = (uint64_t)jdl_arg_i64(c, 0);
  int32_t count = 0;
  while (x) {
    x &= x - 1;
    count++;
  }
  jdl_return_i32(c, count);
}

/* Whether an I32 is a power of two. */
static void is_power_of_two(jdlo_ctx* c) {
  int32_t x = jdl_arg_i32(c, 0);
  jdl_return_bool(c, x > 0 && (x & (x - 1)) == 0);
}

/* Half of an F32. */
static void halve(jdlo_ctx* c) {
  jdl_return_f32(c, jdl_arg_f32(c, 0) / 2.0f);
}

static const jello_jdll_export EXPORTS[] = {
  {"hypotenuse", hypotenuse, JELLO_T_F64, 2, {JELLO_T_F64, JELLO_T_F64}, 0},
  {"bits_set", bits_set, JELLO_T_I32, 1, {JELLO_T_I64}, 0},
  {"is_power_of_two", is_power_of_two, JELLO_T_BOOL, 1, {JELLO_T_I32}, 0},
  {"halve", halve, JELLO_T_F32, 1, {JELLO_T_F32}, 0},
};

JELLO_JDLL_LIBRARY(numbers, EXPORTS);
```

On Linux, add `-lm` to the build command for `sqrt`.

```jello native=numbers
// expect: 5.0 64 true false 1.25
load { hypotenuse, bits_set, is_power_of_two, halve } from numbers;

let hyp: (F64, F64) -> F64 = hypotenuse;
let setBits: (I64) -> I32 = bits_set;
let isPow2: (I32) -> Bool = is_power_of_two;
let half: (F32) -> F32 = halve;

Std.string(hyp(3.0, 4.0)) + " " + Std.string(setBits(-1i64)) + " "
  + Std.string(isPow2(1024)) + " " + Std.string(isPow2(1000)) + " " + Std.string(half(2.5f32))
```

### What the compiler checks

The compiler holds the `let` to these rules, so a mistake shows up as you compile, with an error
code you can look up with `jelloc --explain`:

- The `let` must have a function type. `let f = add;` is error
  [E0213](../08-errors/01-compile-errors.md#e0213-an-invalid-native-declaration).
- Each parameter and the result must be one of the seven types above or an abstract type, null
  or not, and there are at most 8 parameters; `(U8) -> I32` is E0213 too. In a function type
  `?Name` is a parameter a call may omit, so a parameter that may be null is written
  `Null<Name>`, as in `(Null<Tally>) -> Void`.
- The result may also be `Void`.
- The binding can only be called. To pass it around as a value, wrap it:
  `let f = fn(a: I32, b: I32): I32 { plus(a, b) };`.

The VM then checks the types against the library's table when the program starts.

## Bytes in, Bytes out

`Bytes` is how text and binary data cross the border, and it comes with two rules.

**A `Bytes` argument is borrowed.** `jdl_arg_bytes_data` gives a pointer to the bytes, and
`jdl_arg_bytes_len` their length. The pointer is good until the export returns and not a moment
longer, so never keep it in a global or hand it to another thread. And it is not a C string:
there is no `'\0'` at the end, and the bytes may hold zeros in the middle. Always use the length.

**A `Bytes` result is copied.** `jdl_return_bytes_copy(c, data, len)` copies `len` bytes into a
new Jello `Bytes`, so the export can free or reuse its buffer straight after.

Here is a library that shouts, turning ASCII letters to capitals and adding an exclamation mark:

```c native=shout
#include <jello/jdll.h>

#include <stdlib.h>
#include <string.h>

/* The text in capitals, with "!" after it. */
static void shout(jdlo_ctx* c) {
  const uint8_t* text = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  uint8_t* loud = malloc((size_t)len + 1);
  if (!loud) {
    jdl_fail(c, "out of memory");
    return;
  }
  for (uint32_t i = 0; i < len; i++) {
    uint8_t ch = text[i];
    loud[i] = (ch >= 'a' && ch <= 'z') ? (uint8_t)(ch - 'a' + 'A') : ch;
  }
  loud[len] = '!';
  jdl_return_bytes_copy(c, loud, len + 1);
  free(loud);
}
```

The `malloc` buffer belongs to the export, which frees it once the result is copied. For a small,
bounded result, a buffer on the stack does as well.

The same library can count words, reading `Bytes` without changing them:

```c native=shout
/* How many words, runs of bytes other than spaces, the text holds. */
static void word_count(jdlo_ctx* c) {
  const uint8_t* text = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int32_t words = 0;
  int in_word = 0;
  for (uint32_t i = 0; i < len; i++) {
    int space = text[i] == ' ';
    if (!space && !in_word) words++;
    in_word = !space;
  }
  jdl_return_i32(c, words);
}

static const jello_jdll_export EXPORTS[] = {
  {"shout", shout, JELLO_T_BYTES, 1, {JELLO_T_BYTES}, 0},
  {"word_count", word_count, JELLO_T_I32, 1, {JELLO_T_BYTES}, 0},
};

JELLO_JDLL_LIBRARY(shout, EXPORTS);
```

```jello native=shout
// expect: WOBBLE, WOBBLE!
// expect: 4 words
// expect: !
load { shout, word_count } from shout;

let loud: (Bytes) -> Bytes = shout;
let words: (Bytes) -> I32 = word_count;

loud("wobble, wobble") + "\n" + Std.string(words("  jello is quite  wobbly ")) + " words\n"
  + loud("")
```

Notice the last line: an empty `Bytes` has length 0, and its pointer may be anything, even
`NULL`, so code that checks the length before reading is safe.

## When things go wrong

C has no exceptions, so a native reports failure one of two ways.

### The quick way: `jdl_fail`

`jdl_fail(c, message)` makes the call fail: once the export returns, the program stops with the
trap `TYPE_MISMATCH`, whatever else the export returned, and the VM prints the export's name and
the message, which it copies, after the trap. It is the right response to something that should
never happen, such as running out of memory. It is the wrong response to anything a program might
reasonably want to handle, because a trap cannot be caught: `try` does not see it.

The same trap also happens when an export forgets to return anything, or returns a kind other
than the one in its table. Each of those is a bug in the library.

### The friendly way: return null, and let Jello throw

For a failure the program should handle, such as bad input or a missing file, declare the result
`Dynamic` and return null, and let a small Jello wrapper turn null into an exception. This is
what the standard library does throughout.

```c native=ports
#include <jello/jdll.h>

/* The port number the text spells, 1 to 65535, as a Dynamic, or null if it spells none. */
static void parse_port(jdlo_ctx* c) {
  const uint8_t* text = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int32_t port = 0;
  if (len == 0 || len > 5) {
    jdl_return_null(c);
    return;
  }
  for (uint32_t i = 0; i < len; i++) {
    if (text[i] < '0' || text[i] > '9') {
      jdl_return_null(c);
      return;
    }
    port = port * 10 + (text[i] - '0');
  }
  if (port < 1 || port > 65535) {
    jdl_return_null(c);
    return;
  }
  jdl_return_value(c, jello_make_i32(port));
}

/* Always fails, as a library with a bug might. */
static void broken(jdlo_ctx* c) {
  jdl_fail(c, "this never works");
}

static const jello_jdll_export EXPORTS[] = {
  {"parse_port", parse_port, JELLO_T_DYNAMIC, 1, {JELLO_T_BYTES}, 0},
  {"broken", broken, JELLO_T_I32, 0, {0}, 0},
};

JELLO_JDLL_LIBRARY(ports, EXPORTS);
```

`jello_make_i32` turns a C integer into a `Dynamic` without allocating anything, so an export may
use it (see [the rules](#the-rules) for why that matters). On the Jello side, the wrapper checks
for null:

```jello native=ports
// expect: 8080
// expect: caught: not a port: http
load { parse_port } from ports;

let _parsePort: (Bytes) -> Dynamic = parse_port;

// The port the text spells; anything else throws.
let parsePort = fn(text: Bytes): I32 {
  let port = _parsePort(text);
  if (port == null) { throw "not a port: " + text; }
  port as I32
};

let good = Std.string(parsePort("8080"));
let bad = try { Std.string(parsePort("http")) } catch (e) { "caught: " + (e as Bytes) };
good + "\n" + bad
```

And here is the trap, for comparison. The program stops, and `try` is no help:

```jello native=ports
// stderr: trap TYPE_MISMATCH
// stderr: jello: native broken failed: this never works
load { broken } from ports;

let neverWorks: () -> I32 = broken;

try { neverWorks() } catch (e) { 0 }
```

## Dynamic values

A `Dynamic` is a `jello_value`, one machine word that is either the value itself or a pointer to
it. `jello.h` has a test and a reader for each kind:

| A Dynamic holding | Test | Read |
|-------------------|------|------|
| `null` | `jello_is_null(v)` | |
| a `Bool` | `jello_is_bool(v)` | `jello_as_bool(v)` |
| an `I32` | `jello_is_i32(v)` | `jello_as_i32(v)` |
| an atom, such as `:ok` | `jello_is_atom(v)` | `jello_as_atom(v)`, then `jdl_atom_name(c, id)` for its name |
| an `I64` | `jello_is_box_i64(v)` | `jello_as_box_i64(v)` |
| an `F64` | `jello_is_box_f64(v)` | `jello_as_box_f64(v)` |
| anything else | `jello_is_ptr(v) && v != 0` | the object's `jello_obj_header` says its kind |

An object's header starts every heap object, and its `kind` is a `jello_obj_kind`:
`JELLO_OBJ_BYTES` for `Bytes` (a `jello_bytes`, with `data` and `length`), `JELLO_OBJ_OBJECT` for
an `Object` (a `jello_object`), and so on.

An atom's number is the program's own, and means nothing on its own: `:ok` may be 7 in one
program and 12 in another. Always turn it into a name with `jdl_atom_name` before comparing.

Here is a library that describes whatever it is given:

```c native=peek
#include <jello/jdll.h>

#include <stdio.h>
#include <string.h>

/* The kind of object at v, which is a pointer. */
static uint8_t object_kind(jello_value v) {
  return ((const jello_obj_header*)jello_as_ptr(v))->kind;
}

/* Adds the names of an Object's own fields to out, which holds at most size bytes. */
static void field_names(jdlo_ctx* c, const jello_object* o, char* out, size_t size) {
  uint32_t keys[64];
  if (o->len > 64) {
    snprintf(out, size, "an object with %u fields", (unsigned)o->len);
    return;
  }
  /* Atom 0 is __proto__ only in a program that uses prototypes, so it is left out only then. */
  const char* first = jdl_atom_name(c, 0);
  int skip_proto = first && strcmp(first, "__proto__") == 0;
  uint32_t n = jdl_object_keys(o, skip_proto, keys);
  snprintf(out, size, "an object with");
  for (uint32_t i = 0; i < n; i++) {
    size_t at = strlen(out);
    snprintf(out + at, size - at, " %s", jdl_atom_name(c, keys[i]));
  }
}

/* What the Dynamic argument holds, in words. */
static void describe(jdlo_ctx* c) {
  jello_value v = jdl_arg_value(c, 0);
  char text[256];
  if (jello_is_null(v)) {
    snprintf(text, sizeof text, "null");
  } else if (jello_is_bool(v)) {
    snprintf(text, sizeof text, "the Bool %s", jello_as_bool(v) ? "true" : "false");
  } else if (jello_is_i32(v)) {
    snprintf(text, sizeof text, "the I32 %d", (int)jello_as_i32(v));
  } else if (jello_is_atom(v)) {
    snprintf(text, sizeof text, "the atom :%s", jdl_atom_name(c, jello_as_atom(v)));
  } else if (jello_is_box_i64(v)) {
    snprintf(text, sizeof text, "the I64 %lld", (long long)jello_as_box_i64(v));
  } else if (jello_is_box_f64(v)) {
    snprintf(text, sizeof text, "the F64 %g", jello_as_box_f64(v));
  } else if (object_kind(v) == JELLO_OBJ_BYTES) {
    const jello_bytes* b = jello_as_ptr(v);
    snprintf(text, sizeof text, "%u bytes", (unsigned)b->length);
  } else if (object_kind(v) == JELLO_OBJ_OBJECT) {
    field_names(c, jello_as_ptr(v), text, sizeof text);
  } else {
    snprintf(text, sizeof text, "something else");
  }
  jdl_return_bytes_copy(c, (const uint8_t*)text, (uint32_t)strlen(text));
}

static const jello_jdll_export EXPORTS[] = {
  {"describe", describe, JELLO_T_BYTES, 1, {JELLO_T_DYNAMIC}, 0},
};

JELLO_JDLL_LIBRARY(peek, EXPORTS);
```

```jello native=peek
// expect: null
// expect: the Bool true
// expect: the I32 42
// expect: the atom :wobbly
// expect: the I64 9000000000
// expect: the F64 2.5
// expect: 5 bytes
// expect: an object with flavour scoops
// expect: something else
load { describe } from peek;

let what: (Dynamic) -> Bytes = describe;

let things: Array<Dynamic> = [
  null, true as Dynamic, 42 as Dynamic, :wobbly as Dynamic, 9000000000i64 as Dynamic,
  2.5 as Dynamic, "jelly" as Dynamic, { flavour: "lime", scoops: 2 } as Dynamic,
  [1, 2, 3] as Dynamic
];
let lines = Array.new<Bytes>(0);
for (thing in things) { lines.push(what(thing)); }
lines.join("\n")
```

Fields come out in atom order, which is not the order they were written in.

### What a native can give back as a Dynamic

A native cannot make new Jello objects (see [the rules](#the-rules)), so a `Dynamic` result can
only be:

- null, with `jdl_return_null`;
- a value that fits in the word, made with `jello_make_bool`, `jello_make_i32` or
  `jello_make_atom` (an atom the program already has, by its number);
- an object that came in as an argument, returned as it is;
- new `Bytes`, with `jdl_return_bytes_copy`;
- a native handle, with `jdl_return_handle` (next section).

An `I64` or `F64` result, which would need a box, has its own typed export instead. Structured
results, such as a list of names, are best encoded in `Bytes` and decoded by a Jello wrapper:
the standard library returns a directory listing as names ending in `'\0'`, for example.

## Handles: keeping C things alive

Many C libraries hand out pointers to things that live for a while: an open database, a
compiled regular expression, a window and the renderer that draws in it. A *native handle* lets
a Jello program hold one. To Jello it is a value of an
[abstract type](../02-types/06-typedefs-and-records.md#abstract-types), a type with a name and
nothing else, that it cannot look inside; to C it is a pointer, the *payload*, with a *class*
that says how to free it.

A class is a `jello_jdll_handle_class` with three members:

| Member | What it is |
|--------|------------|
| `name` | a name for the class, for people reading the code |
| `finalize` | frees the payload once no handle holds it; may be `NULL` |
| `retain` | takes another hold on the payload, for a copy in another process; may be `NULL` |

Here is a tally, a counter Jello code can add to:

```c native=tally
#include <jello/jdll.h>

#include <stdlib.h>

/* A tally: how many handles hold it, which jdl_retain and jdl_release keep, and its count. */
typedef struct tally {
  jdl_refs refs;
  int64_t count;
} tally;

/* Drops a hold on a tally, freeing it with the last. */
static void tally_finalize(void* payload) {
  if (jdl_release(payload)) free(payload);
}

static const jello_jdll_handle_class TALLY = {"tally", tally_finalize, jdl_retain};

/* A new tally at zero. */
static void tally_new(jdlo_ctx* c) {
  tally* t = malloc(sizeof *t);
  if (!t) {
    jdl_fail(c, "out of memory");
    return;
  }
  t->refs = 1;
  t->count = 0;
  jdl_return_handle(c, &TALLY, t, sizeof *t);
}

/* Adds to a tally; its new count. */
static void tally_add(jdlo_ctx* c) {
  tally* t = jdl_arg_handle(c, 0, &TALLY);
  if (!t) return;
  int64_t added = jdl_arg_i64(c, 1);
  jdl_return_i64(c, __atomic_add_fetch(&t->count, added, __ATOMIC_RELAXED));
}

/* Sets a tally back to zero. It returns nothing: its result is Void. */
static void tally_reset(jdlo_ctx* c) {
  tally* t = jdl_arg_handle(c, 0, &TALLY);
  if (t) __atomic_store_n(&t->count, 0, __ATOMIC_RELAXED);
}
```

The pieces:

- **`jdl_return_handle(c, &TALLY, t, sizeof *t)`** wraps the payload in a handle and returns it
  as a `Dynamic`. The last argument is roughly how much memory the payload uses; the VM counts it
  when it decides to collect garbage, so a program that makes many big handles frees them sooner.
- **`jdl_arg_handle(c, 0, &TALLY)`** unwraps argument 0. It gives `NULL` for a null argument, and
  for anything that is not a handle of this class it fails the call, so `tally_add` can never be
  given someone else's pointer. Returning straight away on `NULL` is all an export has to do.
- **`finalize`** runs when the garbage collector finds the handle unreachable, or when the
  program ends. It runs on whatever thread is collecting, at a time no one chooses, so it must
  only free things: no `jdl_*` calls, no locks the program might hold.

On the Jello side, give each class a type of its own with `typedef Tally = abstract;`, and bind
the natives with it:

```text
typedef Tally = abstract;
let newTally: () -> Tally = tally_new;
let add: (Tally, I64) -> I64 = tally_add;
```

Now passing a window where a tally is wanted is an error at compile time, not a trap at run
time. A native that may return null, say when a file will not open, is bound with `?Tally`.
[Abstract types](../02-types/06-typedefs-and-records.md#abstract-types) says what Jello checks
as a handle comes back.

### Handles and processes

A Jello program can run many [processes](../06-stdlib/04-concurrent.md) at once, on several
threads. A handle sent to another process, or captured by a function a process runs, is a copy
of the handle with the same payload, so two threads can reach one payload at the same time.

A class with a `retain` member agrees to that. `retain` takes another hold on the payload for
the copy, and the copy's `finalize` drops it, so the payload is freed only when the last copy is
gone. `jdl_refs` and its two helpers do the counting for you: make a `jdl_refs` the payload's
first member, set it to 1, and use `jdl_retain` as `retain` and `jdl_release` in `finalize`, as
the tally does.

But sharing has a price: the payload must be safe to use from several threads. The tally adds
with an atomic instruction, `__atomic_add_fetch`, so two processes adding at once never lose a
count. A payload that cannot be made safe, say a C library object that must stay on one thread,
gets a class *without* `retain`, and then the VM refuses to let its handles leave their process:
sending one in a message, or capturing one in a function a new process runs, stops the program
with the trap `UNSENDABLE`. (A new process also starts with a copy of its spawner's globals; a
global holding such a handle is simply `null` in the copy.)

Here are both, in the same library:

```c native=tally
/* A tally that only one process may use: its class has no retain. */
static const jello_jdll_handle_class LOCAL_TALLY = {"local tally", free, NULL};

/* A new tally at zero, for the calling process alone. */
static void local_tally_new(jdlo_ctx* c) {
  tally* t = calloc(1, sizeof *t);
  if (!t) {
    jdl_fail(c, "out of memory");
    return;
  }
  jdl_return_handle(c, &LOCAL_TALLY, t, sizeof *t);
}

static const jello_jdll_export EXPORTS[] = {
  {"tally_new", tally_new, JELLO_T_DYNAMIC, 0, {0}, 0},
  {"tally_add", tally_add, JELLO_T_I64, 2, {JELLO_T_DYNAMIC, JELLO_T_I64}, 0},
  {"tally_reset", tally_reset, JELLO_T_VOID, 1, {JELLO_T_DYNAMIC}, 0},
  {"local_tally_new", local_tally_new, JELLO_T_DYNAMIC, 0, {0}, 0},
};

JELLO_JDLL_LIBRARY(tally, EXPORTS);
```

A shared tally, with four jobs each adding to it a thousand times:

```jello native=tally
// expect: 4000, then 1 after a reset
import std.concurrent.Job as Job;
import type { Job } from std.concurrent.Job;
load { tally_new, tally_add, tally_reset } from tally;

typedef Tally = abstract;
let newTally: () -> Tally = tally_new;
let add: (Tally, I64) -> I64 = tally_add;
let reset: (Tally) -> Void = tally_reset;

let votes = newTally();
let counters = Array.new<Job<Bool>>(0);
while (counters.len() < 4) {
  counters.push(Job.start<Bool>(fn(): Bool {
    let i = 0;
    while (i < 1000) { add(votes, 1i64); i++; }
    true
  }));
}
for (counter in counters) { counter.join(); }
let counted = add(votes, 0i64);
reset(votes);
`${counted}, then ${add(votes, 1i64)} after a reset`
```

And a local one, which cannot go along:

```jello native=tally
// stderr: trap UNSENDABLE
import std.concurrent.Job as Job;
load { local_tally_new } from tally;

typedef LocalTally = abstract;
let newLocal: () -> LocalTally = local_tally_new;

let share = fn(): Bool {
  let mine = newLocal();
  Job.start<Bool>(fn(): Bool { mine == mine }).join()
};
share()
```

The two classes have two types, so the compiler keeps a local tally out of `add`, whose C would
otherwise refuse it at run time:

```jello native=tally
// stderr: wrong argument type for add: the parameter is Tally, but this argument is LocalTally
load { tally_add, local_tally_new } from tally;

typedef Tally = abstract;
typedef LocalTally = abstract;
let newLocal: () -> LocalTally = local_tally_new;
let add: (Tally, I64) -> I64 = tally_add;

add(newLocal(), 1i64)
```

## Blocking calls

Jello runs many processes on a few threads, its *schedulers*. A process that calls a native
keeps its scheduler's thread busy until the native returns, so a native that waits, for a
network reply, a file on a slow disk or a child program, would hold up every process waiting
for that scheduler.

Mark such an export `JELLO_JDLL_BLOCKING` in its table's `flags`. A process calling it then
*parks*, like a process waiting for a message, while a thread from a separate pool makes the
call; its scheduler runs other processes meanwhile, and the process carries on once the call
returns.

```c native=sleepy
#include <jello/jdll.h>

#include <time.h>

/* Sleeps for the milliseconds given; true. */
static void nap(jdlo_ctx* c) {
  int32_t ms = jdl_arg_i32(c, 0);
  struct timespec wait = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&wait, NULL);
  jdl_return_bool(c, 1);
}

static const jello_jdll_export EXPORTS[] = {
  {"nap", nap, JELLO_T_BOOL, 1, {JELLO_T_I32}, JELLO_JDLL_BLOCKING},
};

JELLO_JDLL_LIBRARY(sleepy, EXPORTS);
```

Four cats each nap for 300 milliseconds. This page's examples run on a single scheduler, and the
naps still overlap: the four together take far less than the 1200 milliseconds one after another
would.

```jello native=sleepy
// expect: all four cats woke up in under a second
import std.concurrent.Job as Job;
import std.concurrent.Process as Process;
import type { Job } from std.concurrent.Job;
load { nap } from sleepy;

let napFor: (I32) -> Bool = nap;

let started = Process.now();
let cats = Array.new<Job<Bool>>(0);
while (cats.len() < 4) { cats.push(Job.start<Bool>(fn(): Bool { napFor(300) })); }
for (cat in cats) { cat.join(); }
let took = Process.now() - started;
took < 1000 ? "all four cats woke up in under a second" : `the cats took ${took} ms`
```

Mark an export blocking when it can wait on something outside the program. Leave quick ones,
like everything else on this page, unmarked: handing a call to another thread costs far more
than adding two numbers.

## The rules

A native runs inside the VM, with nothing between it and the VM's memory. These rules keep it
safe. Most are about one fact: while a native runs, the VM carries on around it, and its garbage
collector may move or free anything the native has not been told it can keep.

1. **Don't make Jello objects.** An export reads arguments and returns results, and the VM makes
   the result's object after the export returns. Only `jdl_*` calls, and the `jello_make_*`,
   `jello_is_*` and `jello_as_*` helpers, touch Jello values; never call `jello_gc_alloc` or
   other `jello.h` functions that allocate.
2. **Borrow, don't keep.** An argument's `Bytes` pointer, an object from a `Dynamic` argument
   and the `jdlo_ctx` itself are good until the export returns. Copy what you need to keep.
3. **Only call `jdl_*` from inside an export.** Not from a thread you started, not from a C
   constructor or `atexit` handler, and not from a `finalize`. They all need a call in progress,
   or the table the VM hands over when it loads the library.
4. **Expect several threads.** Different processes can call the same export at the same time,
   on different threads. Globals the library writes need a lock or atomics; read-only tables are
   fine.
5. **Return exactly once, with the declared kind.** If several `jdl_return_*` calls run, the last
   wins; if none does, or the kind is wrong, the call traps. Only a `Void` export may simply
   return.
6. **Never let a C++ exception or a `longjmp` escape an export.** Catch it inside and report it.
7. **Take the time you need, but say so.** A native that may wait is
   [blocking](#blocking-calls).

## Packaging a library

### Wrap it in a module

A program can `load` natives directly, as every example so far does, but a library is nicer to
use behind a Jello module that gives it Jello names, Jello types and Jello errors. The module
loads the natives, binds them, and exports what a user should call:

```jello native=ports file=net/Port.jello
package net;

load { parse_port } from ports;

let _parse: (Bytes) -> Dynamic = parse_port;

// The port TEXT spells, 1 to 65535; anything else throws.
export let parse = fn(text: Bytes): I32 {
  let port = _parse(text);
  if (port == null) { throw `not a port: ${text}`; }
  port as I32
};

// Whether TEXT spells a port.
export let isPort = fn(text: Bytes): Bool { _parse(text) != null };
```

```jello native=ports
// expect: 443 false
import net.Port as Port;

Std.string(Port.parse("443")) + " " + Std.string(Port.isPort("99999"))
```

A program using `net.Port` never sees a `load`, a `Dynamic` or a null.

A library with handles exports their abstract types beside its functions, with
`export typedef Tally = abstract;`, and a program imports them as it would any type,
`import type { Tally } from counting.Tally;`. Bind each native in that one module: a native has
one type, and two modules binding it with abstract types of their own is error E0213. Wrapping a
handle in a prototype, `typedef Counter = { let tally: Tally; };`, gives it methods.

### Where the VM looks

When a program starts, the VM looks for `name.jdll` in these places, in order, and takes the
first it can open (the [jello](02-jello.md#native-libraries) page has the details):

1. `jdll/` beside the program's `.jlo`;
2. the directory of the `.jlo` itself;
3. each directory given with `jello --jdll-path`;
4. the current directory;
5. each directory in the `JELLO_JDLL_PATH` environment variable;
6. the directory `jello` is in, and then `../lib` from there.

Outside a project, `jelloc run file.jello` compiles the program somewhere out of the way, so it
adds `file.jello`'s directory, and `jdll/` in it, to the search: libraries beside the source are
found, as in this page's examples.

In a [project](03-projects.md), put libraries in `jdll/` at the project's root, or name another
directory with `"jdll"` in `jello.json`. A package that ships natives does the same in its own
`jello.json`, and `jelloc run` and `jelloc test` pass every such directory to `jello`.

### Shipping it

- `jello --boot --bundle -o dist/app main.jlo` makes an executable and copies every library the
  program uses, `std.jdll` included, into `dist/jdll/`, so the `dist` directory can be zipped and
  shipped as it is.
- A `.jdll` is built for one system and one processor, so ship one per target. On macOS you can
  build both processors into one file with `-arch arm64 -arch x86_64`.
- Rebuild your libraries when you upgrade Jello to a version with a new ABI; the VM tells you
  when (see below).

## Troubleshooting

Each message below is exactly what Jello prints, with the names from this page's examples.

| You see | It means | Do this |
|---------|----------|---------|
| `native add cannot pass U8; natives take and return Bool, I32, I64, F32, F64, Bytes, Dynamic or an abstract type, and may return Void [E0213]` | the `let` uses a type natives cannot take | use one of the seven or an abstract type, or `Void` for a result, converting in a wrapper |
| `a native needs a function type: let f: (Bytes) -> I32 = ...; [E0213]` | the `let` has no type | write the function type |
| `cannot find hello.jdll in ./jdll, ., ...` | the file is in none of the places the VM looks | move it, or pass `--jdll-path` |
| `./jdll/hello.jdll is not a hello library` | the file has no manifest named `hello` | check the name in `JELLO_JDLL_LIBRARY` matches the file's |
| `./jdll/hello.jdll was built for ABI 3; rebuild it against this VM's jello/jdll.h (ABI 4)` | the library is from an older Jello | rebuild it with the current headers |
| `hello.jdll exports no function greeet` | `load` names a function that is not in the table | check the spelling on both sides |
| `native add from hello is bound as (I64, I64) -> I64, but the library exports (I32, I32) -> I32` | the `let`'s type and the table disagree | change one to match the other |
| `trap TYPE_MISMATCH` at a native call | the export called `jdl_fail`, returned nothing, returned the wrong kind, was given a handle of another class, or returned something other than a handle for an abstract type; the line after the trap's, such as `jello: native add returned nothing`, says which | read the export's code paths, or run its C under a debugger |
| `trap NULL_DEREF` at a native call | an export bound to return an abstract type, not `?Name`, returned null | bind it with `?Name`, or make it fail instead |
| `trap UNSENDABLE` | a handle whose class has no `retain` left its process | keep it in one process, or make the payload thread-safe and add `retain` |

A file the system cannot open at all, such as a library built for another processor, is passed
over if a later directory has one that opens; if none does, the VM says why the last one would
not, in the system's words:

```text
cannot open jdll/hello.jdll: dlopen(jdll/hello.jdll, 0x0006): tried: 'jdll/hello.jdll' (mach-o file, but is an incompatible architecture (have 'x86_64', need 'arm64')) ...
```

`file jdll/hello.jdll` tells you what a library was built for.

When a native crashes, run the program with `jello --no-jit`, under a debugger such as `lldb` or
`gdb`, with the library built with `-g`.

## Cheat sheet

```c
#include <jello/jdll.h>

static void my_export(jdlo_ctx* c) {
  /* Arguments, numbered from 0. */
  int     b   = jdl_arg_bool(c, 0);
  int32_t i   = jdl_arg_i32(c, 1);
  int64_t l   = jdl_arg_i64(c, 2);
  float   f   = jdl_arg_f32(c, 3);
  double  d   = jdl_arg_f64(c, 4);
  const uint8_t* data = jdl_arg_bytes_data(c, 5);   /* borrowed, not NUL-terminated */
  uint32_t       len  = jdl_arg_bytes_len(c, 5);
  jello_value    v    = jdl_arg_value(c, 6);         /* a Dynamic */
  void* payload = jdl_arg_handle(c, 7, &MY_CLASS);   /* NULL: null, or the call has failed */

  /* Exactly one result. */
  jdl_return_i32(c, 42);       /* also _bool, _i64, _f32, _f64 */
  jdl_return_bytes_copy(c, data, len);
  jdl_return_value(c, jello_make_i32(7));   /* or _bool, _atom; or an argument's object */
  jdl_return_null(c);
  jdl_return_handle(c, &MY_CLASS, payload, size_estimate);
  jdl_fail(c, "a bug");        /* traps TYPE_MISMATCH; cannot be caught */
  /* An export whose result is JELLO_T_VOID just returns. */
}

static const jello_jdll_export EXPORTS[] = {
  /* name, function, result, arity, argument kinds, flags (0 or JELLO_JDLL_BLOCKING) */
  {"my_export", my_export, JELLO_T_I32, 2, {JELLO_T_I32, JELLO_T_BYTES}, 0},
};

JELLO_JDLL_LIBRARY(mylib, EXPORTS);   /* the file is mylib.jdll */
```

```jello
// expect: 3 <Pattern>
// In Jello: load the natives, then give each a type; an abstract type for each class of handle.
load { unicode_length, regexp_compile } from std;
typedef Pattern = abstract;
let length: (Bytes) -> I32 = unicode_length;
let compile: (Bytes, Bytes) -> Pattern = regexp_compile;
`${length("abc")} ${compile("a+", "")}`
```

Also in the header, for the curious: `jdl_program_arg_count` and `jdl_program_arg` read the
program's command-line arguments, as `System.args` does, and `jdl_object_get` reads one field of
an `Object` by its atom.
