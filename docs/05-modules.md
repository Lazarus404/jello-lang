# Modules

Every source file is a module. A program is one entry module, the file given to the compiler,
together with every module it imports, directly or through other modules. A module keeps its
top-level names to itself unless it marks them `export`, and another module takes them with
`import`.

```jello file=geometry/Shapes.jello
package geometry;

export enum Shape { Square(side: I32); Circle(radius: I32); }

export let area = fn(shape: Shape): I32 {
  match (shape) { Square(side) => side * side, Circle(radius) => 3 * radius * radius }
};
```

```jello
// expect: 9 12
// flags: --path $DIR
import { Shape, area } from geometry.Shapes;

Std.string(area(Shape.Square(3))) + " " + Std.string(area(Shape.Circle(2)))
```

In the examples on this page, a block headed `file=PATH` is a module at that path, beside the
program that follows it.

## Module paths and packages

A module's import path is its file's path under a source root, with dots for slashes and without
`.jello`: `geometry.Shapes` is the file `geometry/Shapes.jello`. The roots are described in
[Source roots](#source-roots).

An imported module starts with a `package` declaration naming every part of its path but the
last, so `geometry/Shapes.jello` declares `package geometry;` and `a/b/C.jello` declares
`package a.b;`. A module whose path has one part, such as `Shapes.jello` imported as `Shapes`,
declares no package. The entry module is not imported, so it needs no declaration.

A declaration that does not match the path the module is imported by is an error, reported at
the import, with a note at the declaration:

```jello file=geometry/Points.jello
package geo;

export let origin = 0;
```

```jello
// error: 3:1: $DIR/geometry/Points.jello declares `package geo;`, but importing it as `geometry.Points` needs `package geometry;` [E0202]
// flags: --path $DIR
import { origin } from geometry.Points;

origin
```

Because the package fixes a module's path, one file has one import path. Importing the same file
by two paths, which two overlapping roots make possible, is an error (E0207).

## Exports and privacy

`export` goes before a top-level declaration to let other modules import it:

| Declaration | Exports |
|-------------|---------|
| `export let name = ...;`, `export const name = ...;` | a value |
| `export let (a, b) = ...;` | each name the pattern binds |
| `export enum Name { ... }` | the type and its constructors |
| `export typedef Name = ...;` | a type: an alias, a record or a bytes layout |
| `export prototype Name { ... };` | the prototype, its type and its methods |
| `export macro name(...) { ... }` | a macro |

Every other top-level name is private to its module. Importing one is the same error as
importing a name that does not exist:

```jello file=counting/Steps.jello
package counting;

let size = 2;
export let step = fn(n: I32): I32 { n + size };
```

```jello
// error: 3:1: unknown export size in $DIR/counting/Steps.jello [E0208]
// flags: --path $DIR
import { size } from counting.Steps;

size
```

The rules:

- `export` applies only to top-level declarations. Before anything else, such as an expression
  statement, it is an error (E0107).
- `export let _ = ...;` binds nothing, so it exports nothing.
- A typedef and the prototype of the same name are linked, so both are exported or neither is
  (E0804).
- A module exports only what it declares. To pass on a name it imports, it declares its own:
  `export let area = S.area;` after `import geometry.Shapes as S;`, for example.

## Importing

Imports are top-level statements. They are usually written first, but may come anywhere at the
top level; a name an import brings in is in scope in the whole module. An import inside a
function body is an error.

### Named imports

```text
import { name, other as alias, } from a.b.Module;
```

`import { ... } from path;` binds each named export of the module. `as` gives a binding a local
name, and the list may end with a comma.

```jello file=text/Words.jello
package text;

export const separator = ", ";
export let wrap = fn(word: Bytes): Bytes { "[" + word + "]" };
```

```jello
// expect: [a], [b]
// flags: --path $DIR
import { separator as sep, wrap } from text.Words;

wrap("a") + sep + wrap("b")
```

An imported name is the exporting module's own binding, not a copy. Reads see its current value,
and only its module can assign to it, so a module can keep state that others read:

```jello file=tally/Counter.jello
package tally;

export let count = 0;
export let bump = fn(): I32 { count = count + 1; count };
```

```jello
// expect: 2
// flags: --path $DIR
import { bump, count } from tally.Counter;

bump();
bump();
count
```

Assigning to an imported name, as `count = 5;` here would, is an error: `cannot assign to
imported binding: count` (E0303).

Two imports that bind the same local name are an error (E0205), as is a top-level declaration
with the name of an import (E0302). One export may be imported twice under different names.

### Type imports

```text
import type { Name, Other as Alias } from a.b.Module;
```

Types are named apart from values. `import type` brings in exported typedefs, records, bytes
layouts and prototypes as types; a value named in `import type` is an error (E0208).

```jello file=shop/Item.jello
package shop;

export typedef Item = { let name: Bytes; let price: I32; };

export let total = fn(items: Array<Item>): I32 {
  let sum = 0; let i = 0;
  while (i < items.len()) { sum += items[i].price; i++; }
  sum
};
```

```jello
// expect: 7
// flags: --path $DIR
import type { Item } from shop.Item;
import { total } from shop.Item;

let basket: Array<Item> = [{ name: "tea", price: 3 }, { name: "jam", price: 4 }];
total(basket)
```

An enum is both: `import { Shape }` binds its constructors and also names its type, as the first
example on this page shows. Writing `import type { Shape }` beside it is allowed and changes
nothing.

A prototype is likewise reached through either form; `new` and the prototype's methods work
wherever its type is imported:

```jello file=boxes/Box.jello
package boxes;

export typedef Box = { let w: I32; let h: I32; };
export prototype Box {
  init(w: I32, h: I32) { this.w = w; this.h = h; };
  area(): I32 { this.w * this.h };
};
```

```jello
// expect: 10
// flags: --path $DIR
import type { Box } from boxes.Box;

let box = new Box(2, 5);
box.area()
```

### Namespace imports

```text
import a.b.Module as Alias;
```

A namespace import binds one name, and `Alias.name` then names the value export `name`. It means
exactly what `import { name }` would: reads are live and assignment is an error.

```jello file=ops/Ops.jello
package ops;

export let twice = fn(n: I32): I32 { n * 2 };
export let count = 0;
export let bump = fn(): I32 { count += 1; count };
```

```jello
// expect: 41
// flags: --path $DIR
import ops.Ops as O;

O.bump();
O.twice(20) + O.count
```

The rules:

- A namespace names values only. A type, an enum's constructors among them, is imported with
  `import type { T }` or `import { E }`; `O.Shape` is an error (E0206).
- The namespace is not itself a value; `let m = O;` is an error (E0602).
- A local of the same name, such as a parameter called `O`, shadows the namespace.
- `import type a.b.M as M;` is an error: a namespace import names values (E0206).
- `import type { T } from a.b.T;` and `import a.b.T as T;` may bind the same name, since one
  names a type and the other values. This is how a prototype's type and its module's functions
  share a name, as in `Process<M>` and `Process.spawn`.

### Macro imports

`import macro { name, other as alias } from a.b.Module;` imports exported macros by name. A macro
cannot be imported through a namespace. See [Macros](03-statements/05-macros.md).

## Source roots

The compiler looks for a module under a list of source roots, in order, and takes the first file
that exists:

1. the entry module's directory;
2. each `--path` directory, in the order given on the command line;
3. in a project, under `jelloc run` and `jelloc test`, the project's `src` and then each
   dependency's (see [Projects](07-tooling/03-projects.md)).

A module whose path starts with `std` is only looked for in the standard library's directory,
given by `--std dir` or else the `JELLO_STD_PATH` environment variable; `std.io.File` is the file
`io/File.jello` there. Without either, importing a `std` module is an error (E0201).

A module found under no root is an error (E0200) that lists every file the compiler tried:

```text
main.jello:1:1: module geometry.Nope not found: ./geometry/Nope.jello, lib/geometry/Nope.jello [E0200]
```

## Native libraries

Functions written in C come from native libraries, `.jdll` files that the VM loads. `load` names
the functions a library exports, and a typed `let` binds each one:

```jello
// expect: 5 true
load { unicode_length, unicode_validate as valid } from std;

let length: (Bytes) -> I32 = unicode_length;
let isText: (Bytes) -> Bool = valid;

Std.string(length("héllo")) + " " + Std.string(isText("héllo"))
```

`load { name [as alias], ... } from library;` takes the same forms as a named import. The library
is one identifier: `std` is the standard native library, `std.jdll`, and any other name is the
file `name.jdll`, which the VM finds when the program runs (see
[jello](07-tooling/02-jello.md#native-libraries)).

A name from `load` is not yet callable. It must first be bound with a function type, and the
binding is what the program calls:

```jello
// error: 3:1: native unicode_length needs a typed binding first: let f: (Bytes) -> I32 = unicode_length; [E0213]
load { unicode_length } from std;
unicode_length("abc")
```

The rules for the binding:

- The `let` has a function type annotation; `let length = unicode_length;` is an error (E0213).
- Parameters and the result are `Bool`, `I32`, `I64`, `F32`, `F64`, `Bytes` or `Dynamic`, and
  there are at most 8 parameters. The result may also be `Void`.
- Every binding of one native gives it the same type.
- The binding can only be called. It cannot be assigned, or passed or stored as a function
  value; wrap it in a function to do that: `let f = fn(s: Bytes): I32 { length(s) };`.
- The binding can be exported, and importers call it as they call any imported function.

A `load` is a top-level statement, and a namespace form of it, `load std as S;`, is an error
(E0206). [Native libraries](07-tooling/06-native-libraries.md) shows how to write a library of
your own.

## Initialisation order

A program runs its modules' top-level statements once each, a module before the modules that
import it, and the entry module last. Modules are reached in the order of their imports, depth
first, so a module's dependencies run in the order its import statements name them.

```jello file=app/Log.jello
package app;

export let entries = Array.new<Bytes>(0);
export let note = fn(text: Bytes): Null { entries.push(text); null };
```

```jello file=app/Config.jello
package app;

import { note } from app.Log;

note("config");
export let name = "demo";
```

```jello file=app/Server.jello
package app;

import { note } from app.Log;
import { name } from app.Config;

note("server " + name);
export let port = 8080;
```

```jello
// expect: config, server demo, main 8080
// flags: --path $DIR
import { port } from app.Server;
import { entries, note } from app.Log;
import { name } from app.Config;

note("main " + Std.string(port));
Bytes.join(entries, ", ")
```

Here `app.Server` imports `app.Log` and then `app.Config`, so they run first, in that order, and
then `app.Server` itself. The entry's later imports of `app.Log` and `app.Config` find them
already run. A module runs at most once however many modules import it, and every importer sees
the same bindings.

A module that is only reached through `import type` does not run, since nothing of its values is
used, unless a type it gives is a prototype: then the prototype's module runs, so that `new` and
the methods work.

Reading a module's global before it is initialised is an error, found at compile time; see
[Bindings](03-statements/01-bindings.md).

## Import cycles

Modules cannot import each other in a cycle, directly or not, since none of them could run
first. The error is reported at the import that closes the cycle:

```jello file=parity/Even.jello
package parity;

import { odd } from parity.Odd;
export let even = fn(n: I32): Bool { n == 0 ? true : odd(n - 1) };
```

```jello file=parity/Odd.jello
package parity;

import { even } from parity.Even;
export let odd = fn(n: I32): Bool { n == 0 ? false : even(n - 1) };
```

```jello
// error: parity/Odd.jello:3:1: import cycle through $DIR/parity/Even.jello [E0203]
// flags: --path $DIR
import { even } from parity.Even;

even(10)
```

A type-only import counts too, and so does a module importing itself. Functions that call each
other, and types that refer to each other, go in one module, or what two modules share moves into
a third that both import.

## The entry module and its result

The entry module's last statement may be an expression without a `;`; its value is the
program's result, which `jello` prints (see [jello](07-tooling/02-jello.md)). Only the
entry module has a result, so an imported module that ends with an expression is an error:

```jello file=values/Numbers.jello
package values;

export let one = 1;

one
```

```jello
// error: values/Numbers.jello:5:1: an imported module cannot end with an expression, since only the program's entry module has a result; end it with `;` [E0204]
// flags: --path $DIR
import { one } from values.Numbers;

one
```

## Test files and private names

A test file, named `NAME.test.jello`, is a module whose import path ends in `NAME.test`, which no
import can name. When it imports the module it tests, the one with the same path less `.test`,
it may import that module's private names as well as its exports:

```text
// src/app/Names.jello
package app;

let _default = "world";
export let pick = fn(args: List<Bytes>): Bytes { List.is_nil(args) ? _default : List.head(args) };
```

```text
// tests/app/Names.test.jello: its path is app.Names.test, so it tests app.Names
package app;

import type { Test } from std.testing;
import std.testing as T;
import { _default, pick } from app.Names;

let testDefault = fn(t: Test) {
  T.equal(t, pick(List.nil<Bytes>()), _default);
  null
};
```

Any other module importing `_default` gets the unknown-export error. See
[Testing](07-tooling/04-testing.md).
