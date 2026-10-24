# Typedefs and records

`typedef` gives a name to a type. It has four forms:

```text
typedef Name = Type;                                           an alias
typedef Name = { let field: Type; ... };                       a record
typedef Name = bytes-layout { let field: layout-type; ... };   a byte layout
typedef Name = abstract;                                       an abstract type
```

A typedef is declared at the top level of a module, may take type parameters (except a byte
layout or an abstract type), and may be exported with `export typedef`; another module imports
it with `import type { Name } from path;` (see [Modules](../05-modules.md)). Its name may not be
used for another top-level declaration, and the order of typedefs does not matter: one may refer
to a typedef declared after it.

## Aliases

An alias is another name for an existing type. Values of the alias and of the type it names are
interchangeable.

```jello
// expect: 3 b true
typedef Ints = Array<I32>;
typedef Many<T> = Array<T>;
typedef Two<A, B> = Tuple<A, B>;

let xs: Ints = [1, 2];
let words: Many<Bytes> = ["a", "b"];
let pair: Two<I32, Bool> = (1, true);
Std.string(xs[0] + xs[1]) + " " + words[1] + " " + Std.string(pair.1)
```

A generic alias must be given its type arguments wherever it is used, and an alias may not refer
to itself, directly or through other aliases:

```jello
// error: 3:1: typedef A refers to itself [E0804]
typedef A = Array<B>;
typedef B = A;
0
```

## Records

A record typedef lists named, typed fields. A record value is written as an object literal where
the context expects the record type: an annotated `let`, an argument, a return value, a field of
another record, or an array item.

```jello
// expect: 6 9 {x: 6, y: 9}
typedef Point = { let x: I32; let y: I32; };

let p: Point = { x: 1, y: 2 };
p.x = 5;
p.x++;
p.y += 7;
Std.string(p.x) + " " + Std.string(p.y) + " " + Std.string(p)
```

The literal must give every field once, each with a value of the field's type; there are no
default values, so a nullable field is given `null` explicitly. The shorthand `{ x, y }` takes
each field from the variable of the same name. Fields are evaluated in the order they are
written. A literal cannot have computed keys, and it cannot have fields the record does not
declare.

```jello
// error: 3:15: the record literal of User is missing field name [E0801]
typedef User = { let age: I32; let name: Bytes; };
let u: User = { age: 1 };
0
```

```jello
// error: 4:7: field x of Point is I32, but this value is Bytes [E0400]
typedef Point = { let x: I32; };
let p: Point = { x: 1 };
p.x = "one";
0
```

Without a record context, a literal in braces makes an `Object`, whose fields read as `Dynamic`;
see [Collections](03-collections.md#objects).

A record type's name is a type only; `Point` alone, or `Point.Point(...)`, is an error.

### Fields, updates and sharing

A field is read with `r.field` and assigned with `=`, a compound assignment, `++` or `--` (on an
integer field). A field that holds a function is called as `r.field(args)`.

A record value is a reference: assigning it to another variable shares it, and a change through
one is seen through the other. To make a changed copy, write a literal that starts with a
spread of the original, followed by the fields that differ. The spread must be the first entry,
and must be a record of the same type; a record of another type with the same fields does not
spread.

```jello
// expect: {x: 1, y: 2} {x: 1, y: 7} 42
typedef Point = { let x: I32; let y: I32; };
typedef Handler = { let run: (I32) -> I32; };

let p: Point = { x: 1, y: 2 };
let moved: Point = { ...p, y: 7 };
let h: Handler = { run: fn(v: I32) { v * 2 } };
Std.string(p) + " " + Std.string(moved) + " " + Std.string(h.run(21))
```

```jello
// error: 4:27: a spread must be the first entry of a record literal [E0801]
typedef Point = { let x: I32; let y: I32; };
let p: Point = { x: 1, y: 2 };
let q: Point = { x: 3, ...p };
0
```

### Equality and printing

`==` on two records of the same type compares their fields, structurally; `Std.string` shows the
fields in declaration order.

```jello
// expect: true false
typedef Named = { let x: I32; let name: Bytes; };
let a: Named = { x: 1, name: "a" };
let b: Named = { x: 1, name: "a" };
let c: Named = { x: 2, name: "a" };
Std.string(a == b) + " " + Std.string(a == c)
```

### Recursive and generic records

A record may hold itself, usually through a nullable field, and may take type parameters:

```jello
// expect: 6 two 1
typedef Node = { let value: I32; let next: ?Node; };
typedef Pair<A, B> = { let first: A; let second: B; };

let sum = fn(node: ?Node): I32 {
  let total = 0;
  let at = node;
  while (at != null) { total += at.value; at = at.next; }
  total
};
let swap<A, B> = fn(p: Pair<A, B>): Pair<B, A> { { first: p.second, second: p.first } };

let list: Node = { value: 1, next: { value: 2, next: { value: 3, next: null } } };
let pair: Pair<I32, Bytes> = { first: 1, second: "two" };
let swapped = swap(pair);
Std.string(sum(list)) + " " + swapped.first + " " + Std.string(swapped.second)
```

### Record patterns

A pattern in braces matches a record by its fields. `{ x, y }` binds fields to names of their
own, `{ x: a }` binds a field to another name, and `{ x: 0 }` tests a field's value. Fields the
pattern does not name are ignored.

```jello
// expect: 1 2 y2 x1
typedef Pt = { let x: I32; let y: I32; };
let p: Pt = { x: 1, y: 2 };
let { x: a, y } = p;
let named = match (p) { { x: 0 } => "x0", { y: 2, x } => "y2 x" + Std.string(x), _ => "other" };
Std.string(a) + " " + Std.string(y) + " " + named
```

See [Patterns](../03-statements/03-patterns.md) for the rules of `match`, `let` and `if (let ...)`
with these patterns.

### Linked typedefs

A record typedef and a prototype with the same name are linked: the prototype constructs the
values with `new`, and the typedef gives their fields static types, so they read and write as
those types instead of as `Dynamic`. Both must take the same type parameters and be exported
together. See [Objects and prototypes](../04-expressions/03-objects-and-prototypes.md).

```jello
// expect: 3 4
typedef Counter = { let count: I32; };
prototype Counter {
  init(start: I32) { this.count = start; };
  bump(): I32 { this.count++; this.count };
};
let c = new Counter(1);
c.bump();
let n: I32 = c.bump();
// The field is an I32, so arithmetic on it needs no conversion.
Std.string(n) + " " + Std.string(c.count + 1)
```

## Byte layouts

A `bytes-layout` typedef describes how a record is laid out in `Bytes`, field after field. The
layout gets two functions: `Name.pack(record)` encodes a record as `Bytes`, and
`Name.unpack(bytes)` decodes `Bytes` into a nullable record, which is null when the bytes do not
have the layout's length.

```jello
// expect: 5 [1, 2, 0, 104, 105] 2 hi
typedef Message = bytes-layout { let kind: u8; let length: u16.le; let body: rest; };

let raw = Message.pack({ kind: 1, length: 2, body: "hi" });
let shown = [];
let i = 0;
while (i < raw.len()) { shown.push(Bytes.get_u8(raw, i)); i++; }
let decoded = Message.unpack(raw);
let length = decoded == null ? 0 : decoded.length;
let body = decoded == null ? "" : decoded.body;
Std.string(raw.len()) + " " + Std.string(shown) + " " + Std.string(length) + " " + body
```

The field types are:

| Type | Field value | Size |
|------|-------------|------|
| `u8` | unsigned integer | 8 bits |
| `u16`, `u32` | unsigned integer | 16 or 32 bits |
| `i32` | signed integer | 32 bits |
| `f32` | `F32` float | 32 bits |
| `bits(N)` | unsigned integer, `N` from 1 to 7 | `N` bits |
| `rest` | `Bytes`: everything after the fields before it | the remaining bytes |

The type names may also be written in capitals (`U16`). A 16- or 32-bit field may have a byte
order, `.le` (little-endian, the default) or `.be` (big-endian). The fields pack in order, as the
segments of a `bits[...]` expression do (see [Bits](../04-expressions/04-bits.md)), so:

- the fields must fill whole bytes, and a field of 8 bits or more, or a `rest`, must start on a
  byte boundary;
- a `rest` field, if any, is last; without one, `unpack` needs exactly the layout's length, and
  with one it needs at least the length of the fields before it;
- a value too wide for its field, given as a literal to `pack`, is an error.

```jello
// expect: [37, 7]
typedef Header = bytes-layout { let version: bits(4); let flags: bits(4); let kind: u8; };
let raw = Header.pack({ version: 2, flags: 5, kind: 7 });
Std.string([Bytes.get_u8(raw, 0), Bytes.get_u8(raw, 1)])
```

```jello
// error: 2:1: a segment of 8 or more bits, or a rest, must start on a byte boundary [E0104]
typedef B = bytes-layout { let a: bits(4); let b: u8; let c: bits(4); };
0
```

A layout's name is also a pattern for `Bytes`: `Name(p)` matches bytes that unpack to the layout
and matches the record against `p`. A match on `Bytes` with layout patterns needs a `_` arm.

```jello
// expect: ping 7 | message hi | kind 9 | none
typedef Message = bytes-layout { let kind: u8; let length: u16.le; let body: rest; };
typedef Ping = bytes-layout { let kind: u8; let stamp: u32.be; };

let describe = fn(raw: Bytes): Bytes {
  match (raw) {
    Ping({ kind: 1, stamp }) => "ping " + Std.string(stamp),
    Message({ kind: 2, body }) => "message " + body,
    Message(m) => "kind " + Std.string(m.kind),
    _ => "none",
  }
};
describe(Ping.pack({ kind: 1, stamp: 7 })) + " | "
  + describe(Message.pack({ kind: 2, length: 2, body: "hi" })) + " | "
  + describe(Ping.pack({ kind: 9, stamp: 7 })) + " | " + describe("ab")
```

## Abstract types

An abstract type names a kind of thing that a [native library](../07-tooling/06-native-libraries.md)
keeps for a program: an open window, a database connection, a compiled regular expression. Jello
code cannot make one or look inside it. It gets one from a native, keeps it, and hands it back to
the natives that use it.

```text
typedef Window = abstract;
```

Each abstract typedef is a type of its own, even when another module declares one of the same
name. So a program cannot pass a `Renderer` where a `Window` is wanted, though at run time both
are native handles:

```jello
// expect: 3 true false
load { regexp_compile, regexp_text, regexp_match_at, regexp_capture_bytes } from std;

typedef Pattern = abstract;
typedef Text = abstract;
typedef Match = abstract;
let compile: (Bytes, Bytes) -> Pattern = regexp_compile;
let text: (Bytes) -> Text = regexp_text;
let matchAt: (Text, Pattern, I32) -> ?Match = regexp_match_at;
let group: (Match, I32) -> Bytes = regexp_capture_bytes;

let digits = compile("[0-9]+", "");
let found = matchAt(text("abc 123"), digits, 4);
let length = found == null ? 0 : group(found, 0).len();
Std.string(length) + " " + Std.string(digits == digits) + " "
  + Std.string(digits == compile("[0-9]+", ""))
```

```jello
// error: 7:9: wrong argument type for matchAt: the parameter is Text, but this argument is Pattern
load { regexp_compile, regexp_match_at } from std;
typedef Pattern = abstract;
typedef Text = abstract;
let compile: (Bytes, Bytes) -> Pattern = regexp_compile;
let matchAt: (Text, Pattern, I32) -> Dynamic = regexp_match_at;
matchAt(compile("x", ""), compile("x", ""), 0)
```

A value of an abstract type:

- comes from a native whose binding returns the type, and goes to natives whose bindings take it;
  a native that returns null instead traps `NULL_DEREF`, and one that returns anything but a
  native handle traps `TYPE_MISMATCH`;
- may be null only as `?Name`, which narrows as any nullable value does;
- compares with `==` and `!=` by identity: two values are equal only when they are the same
  handle;
- has no fields or methods, though a record, an array or a linked typedef may hold it, so a
  prototype can wrap it in methods;
- becomes a `Dynamic` with `as Dynamic`, and comes back with `as Name`, and `is Name` tests for
  one; a linked typedef's field of the type is checked as it is read, as `as Name` is;
- prints as its type's name, as `<Pattern>`, since only its library sees inside it.

Those checks see only that a value is a native handle, not which library's kind it is: a
`Dynamic` holding a regexp passes `as Text`. The library itself checks the kind, so the native
the value next reaches traps `TYPE_MISMATCH`, and C code never sees the wrong kind of thing.
