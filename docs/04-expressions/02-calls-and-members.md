# Calls and members

This page covers the postfix forms of expressions: calls, `.member` access, methods on built-in
types, `[index]`, and the forms built on them: spread arguments, pipelines and casts. Their
precedence is in the [operators table](01-operators.md#precedence-and-associativity). Function
types, parameters and closures are described in [Functions](../02-types/04-functions.md).

## Calls

`f(a, b)` calls the function `f` with the arguments `a` and `b`. The callee is evaluated first, and
then the arguments, left to right. The call's value is the function's result.

```jello
// expect: 25
let square = fn(x: I32): I32 { x * x };
let add = fn(a: I32, b: I32): I32 { a + b };
Std.string(add(square(3), square(4)))
```

Each argument is checked against its parameter's type, and the number of arguments must match the
parameters, apart from those with defaults, optional ones and a variadic one (see
[Functions](../02-types/04-functions.md)).

```jello
// error: 3:7: wrong argument type for twice: the parameter is I32, but this argument is Bytes [E0400]
let twice = fn(x: I32): I32 { x * 2 };
twice("two")
```

```jello
// error: 3:1: wrong argument count for twice [E0600]
let twice = fn(x: I32): I32 { x * 2 };
twice(1, 2)
```

A list of arguments may end with a comma, and so may a list of parameters, so a long call or
signature can put one item per line without its last line being the odd one out:

```jello
// expect: 3 cm by 4 cm
let describe = fn(
  width: I32,
  height: I32,
  unit: Bytes,
): Bytes {
  `${width} ${unit} by ${height} ${unit}`
};
describe(
  3,
  4,
  "cm",
)
```

Any expression whose value is a function can be called: a variable, an element, the result of
another call, or a parenthesised `fn` expression. Calling a value that is not a function is an
error.

```jello
// expect: 2 8 6
let adder = fn(n: I32): (I32) -> I32 { fn(x: I32): I32 { x + n } };
let table = [fn(x: I32): I32 { x * 2 }];
Bytes.join([Std.string(adder(1)(1)), Std.string(table[0](4)),
  Std.string((fn(x: I32): I32 { x * 3 })(2))], " ")
```

```jello
// error: 3:1: call target is not a function [E0601]
let x = 1;
x(2)
```

### Type arguments

A generic function infers its type arguments from its arguments and from the type its result is
expected to have. Type arguments can also be written after the callee, as `f<T>(args)`; see
[Generics](../02-types/07-generics.md).

```jello
// expect: 5 x
let id<T> = fn(value: T): T { value };
let wide = id<I64>(5);
Std.string(wide) + " " + id("x")
```

### Functions as values

A function bound with `let` is a value like any other. It can be stored, passed to another function
and returned, with a [function type](../02-types/04-functions.md) such as `(I32) -> I32`.

```jello
// expect: 8
let double = fn(x: I32): I32 { x * 2 };
let apply = fn(f: (I32) -> I32, value: I32): I32 { f(value) };
let chosen = double;
Std.string(apply(chosen, 4))
```

A generic function is not a value, because it has no single type; it can only be called.

```jello
// error: 3:9: id is generic, so it can only be called [E0602]
let id<T> = fn(value: T): T { value };
let f = id;
1
```

To pass one instance of it, wrap the call in a function of the type you need, such as
`fn(x: I32): I32 { id(x) }`.

A prototype's method read without a call, as `o.m`, is a function that takes the receiver as its
first parameter; see [Objects and prototypes](03-objects-and-prototypes.md#methods-as-values).

## Member access

`value.name` reads a member of a value. What members a value has depends on its type:

| Value | `.name` reads |
| --- | --- |
| Tuple | `t.0`, `t.1`, ...: the item at that position |
| Record | the field of that name, as its declared type |
| Object | the field of that name, as a `Dynamic`; null when there is none |
| Prototype value | a field, as a `Dynamic` or, for a [linked typedef](03-objects-and-prototypes.md#linked-typedefs), its declared type; or a method |
| Module imported as a namespace | an export of the module (see [Modules](../05-modules.md)) |
| Enum | `E.Variant`: a variant of the enum named `E` (see [Enums](../02-types/05-enums.md)) |

```jello
// expect: a 2 1
typedef Point = { let x: I32; let y: I32; };
let pair = (1, "a");
let p: Point = { x: 2, y: 3 };
let o = { n: 1 };
pair.1 + " " + Std.string(p.x) + " " + Std.string(o.n as I32)
```

An enum value's fields are read by matching it against a pattern, not by `.name`; see
[Patterns](../03-statements/03-patterns.md).

A member name may be a keyword, so `o.match` and `o.type` are field reads.

## Methods on built-in types

A call `value.name(args)` on a value of a built-in type is a method call. Every method of a built-in
type is also a function of the type's namespace that takes the value first, so `xs.push(4)` and
`Array.push(xs, 4)` are the same call, and so are `text.slice(0, 2)` and `Bytes.slice(text, 0, 2)`.

```jello
// expect: 4 4 he he
let xs = [1, 2];
xs.push(3);
Array.push(xs, 4);
let text = "hello";
Std.string(xs.len()) + " " + Std.string(Array.len(xs)) + " " + text.slice(0, 2) + " " + Bytes.slice(text, 0, 2)
```

### Intrinsics

Some functions are built into the compiler. They are:

| Type | Functions |
| --- | --- |
| `Array` | `Array.new<T>(length)`; `len()`, `push(value)` (gives the new length), `pop()`, `resize(length)` |
| `List` | `List.nil<T>()`, `List.cons(head, tail)`; `head()`, `tail()`, `is_nil()` |
| `Bytes` | `Bytes.new(length)`; `len()`, `get(i)` and `get_u8(i)`, `slice(start, length)`, `eq(other)`; and, of a buffer from `Bytes.new`, `set(i, byte)`, `set_u8(i, byte)` and `toString()` |
| Tuple | `len()`, `toString()`; `Tuple.get(t, i)` |
| Enum | `tag()` (the variant's position in its declaration), `toString()` |
| `Atom` | `toString()`; `Atom.intern("literal")` |
| `Object` | `get<T>(key)`, `set(key, value)`, `exists(key)`, `remove(key)`, `clear()`, `copy()`, `keys()`, `toString()` |
| Function | `bind(thisArg, args...)`, `callWith(thisArg, args...)`; see [Objects and prototypes](03-objects-and-prototypes.md#bind-and-callwith) |

`Array.new`, `List.nil`, `List.cons`, `Bytes.new` and `Atom.intern` make a value rather than take
one, so they have only the namespace form.

```jello
// expect: 3 [1, 2] 1 Colour.Blue hi
enum Colour { Red; Green; Blue; }
let xs = [1, 2, 3];
let last = xs.pop();
let tag = Colour.Green.tag();
Bytes.join([Std.string(last), Std.string(xs), Std.string(tag), Colour.Blue.toString(), :hi.toString()], " ")
```

### Library methods

Beyond the intrinsics, `Array`, `Bytes`, `List` and `Object` values have the functions that the
standard library's module of that name exports: `std.core.Array`, `std.core.Bytes`, `std.core.List`
and `std.core.Object`. Such a function takes the value as its first parameter, and
the compiler links both call forms to it, loading the module when a program uses one of its
functions; no import is needed. Where an intrinsic and a library function share a name, the
intrinsic wins.

```jello
// expect: true [10, 20, 30] a|b|c 3
let xs = [1, 2, 3];
let found = xs.contains(2);
let scaled = xs.map(fn(x: I32): I32 { x * 10 });
let parts = "a,b,c".split(",");
let joined = Bytes.join(parts, "|");
let words = list["x", "y", "z"];
Bytes.join([Std.string(found), Std.string(scaled), joined, Std.string(words.len())], " ")
```

The functions are also ordinary exports, which can be imported by name, as
`import { map } from std.core.Array;`. The standard library pages list them; see
[the standard library](../06-stdlib/README.md).

A name that is neither an intrinsic nor an export of the type's module is an error, which lists
the closest names the module does export:

```jello
// error: 3:1: unknown method nope of Array<I32>; closest: copy [E0800]
let xs = [1];
xs.nope()
```

On an Object, a method name that is neither an intrinsic nor a `std.core.Object` function calls the
function held in the Object's field of that name, with the Object as its first argument. See
[Objects and prototypes](03-objects-and-prototypes.md#objects).

Functions of other namespaces, such as `Math.sqrt(x)` and `Std.string(x)`, are called only in the
namespace form; see [the standard library](../06-stdlib/README.md).

## Spread arguments

`...value` in a call's arguments spreads a tuple or an array.

- A tuple's items become arguments in turn, each checked against its parameter.
- An array's elements go, with any arguments after it, to the function's variadic parameter, so an
  array cannot fill a required parameter.

Arguments, spread or not, are evaluated in order.

```jello
// expect: 123 10 21
let add3 = fn(a: I32, b: I32, c: I32): I32 { a * 100 + b * 10 + c };
let sum = fn(first: I32, ...rest: I32): I32 {
  let total = first; let i = 0;
  while (i < rest.len()) { total += rest[i]; i++; }
  total
};
let triple = (1, 2, 3);
let more = [4, 5];
Bytes.join([Std.string(add3(...triple)), Std.string(sum(1, ...more)), Std.string(sum(...triple, ...more, 6))], " ")
```

```jello
// error: 4:1: wrong argument count for f [E0600]
// An array goes to the variadic parameter, so it cannot pass a required one.
let f = fn(a: I32, ...b: I32): I32 { a };
f(...[1])
```

```jello
// error: 3:3: spread operand must be a tuple or an array, not I32 [E0604]
let f = fn(a: I32, ...b: I32): I32 { a };
f(...1)
```

Spreads in array and tuple literals are described in
[Collections](../02-types/03-collections.md), and in object literals with
[Objects](03-objects-and-prototypes.md#objects).

## Index expressions

`value[index]` reads an element. The index of an array, `Bytes` or a buffer is an `I32`, counted
from 0; an index outside the value traps with `BOUNDS`.

| Value | `v[i]` | `v[i] = x` |
| --- | --- | --- |
| `Array<T>` | the element, a `T` | stores `x` |
| `Bytes` | the byte, an `I32` from 0 to 255 | an error: `Bytes` is immutable |
| Buffer from `Bytes.new` | the byte, an `I32` | stores the byte; a value outside 0 to 255 traps |
| `Object` | the field whose key is `i`, an atom or a `Bytes` literal, as a `Dynamic` | stores `x` |

```jello
// expect: 30 104 2
let xs = [10, 20, 30];
let text = "hi";
let o = { a: 1 };
let key = :a;
xs[0] = xs[2];
o[key] = 2;
Std.string(xs[0]) + " " + Std.string(text[0]) + " " + Std.string(o["a"])
```

```jello
// trap: BOUNDS 3:1
let xs = [1, 2, 3];
xs[3]
```

Tuples are indexed with `.0`, `.1` and so on; `Tuple.get(t, i)` reads an item at an index computed
at run time. Lists are not indexed; take them apart with patterns or `head()` and `tail()`.

## Pipelines

`value |> body` evaluates `value` once and then `body`, in which `%`, the topic, stands for the
value.

```jello
// expect: 3 1 8
let xs = [1, 2, 3];
let add = fn(a: I32, b: I32): I32 { a + b };
Bytes.join([Std.string(xs |> %.len()), Std.string(10 |> % % 3), Std.string(4 |> add(%, %))], " ")
```

When the body does not use `%`, it must be a call, and the value becomes the call's first argument.
`x |> f(a)` is `f(x, a)`, and `x |> f()` is `f(x)`.

```jello
// expect: 11 5
let double = fn(x: I32): I32 { x * 2 };
let inc = fn(x: I32): I32 { x + 1 };
let add = fn(a: I32, b: I32): I32 { a + b };
Std.string(5 |> double() |> inc()) + " " + Std.string(2 |> add(3))
```

Pipelines are left-associative, so each body takes the value the one before gives. Inside a nested
pipeline, `%` is the innermost pipeline's value.

```jello
// error: 3:14: a pipeline body that does not use % must be a call [E0413]
let double = fn(x: I32): I32 { x * 2 };
let r = 5 |> double;
r
```

```jello
// error: 2:9: ParseError: pipeline topic used outside pipeline body [E0106]
let r = %;
r
```

A pipeline binds more loosely than every operator except assignment, so the body extends as far as
it can: `x |> % + 1 |> % * 2` is `(x |> % + 1) |> % * 2`.

## Casts

`value as T` converts a value to the type `T`. It binds more tightly than any binary operator, and
more loosely than a prefix operator: `-x as I64` is `(-x) as I64`, and `a + b as I64` converts only
`b`.

| From | To | Result |
| --- | --- | --- |
| an integer type | another integer type | the value wrapped to the new width, keeping the low bits |
| an integer or float type | a float type | the nearest representable value |
| a float type | an integer type | the value truncated toward zero; NaN, or a value outside the type, traps with `ARITHMETIC` |
| `?T` | `T` | the value; null traps with `NULL_DEREF` |
| any value but a buffer | `Dynamic` | the value boxed |
| `Dynamic` | `T` | the value it holds, when it holds a `T`; otherwise a `TYPE_MISMATCH` trap |
| `Object` or `Dynamic` | a prototype's type | the Object, when it delegates to that prototype |

A cast to the value's own type does nothing. Any other cast, such as `Bool` to `I32`, is an error.

```jello
// expect: 3 -3 44 255 2.5
let small = 300 as U8;
let negative = -1;
let ratio = 5 as F64;
Bytes.join([Std.string(3.9 as I32), Std.string(-3.9 as I32), Std.string(small),
  Std.string(negative as U8), Std.string(ratio / 2.0)], " ")
```

```jello
// error: 2:1: cannot convert Bool to I32 [E0406]
true as I32
```

```jello
// trap: NULL_DEREF 3:1
let missing: ?I32 = null;
missing as I32
```

Casts to and from `Dynamic`, and the `is` test that goes with them, are described in
[Nullability and Dynamic](../02-types/02-nullability-and-dynamic.md).
