# Built-in namespaces and value methods

Some functions need no import. The namespaces `Std`, `System`, `Math`, `Float`, `Integer`,
`Array`, `Bytes`, `List`, `Object`, `Tuple`, `Atom`, `Enum` and `Function` are always in scope,
and values of the built-in types have methods. Most of these are *intrinsics*: the compiler checks
and compiles them itself, often to a single VM instruction. The rest come from the extension modules
`std.core.Array`, `std.core.Bytes`, `std.core.List` and `std.core.Object`, which the compiler loads
on its own the first time a program calls one of their functions (see
[the core modules](02-core.md)).

```jello
// expect: 3 [1, 2, 3, 4] 1.5 true
let xs = [1, 2, 3];
xs.push(4);
let parts = [Std.string(xs.len() - 1), Std.string(xs), Std.string(Math.max(1, 1.5)),
  Std.string("abc".startsWith("a"))];
parts.join(" ")
```

A namespace name is a binding like any other, so a local or an import of the same name hides it:
after `let Math = 3;`, `Math` is that integer.

## Calls as functions or as methods

A function of a type's namespace that takes a value of that type first can also be called as a
method of the value: `Array.len(xs)` and `xs.len()` are the same call, as are
`Bytes.trim(s)` and `s.trim()`. The tables below give the static form; where only one form works,
they say so.

When an intrinsic and an extension module function have the same name, the intrinsic wins.

## Std

| Function | Description |
| --- | --- |
| `Std.string(value): Bytes` | `value` as text. Bytes is itself; anything else prints as a literal |

`Std.string` prints numbers, `Bool`, `null`, atoms, tuples, arrays, lists, enums, records and
Objects; Bytes inside a collection print quoted. A function or a buffer from `Bytes.new` cannot be
printed: that is a compile error (E0500), and a buffer's bytes are read with `buffer.toString()`
first.

```jello
// expect: 42 2.5 true null ok ["a", "b"] (1, "one") Shade.Rgb(1, 2, 3) {x: 1}
enum Shade { Grey; Rgb(r: I32, g: I32, b: I32); }
let parts = [Std.string(42), Std.string(2.5), Std.string(true), Std.string(null), Std.string("ok"),
  Std.string(["a", "b"]), Std.string((1, "one")), Std.string(Shade.Rgb(1, 2, 3)),
  Std.string({ x: 1 })];
parts.join(" ")
```

## System

| Member | Description |
| --- | --- |
| `System.args: List<Bytes>` | the program's command-line arguments, after the module's path |
| `System.exit(status: I32)` | flushes output and exits with `status & 255`; never returns |
| `System.assert(condition: Bool)` | throws if `condition` is false |
| `System.assertEq(got, want)` | throws if `got != want` |
| `System.assertEq(got, want, message: Bytes)` | the same, adding `message` to what it throws |

`System.args` is a value, not a function. The assertions are expanded at each call: they
evaluate each argument once and throw Bytes naming the call's place, as `FILE:LINE:COL: assertion
failed` or `FILE:LINE:COL: assertEq failed: 1 != 2: message`. An uncaught one ends the program
with the trap `UNCAUGHT_THROW`.

```jello
// args: one two
// expect: 2 one
System.assert(List.head(System.args) == "one");
System.assertEq(List.len(System.args), 2, "two arguments");
Std.string(List.len(System.args)) + " " + List.head(System.args)
```

```jello
// status: 3
System.exit(3);
"never printed"
```

## Math

Every `Math` function takes numbers of any numeric type, converts them to `F64` and returns an
`F64`, except `clz32` and `imul`, which take and return `I32`.

| Function | Description |
| --- | --- |
| `abs`, `sign` | magnitude; -1, 0 or 1 (keeping -0 and NaN) |
| `ceil`, `floor`, `trunc`, `round` | rounding; `round` rounds a half away from zero |
| `fround` | the nearest `F32`, as an `F64` |
| `sqrt`, `cbrt` | square and cube roots |
| `exp`, `expm1`, `log`, `log10`, `log2`, `log1p` | exponentials and logarithms |
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan` | trigonometry, in radians |
| `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh` | hyperbolic functions |
| `pow(x, y)`, `atan2(y, x)` | two arguments |
| `min(x, ...)`, `max(x, ...)`, `hypot(x, ...)` | one argument or more; `min` and `max` are NaN if any argument is, and order -0 below +0 |
| `random()` | a number in [0, 1) |
| `clz32(x: I32): I32` | the number of leading zero bits of `x` |
| `imul(a: I32, b: I32): I32` | the low 32 bits of `a * b` |

`Math.PI` and `Math.E` are the `F64` constants nearest pi and e.

```jello
// expect: 3.0 1024.0 5.0 -3.0 31 0
let parts = [Std.string(Math.sqrt(9)), Std.string(Math.pow(2, 10)), Std.string(Math.hypot(3, 4)),
  Std.string(Math.round(-2.5)), Std.string(Math.clz32(1)), Std.string(Math.imul(65536, 65536))];
parts.join(" ")
```

Calling `Math.min`, `Math.max` or `Math.hypot` with no arguments is an error:

```jello
// error: 2:1: Math.min takes at least 1 argument [E0600]
Math.min()
```

## Float and Integer

| Function | Description |
| --- | --- |
| `Float.parse(text: Bytes): F64` | reads a number; `NaN`, `Infinity` and `-Infinity` included |
| `Float.is_nan(x): Bool` | whether `x` is NaN |
| `Float.is_infinite(x): Bool` | whether `x` is infinite |
| `Integer.parse(text: Bytes)` | reads an optional sign and decimal digits as the integer type the context requires, or `I32` |

Text that is not a number of the type throws Bytes such as `Float.parse: not a number` or
`Integer.parse: not U8`; a value out of the type's range, or a minus sign for an unsigned type,
counts as not a number.

```jello
// expect: 1500.0 true -17 200 Integer.parse: not U8
let big: U8 = Integer.parse("200");
let failed = try { let n: U8 = Integer.parse("256"); "" } catch (e) { e as Bytes };
let infinite = Float.is_infinite(Float.parse("-Infinity"));
let parts = [Std.string(Float.parse("1.5e3")), Std.string(infinite),
  Std.string(Integer.parse("-17")), Std.string(big), failed];
parts.join(" ")
```

## Arrays

| Function | Description |
| --- | --- |
| `Array.new<T>(length: I32): Array<T>` | a new array of `length` default values (0, `""`, `null`, ...) |
| `Array.len(xs): I32` | the number of items |
| `Array.push(xs, value): I32` | appends `value`; the new length |
| `Array.pop(xs): T` | removes and returns the last item |
| `Array.resize(xs, length: I32): Array<T>` | grows with default values or shrinks; `xs` |
| `Array.join(parts: Array<Bytes>, separator: Bytes): Bytes` | the parts with `separator` between each two |

`T` comes from an annotation or a type argument when `Array.new` cannot infer it. `pop` of an empty
array, and an index out of range, trap `BOUNDS`. `join` copies each part once, so it is the cheap
way to build text from pieces. The other array functions, such as `map`, `filter` and `sort`, are
in [std.core.Array](02-core.md#stdcorearray).

```jello
// expect: [1, 2, 0, 0] 3 [1, 2]
let xs: Array<I32> = Array.new(0);
xs.push(1);
Array.push(xs, 2);
xs.resize(4);
let shown = Std.string(xs);
xs.pop();
let n = xs.len();
xs.resize(2);
shown + " " + Std.string(n) + " " + Std.string(xs)
```

## Lists

A list is immutable and singly linked.

| Function | Description |
| --- | --- |
| `List.nil<T>(): List<T>` | the empty list; static only |
| `List.cons(head: T, tail: List<T>): List<T>` | a list of `head` before `tail`; static only |
| `List.head(xs): T` | the first item |
| `List.tail(xs): List<T>` | the items after the first |
| `List.is_nil(xs): Bool` | whether `xs` is empty |

The head or tail of the empty list traps. The other list functions are in
[std.core.List](02-core.md#stdcorelist).

```jello
// expect: [1, 2] 1 [2] false true
let xs = List.cons(1, List.cons(2, List.nil<I32>()));
let parts = [Std.string(xs), Std.string(xs.head()), Std.string(List.tail(xs)),
  Std.string(xs.is_nil()), Std.string(List.is_nil(xs.tail().tail()))];
parts.join(" ")
```

## Bytes

Bytes is an immutable sequence of bytes, which is how Jello holds text. `Bytes.new` makes a
`ByteBuffer`, a mutable buffer of zero bytes; its bytes leave it only as a copy, through `slice` or
`toString`. Indexes count bytes, not characters.

| Function | Description |
| --- | --- |
| `Bytes.len(b): I32` | the number of bytes, of Bytes or a buffer |
| `Bytes.get(b, i: I32): I32` | the byte at `i`, 0 to 255; `get_u8` is the same |
| `Bytes.slice(b, start: I32, length: I32): Bytes` | a copy of `length` bytes from `start` |
| `Bytes.eq(a: Bytes, b: Bytes): Bool` | whether they hold the same bytes, as `==` does |
| `Bytes.new(length: I32): ByteBuffer` | a buffer of `length` zero bytes |
| `Bytes.set(buffer, i: I32, byte: I32)` | writes a byte, 0 to 255, into a buffer; `set_u8` is the same |
| `Bytes.toString(buffer): Bytes` | a copy of a buffer's bytes |

An index out of range traps `BOUNDS`. Calling `set` on Bytes rather than a buffer is a compile
error. Bytes also has `+` for concatenation, and a one-character literal has `.code`, its Unicode
code point as an `I32`. The other functions, such as `split`, `trim` and `indexOf`, are in
[std.core.Bytes](02-core.md#stdcorebytes).

```jello
// expect: 6 5 104 ell HELLO 65 233
let word = "héllo";
let buffer = Bytes.new(5);
let i = 0;
while (i < 5) { buffer.set(i, "hello".get(i) - 32); i++; }
let parts = [Std.string(word.len()), Std.string(Bytes.len("hello")), Std.string("hello".get(0)),
  "hello".slice(1, 3), buffer.toString(), Std.string("A".code), Std.string("é".code)];
parts.join(" ")
```

```jello
// error: 3:1: Bytes.set requires a buffer from Bytes.new; Bytes values are immutable [E0400]
let text = "abc";
text.set(0, 65);
text
```

## Objects

An Object maps atoms to values of any type. These functions are intrinsics; `len`, `isEmpty`,
`merge` and `values` are in [std.core.Object](02-core.md#stdcoreobject).

| Function | Description |
| --- | --- |
| `Object.get(o, key): Dynamic` | a field's value, or `null`; `get<T>` reads it as a `T`, which the VM checks |
| `Object.set(o, key, value)` | writes a field; the value |
| `Object.exists(o, key): Bool` | whether `o` has the field, its own or one it delegates to |
| `Object.remove(o, key): Bool` | removes an own field; whether there was one |
| `Object.clear(o): Object` | removes every field; `o` |
| `Object.copy(o): Object` | a shallow copy |
| `Object.keys(o): Array<Atom>` | the own keys |
| `Object.toString(o): Bytes` | `o` as `{a: 1}` |

A key is an atom, `:name`, a Bytes literal or any `Atom` value. A method call `o.f(args)` of a name
that is neither an intrinsic nor an export of `std.core.Object` calls the function in field `f` with
`o` as its first argument; its result type comes from the context. See [objects and
prototypes](../04-expressions/03-objects-and-prototypes.md).

```jello
// expect: 2 true false {a: 1} [:a, :b] 7
let o = { a: 1 };
o.set(:b, 2);
let two = o.get<I32>(:b);
let had = o.exists("b");
o.remove(:b);
let gone = o.exists(:b);
let shown = o.toString();
o.b = 0;
let counter = { n: 5, add: fn(self: Object, by: I32): I32 { (self.n as I32) + by } };
let seven: I32 = counter.add(2);
Std.string(two) + " " + Std.string(had) + " " + Std.string(gone) + " " + shown + " "
  + Std.string(o.keys()) + " " + Std.string(seven)
```

## Tuples, atoms and enums

| Function | Description |
| --- | --- |
| `Tuple.len(t): I32`, `t.len()` | the number of fields |
| `Tuple.get(t, i: I32): Dynamic` | field `i`, as a `Dynamic`; an index out of range traps |
| `Tuple.toString(t): Bytes`, `t.toString()` | `t` as `Std.string` prints it |
| `Atom.intern(name): Atom` | the atom named by a Bytes literal, the same as `:name` |
| `Atom.toString(a): Bytes`, `a.toString()` | the atom's name, without its colon |
| `Enum.tag(e): I32`, `e.tag()` | an enum value's variant, by its index in the declaration |
| `Enum.toString(e): Bytes`, `e.toString()` | the enum value as `Std.string` prints it |

`Atom.intern` runs at compile time, so its argument must be a Bytes literal; anything else is an
error (E0808). A program that declares a type named `Enum` hides the `Enum` namespace.

```jello
// expect: 2 two (1, "two") hello 1 Shape.Square
enum Shape { Circle(r: I32); Square; }
let t = (1, "two");
let parts = [Std.string(Tuple.len(t)), Std.string(Tuple.get(t, 1)), t.toString(),
  Atom.intern("hello").toString(), Std.string(Shape.Square.tag()), Shape.Square.toString()];
parts.join(" ")
```

## Function

| Function | Description |
| --- | --- |
| `Function.callWith(f, thisArg, args...)` | calls `f` with `thisArg` as its first argument, then `args` |
| `Function.bind(f, thisArg, args...)` | the function of `f`'s remaining parameters that calls `f` with `thisArg` and `args` first |

Both are also methods of a function value, `f.callWith(...)` and `f.bind(...)`. A function's `this`
is its first parameter. `bind` evaluates its arguments once, when `bind` runs, not at each call of
the function it returns. Neither takes a spread argument (E0604). See
[functions](../02-types/04-functions.md).

```jello
// expect: 15 15 13
let scale = fn(o: Object, by: I32): I32 { (o.n as I32) * by };
let box = { n: 5 };
let triple = scale.bind(box);
let add = fn(a: I32, b: I32, c: I32): I32 { a + b + c };
let addTen = Function.bind(add, 4, 6);
Std.string(scale.callWith(box, 3)) + " " + Std.string(triple(3)) + " " + Std.string(addTen(3))
```
