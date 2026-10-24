# Bindings

A binding gives a name to a value. `let` declares one that may be assigned again; `const`
declares one that may not.

```jello
// expect: 3 10
let count = 1;
count = count + 2;
const limit = 10;
Std.string(count) + " " + Std.string(limit)
```

Every binding is given its value where it is declared: `let x;` without `=` is a syntax error.

## `let` and `const`

Both forms take the same patterns, annotations and values. The difference is only whether the
names they bind may be assigned afterwards. Assigning to a `const` is an error, whether it is a
local or a global:

```jello
// error: 4:3: cannot assign to immutable local: limit [E0303]
let f = fn(): I32 {
  const limit = 1;
  limit = 2;
  limit
};
f()
```

`const` fixes the binding, not the value: a `const` array can still have elements pushed onto it.
A `const` holds the value it was given even if the expression it came from is a variable that is
later assigned.

```jello
// expect: 1 3
let x = 1;
const snapshot = x;
x = 2;
const items = [1, 2];
items.push(3);
Std.string(snapshot) + " " + Std.string(items.len())
```

A top-level `const` whose value is a literal, or a negated, converted or nullable one, is read as
that value itself, so name constants freely: reading `const MAX_ARGS = 8;` costs what writing `8`
does. A read ahead of the `const`'s statement, from a function above it, goes through the
global, as does a read of a nullable or `Dynamic` I64 or float, which the global boxes once.

## Type annotations

A binding's type is usually inferred from its value. Write `: Type` after the name to state it:

```jello
// expect: 5 null 0
let big: I64 = 5;
let maybe: ?Bytes = null;
let empty: Array<I32> = [];
Std.string(big) + " " + Std.string(maybe) + " " + Std.string(empty.len())
```

An annotation is needed when the value alone does not settle the type, as for an empty array:

```jello
// error: 2:5: cannot infer the type of xs; annotate it [E0403]
let xs = [];
xs.len()
```

Inside an expression, `as` settles it the same way:

```jello
// expect: a-b
let words = [] as Array<Bytes>;
words.push("a");
words.push("b");
words.join("-")
```

Once a binding has a type, every value assigned to it must have that type:

```jello
// error: 3:5: x is I32, but the value assigned is Bytes [E0400]
let x = 1;
x = "one";
x
```

See [Nullability and Dynamic](../02-types/02-nullability-and-dynamic.md) for `?T`, and
[Generics](../02-types/07-generics.md) for the type parameters a top-level function binding may
take, as in `let id<T> = fn(x: T): T { x };`.

## Destructuring

The left side of a `let` or `const` is a [pattern](03-patterns.md). A pattern that cannot fail
takes a value apart and binds each name it contains:

```jello
// expect: 1 2 3 4 5
enum Shape { Point(x: I32, y: I32); }
let (a, (b, c)) = (1, (2, 3));
let Point(x, y) = Shape.Point(4, 5);
[a, b, c, x, y].map(fn(n: I32): Bytes { Std.string(n) }).join(" ")
```

An annotation after a pattern gives the type of the whole value:

```jello
// expect: 3
let (p, q): Tuple<I32, I32> = (1, 2);
p + q
```

A pattern that can fail, such as an array pattern of a fixed length, is an error in a plain `let`,
because there would be nothing to bind when it does not match:

```jello
// error: 3:7: this pattern can fail, as on []: use `if (let ...)` or `let ... else` [E0702]
let f = fn(xs: Array<I32>): I32 {
  let [a, b] = xs;
  a + b
};
f([1, 2])
```

The name `_` binds nothing. It may be "declared" any number of times, even in one scope, and it
can never be read.

## `let ... else`

`let pattern = value else { ... };` takes a pattern that can fail. When the value matches, the
names are bound as in any `let`. When it does not, the `else` block runs, and it must leave: by
`return`, `throw`, `break` or `continue`.

```jello
// expect: 7 0
let first = fn(xs: Array<I32>): I32 {
  let [head, ..._] = xs else { return 0; };
  head
};
Std.string(first([7, 8])) + " " + Std.string(first([]))
```

The names the pattern binds are not in scope inside the `else` block. An `else` that can finish
normally is an error:

```jello
// error: 3:3: the else of a let must leave, by return, throw, break or continue [E0105]
let f = fn(xs: Array<I32>): I32 {
  let [a] = xs else { 0 };
  a
};
f([1])
```

A `let` of a single name cannot fail, so giving it an `else` is a syntax error. `let ... else` is
described with the other pattern forms in [Patterns](03-patterns.md#let-and-let--else).

## Scope

A local binding is visible from its declaration to the end of the block that declares it. A
block, `{ ... }`, has its own scope, as do the bodies of functions, loops and the branches of `if`
and `match`.

```jello
// expect: 1 20
let total = 1;
let inner = 0;
{
  let doubled = 20;
  inner = doubled;
}
Std.string(total) + " " + Std.string(inner)
```

### Shadowing

A name may be declared again in a nested block or a nested function, which hides the outer
binding until the inner scope ends. It may not be declared twice in one scope, and a function's
body cannot redeclare one of its parameters.

```jello
// expect: 2 1 6
let x = 1;
let inner = fn(): I32 { let x = 2; x };
let shadowed = fn(): I32 {
  let a = 1;
  { let a = 2; }
  let g = fn(): I32 { let a = 5; a };
  g() + a
};
Std.string(inner()) + " " + Std.string(x) + " " + Std.string(shadowed())
```

```jello
// error: 2:27: duplicate local: x [E0302]
let f = fn(x: I32): I32 { let x = 2; x };
f(1)
```

Two top-level bindings of one name are an error too:

```jello
// error: 3:1: duplicate top-level name: a [E0302]
let a = 1;
let a = 2;
a
```

## Globals

A binding at the top level of a module is a global of that module. Functions read and assign
globals as they do locals:

```jello
// expect: 2
let count = 0;
let bump = fn() { count += 1; null };
bump();
bump();
count
```

`export let` and `export const` make a global visible to other modules; see
[Modules](../05-modules.md). Another module may read an imported binding but not assign it.

### Initialisation order

A module's top-level statements run in order, once, when the module is initialised. Top-level
code may only name globals declared above it:

```jello
// error: 2:9: unknown name: later [E0300]
let x = later + 1;
let later = 2;
x
```

A function body may name a global declared further down, because it does not run where it is
written. Its type must be known before its value is checked: the global must be a `fn` literal, or
be annotated:

```jello
// expect: limit is 10
let describe = fn(): Bytes { "limit is " + Std.string(limit) };
let limit: I32 = 10;
describe()
```

```jello
// error: 2:25: later is read by f before later's initialiser is checked; annotate later [E0304]
let f = fn(): I32 { 1 + later };
let later = 2 + 1;
f()
```

The compiler also checks that no global is read before its value is set. A call made by
top-level code may only reach, through any chain of calls, globals whose declarations have
already run; the error points at the statement that would read too early:

```jello
// error: 3:1: global limit is read before it is initialised [E0304]
let describe = fn(): Bytes { "limit is " + Std.string(limit) };
let early = describe();
let limit: I32 = 10;
early
```

A global whose value is a `fn` literal is ready before any top-level code runs, so such functions
may call each other in any order, which is how mutually recursive functions are written:

```jello
// expect: true
let isEven = fn(n: I32): Bool { n == 0 ? true : isOdd(n - 1) };
let isOdd = fn(n: I32): Bool { n == 0 ? false : isEven(n - 1) };
Std.string(isEven(10))
```

So is one whose type is annotated, `let step: (I32) -> I32 = fn(n) { n + 1 };`, unless the
program assigns to it. An assigned function is an ordinary global, holding whichever function was
last assigned:

```jello
// expect: 3 8
let next: (I32) -> I32 = fn(n) { n + 1 };
let scale: (I32) -> I32 = fn(n) { n };
scale = fn(n) { n * 2 };
`${next(2)} ${scale(4)}`
```

A local binding is in scope in its own value only when that is a function whose parameter types
are known before its body: from the binding's annotation, or from the parameters' own, with every
parameter required. Such a local function can call itself, and its result type may be inferred
from the rest of its body; where the body uses its own result before that, as in
`1 + count(n - 1)`, annotate the result. Inside it, the name means the function itself, even when
an outer binding has the same name:

```jello
// expect: 120 55 0
let f = fn(): Bytes {
  let fact = fn(n: I32): I32 { n <= 1 ? 1 : n * fact(n - 1) };
  const fib: (I32) -> I32 = fn(n) { n < 2 ? n : fib(n - 1) + fib(n - 2) };
  let down = fn(n: I32) { n > 0 ? down(n - 1) : n };
  Std.string(fact(5)) + " " + Std.string(fib(10)) + " " + Std.string(down(3))
};
f()
```

Any other value cannot name its own binding:

```jello
// error: 3:11: unknown name: n [E0300]
let f = fn(): I32 {
  let n = n + 1;
  n
};
f()
```

A top-level `let` that destructures declares a global for each name it binds. Functions that
read those globals must come after it.

## Assignment

`target = value` stores a value. The target is a `let` binding (local or global), an element,
`xs[i]`, or a field, `p.x`.

```jello
// expect: 9 [1, 5] 3
typedef Point = { let x: I32; let y: I32; };
let n = 1;
let xs = [1, 2];
let p: Point = { x: 1, y: 2 };
n = 9;
xs[1] = 5;
p.x = 3;
Std.string(n) + " " + Std.string(xs) + " " + Std.string(p.x)
```

An assignment is an expression whose value is the value assigned. Assignments do not chain:
`a = b = 3` is a syntax error, as the right side of an assignment cannot itself be one.

```jello
// expect: 6
let x = 1;
let y = (x = 3);
x + y
```

Operands are evaluated left to right, and a read of a local gives its value at that moment, even
if a later operand assigns to it.

### Compound assignment

`target op= value` applies a binary operator to the target and the value, and stores the result.
The operators are `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=` and `^=`. `+=` on Bytes
appends.

```jello
// expect: 7 abc
let x = 6;
x += 1; x -= 2; x *= 3; x /= 5; x %= 2;
x <<= 3; x >>= 1; x &= 6; x |= 1; x ^= 2;
let s = "ab";
s += "c";
Std.string(x) + " " + s
```

### `++` and `--`

`++` adds one to a numeric target and `--` subtracts one. As a prefix, `++x` gives the new
value; as a suffix, `x++` gives the value before the change. Like assignment, they apply to
locals, globals, elements and record fields.

```jello
// expect: 5 7 7 5 3 2
typedef Point = { let x: I32; let y: I32; };
let a = 5;
let before = a++;
let after = ++a;
let down = a--;
let xs = [1, 2];
xs[0]++;
let p: Point = { x: 1, y: 2 };
p.x++;
[before, after, down, --a, xs[0] + p.x - 1, p.y].map(fn(v: I32): Bytes { Std.string(v) }).join(" ")
```

An `Object`'s fields are `Dynamic`, so `++`, `--` and compound assignment cannot update them in
place; assign the field a new value instead.

```jello
// error: 3:1: an Object field is Dynamic, so it cannot be updated in place; assign it a value [E0806]
let o = { n: 1 };
o.n++;
o.n
```
