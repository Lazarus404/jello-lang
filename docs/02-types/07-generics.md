# Generics

A declaration with type parameters works at many types. Jello makes a separate copy of a generic
function, enum, record or prototype for each list of type arguments the program uses it at, so
each copy is checked and compiled at concrete types.

```jello
// expect: 7hi
let id<T> = fn(x: T): T { x };
Std.string(id(7)) + id("hi")
```

## Type parameters

Type parameters are written in angle brackets after the declared name:

- a function: `let name<T, U> = fn(...) { ... };`
- an enum: `enum Name<T> { ... }` (see [Enums](05-enums.md#generic-enums))
- an alias or record: `typedef Name<T> = ...;` (see
  [Typedefs and records](06-typedefs-and-records.md))
- a prototype: `prototype Name<T> { ... };` (see
  [Objects and prototypes](../04-expressions/03-objects-and-prototypes.md))

Within the declaration, a type parameter is used like any other type name:

```jello
// expect: 6|abc
enum Tree<T> { Leaf; Node(left: Tree<T>, value: T, right: Tree<T>); }

let fold<T, A> = fn(t: Tree<T>, start: A, f: (A, T) -> A): A {
  match (t) { Leaf => start, Node(l, v, r) => fold(r, f(fold(l, start, f), v), f) }
};
let leaf<T> = fn(value: T): Tree<T> { Tree.Node(Tree.Leaf, value, Tree.Leaf) };
let numbers = Tree.Node(leaf(1), 2, leaf(3));
let words = Tree.Node(leaf("a"), "b", leaf("c"));
Std.string(fold(numbers, 0, fn(acc, x) { acc + x })) + "|"
  + fold(words, "", fn(acc, x) { acc + x })
```

A type parameter may not have the name of an existing type:

```jello
// error: 2:8: type parameter I32 hides the type I32 [E0305]
let id<I32> = fn(x: I32): I32 { x };
id(1)
```

### Bounds

An unbounded type parameter's values can only be moved: stored, passed, returned, and put into
and taken out of containers. A bound after `:` allows more, and several bounds are joined with
`+`:

| Bound | Allows on `T` | Satisfied by |
|-------|---------------|--------------|
| `Eq` | `==` and `!=`, also on types built from `T` such as `Array<T>` | types with `==` |
| `Show` | `Std.string` | types `Std.string` can show |
| `Integer` | arithmetic, bitwise operators and comparisons | the integer types |

```jello
// expect: [1, 2] found|missing|9
let describe<T: Eq + Show> = fn(x: T, target: T): Bytes {
  x == target ? Std.string(x) + " found" : "missing"
};
let max<T: Integer> = fn(a: T, b: T): T { a > b ? a : b };
describe([1, 2], [1, 2]) + "|" + describe("a", "b") + "|" + Std.string(max(4, 9))
```

```jello
// error: 2:38: == on T needs the bound Eq: write <T: Eq> [E0501]
let same<T> = fn(a: T, b: T): Bool { a == b };
same(1, 2)
```

```jello
// error: 3:1: Bytes does not satisfy the bound Integer of T in max [E0501]
let max<T: Integer> = fn(a: T, b: T): T { a > b ? a : b };
max("a", "b")
```

Only a function's type parameters take bounds; those of enums, typedefs and prototypes do not.

Inside a generic body, `value as T` converts a `Dynamic` to the type argument, checked as that
type, and `value is T` tests for it (see [Nullability and Dynamic](02-nullability-and-dynamic.md)).
Only an instance whose type argument the VM can check (`Bool`, `I32`, `I64`, the float types,
`Atom`, `Bytes`, an enum or `Object`) may use `as T`; at any other type, such as `Array<I32>`, it
is an error.

## Type arguments

A call usually infers the type arguments from its arguments, from the type the context expects,
and from later uses of the result. A lambda argument takes its parameter types from the generic
function's signature:

```jello
// expect: [2, 3, 4]
let map<T, U> = fn(xs: Array<T>, f: (T) -> U): Array<U> {
  let out = Array.new<U>(0);
  let i = 0;
  while (i < xs.len()) { out.push(f(xs[i])); i++; }
  out
};
Std.string(map([1, 2, 3], fn(x) { x + 1 }))
```

Type arguments can also be written in angle brackets, which overrides inference. Here `1`
becomes an `I64`, so the shift does not overflow:

```jello
// expect: 4294967296
let id<T> = fn(x: T): T { x };
let wide = id<I64>(1);
wide << 32
```

The same syntax gives the element type of built-in constructors and the type arguments of a
generic enum or prototype:

```jello
// expect: [] [] Option.None 7
enum Option<T> { Some(value: T); None; }
typedef Box<T> = { let value: T; };
prototype Box<T> { init(value: T) { this.value = value; }; };

let xs = Array.new<Bytes>(0);
let ys = List.nil<I32>();
let b = new Box<I32>(7);
Std.string(xs) + " " + Std.string(ys) + " " + Std.string(Option<I32>.None) + " "
  + Std.string(b.value)
```

The number of type arguments must match the number of type parameters:

```jello
// error: 3:1: id takes 1 type argument, not 2 [E0402]
let id<T> = fn(x: T): T { x };
id<I32, I64>(1)
```

## Instances

Each list of type arguments a generic is used at makes an instance, which is checked and
compiled at those types. So:

- A generic function body is checked once on its own, with its bounds, even if nothing calls
  it, and again in each instance.
- Each instance of a generic enum or record has its own `==` and `toString`, and each instance of
  a generic prototype has its own prototype object, which `is` tells apart.
- Integer literals in a generic body take the width of each instance's type argument, and are
  range-checked at it.
- A generic function from another module is instantiated in the calling program, but resolves the
  names in its body in its own module, private names included.

```jello file=util/Pairs.jello
package util;

let SEP = "/";
export let joined<T: Show> = fn(a: T, b: T): Bytes { Std.string(a) + SEP + Std.string(b) };
```

```jello
// expect: 1/2|x/y
// flags: --path $DIR
import { joined } from util.Pairs;

joined(1, 2) + "|" + joined("x", "y")
```

### Local generic functions

A generic function may be declared inside another function. Its instances can call themselves
and the local generic functions declared before it, but a local generic function cannot capture
the enclosing function's variables; pass them as arguments instead.

```jello
// error: 3:43: a generic local function cannot capture base; pass it as an argument [E0308]
let run = fn(base: I32): I32 {
  let add<T: Integer> = fn(x: T): T { x + base };
  add(1)
};
run(2)
```

### Limits

Instances are made from the types the program uses, so they must come to an end. A generic
whose instance at `T` needs an instance at a larger type, such as `Array<T>`, without end, is an
error once instances nest more than 64 deep:

```jello
// error: 2:52: instantiating nest nests more than 64 deep [E0901]
let nest<T> = fn(x: T, n: I32): I32 { n == 0 ? 0 : nest([x], n - 1) };
nest(1, 3)
```

```jello
// error: 3:8: instantiating W nests more than 64 deep [E0901]
enum W<T> { Stop; More(next: W<Array<T>>); }
let w: W<I32> = W.Stop;
0
```

A list cannot be null, so a generic that uses `Null<T>` or `?T` cannot be instantiated at a
`List` type.
