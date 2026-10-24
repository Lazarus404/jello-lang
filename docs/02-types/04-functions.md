# Functions

Functions are values. A `fn` expression makes one, a `let` names it, and a call runs it.

```jello
// expect: 7
let add = fn(a: I32, b: I32): I32 { a + b };
add(3, 4)
```

## `fn` expressions

```text
fn(parameters) { body }
fn(parameters): ResultType { body }
```

The body is a block. Its value is its last expression when that has no `;` after it, or the value
of a `return` statement:

```jello
// expect: big small
let size = fn(n: I32): Bytes {
  if (n > 2) { return "big"; }
  "small"
};
size(3) + " " + size(1)
```

Parameters are immutable inside the body; assign to a local copy instead.

```jello
// error: 3:3: cannot assign to immutable local: n [E0303]
let next = fn(n: I32): I32 {
  n = n + 1;
  n
};
next(1)
```

### Result types

The result type after `)` may be left out, and the compiler infers it from the `return`
statements and the final expression together. When some return a value and others return
`null`, the result is nullable:

```jello
// expect: 7 null
let pick = fn(c: Bool) { if (c) { return 7; } null };
Std.string(pick(true)) + " " + Std.string(pick(false))
```

A function that ends without a value returns `Null`, and `return;` returns `null` from it. Write
the result type `Void` instead for a function that never produces a value at all; its calls are
used as statements, and it cannot return a value.

```jello
// expect: [10, 20]
let log = Array.new<I32>(0);
let record = fn(n: I32): Void { log.push(n * 10); };
record(1);
record(2);
Std.string(log)
```

When a result type is written, every path must produce a value of that type:

```jello
// error: 2:13: function can fall through without a value of type I32 [E0400]
let count = fn(n: I32): I32 {
  if (n > 0) { return n; }
  let doubled = n * 2;
};
count(1)
```

### Parameter types

A top-level or local function's parameters need types, unless the function is written where a
function type is expected, such as an argument or an annotated `let`. Then the parameters take
their types from that context:

```jello
// expect: 12
let apply = fn(f: (I32) -> I32, x: I32): I32 { f(x) };
apply(fn(n) { n * 3 }, 4)
```

```jello
// error: 2:15: parameter x of same needs a type [E0403]
let same = fn(x) { x };
same(1)
```

## Function types

The type of a function is written with its parameter types in parentheses and its result type
after `->`:

| Type | Meaning |
|------|---------|
| `() -> I32` | no parameters, returns `I32` |
| `(I32) -> Bool` or `I32 -> Bool` | one parameter; the parentheses are optional |
| `(I32, Bytes) -> Null` | two parameters |
| `(I32) -> (I32) -> I32` | returns a function; `->` groups to the right |
| `(I32, ?I32) -> I32` | the second parameter may be omitted |
| `(I32, I32...) -> I32` | any number of further `I32` arguments; `...I32` also works |

The list of parameter types may end with a comma, as a value's parameters may.

```jello
// expect: 3
let add: (I32) -> (I32) -> I32 = fn(a) { fn(b) { a + b } };
add(1)(2)
```

Function types must match exactly. There is no conversion between, say, `(I32) -> I32` and
`(I64) -> I32`, or to `(Dynamic) -> Dynamic`:

```jello
// error: 3:26: slot is annotated (I64) -> I32, but its value is (I32) -> I32 [E0400]
let inc: (I32) -> I32 = fn(n) { n + 1 };
let slot: (I64) -> I32 = inc;
0
```

## Optional, default and variadic parameters

A parameter may be:

| Form | Meaning |
|------|---------|
| `x: T` | required |
| `x: T = value` | may be omitted; the call then passes `value` |
| `?x: T` | may be omitted; it holds a `Null<T>`, which is `null` when omitted |
| `?x: T = value` | may be omitted; holds a `Null<T>`, which is `value` when omitted |
| `...xs: T` | takes the remaining arguments as an `Array<T>` |

```jello
// expect: 12 13 6
let add = fn(x: I32, y: I32 = 1, z: I32 = 10): I32 { x + y + z };
Std.string(add(1)) + " " + Std.string(add(1, 2)) + " " + Std.string(add(1, 2, 3))
```

```jello
// expect: 1 3
let pick = fn(a: I32, ?b: I32): I32 { b == null ? a : a + b };
Std.string(pick(1)) + " " + Std.string(pick(1, 2))
```

```jello
// expect: 0 6
let sum = fn(...xs: I32): I32 {
  let total = 0;
  let i = 0;
  while (i < xs.len()) { total += xs[i]; i++; }
  total
};
Std.string(sum()) + " " + Std.string(sum(1, 2, 3))
```

Each call of a variadic function gets an array of its own. A call may also spread an array
into the variadic parameter, or a tuple into several parameters; see
[Calls and members](../04-expressions/02-calls-and-members.md).

The rules:

- A default is a constant: a literal (a negative number included), an enum's variant without
  fields, such as `Color.Red`, or the name of a `const`, which may be declared after the function.
  Its type must be the parameter's.
- A parameter with a default cannot have a nullable type; use `?x: T` for a parameter that may be
  null. Likewise the default cannot be `null`.
- The order is: required parameters, then those that may be omitted, then at most one variadic
  parameter, which is last. A required parameter cannot follow one that may be omitted, and a
  parameter with a default cannot follow a `?` parameter that has none.
- A call fills the parameters from left to right. With `fn(a: I32, ?b: I32, ...rest: I32)`,
  `f(1)` gives `b` null and `rest` empty, and `f(1, 2, 3)` gives `b` 2 and `rest` `[3]`.
- Too many arguments, or too few for the required parameters, is an error.

```jello
// error: 2:24: required parameter b cannot follow an optional one [E0102]
let f = fn(a: I32 = 1, b: I32) { a + b };
f(2)
```

```jello
// error: 3:21: the default of x must be a constant [E0603]
let n = 3;
let f = fn(x: I32 = n) { x };
f()
```

### Through a function value

A `?` parameter stays optional through a function value, whose type writes it `?T`. A default
is filled in only by a direct call of the function by name: through a value, a parameter with a
default is required.

```jello
// expect: 0 3
let fallback: (?I32) -> I32 = fn(?x) { x == null ? 0 : x };
Std.string(fallback()) + " " + Std.string(fallback(3))
```

```jello
// error: 4:1: wrong indirect call argument count [E0600]
let f = fn(x: I32 = 1) { x };
let g = f;
g()
```

## Closures

A function can use the variables of the scopes around it. It captures the variables
themselves, not copies of their values: an assignment on either side is seen by the other, and
several closures can share one variable.

```jello
// expect: 31
let counter = fn(): Tuple<() -> Null, () -> I32> {
  let n = 0;
  let add = fn() { n += 10; null };
  let get = fn() { n++; n };
  (add, get)
};
let (add, get) = counter();
add();
add();
add();
get()
```

A variable declared inside a loop's body is a new variable on each pass, so each closure made in
the loop keeps its own. A variable declared before the loop is one variable, which every closure
shares.

```jello
// expect: 0 1 4 | 3 3 3
let squares = Array.new<() -> I32>(0);
let counters = Array.new<() -> I32>(0);
let i = 0;
while (i < 3) {
  let square = i * i;
  squares.push(fn() { square });
  counters.push(fn() { i });
  i++;
}
Std.string(squares[0]()) + " " + Std.string(squares[1]()) + " " + Std.string(squares[2]())
  + " | " + Std.string(counters[0]()) + " " + Std.string(counters[1]()) + " "
  + Std.string(counters[2]())
```

Arrays and objects are references, so a closure that captures one sees later changes to its
contents.

When a closure may assign `null` to a nullable variable, a check made outside the closure does
not protect a later read; if the variable is null by then, the read traps with `NULL_DEREF`.

## Recursion

A top-level function can call itself and the functions declared after it, without result type
annotations. A local function can call itself when its type is known before its body; see
[Bindings](../03-statements/01-bindings.md#initialisation-order).

```jello
// expect: 6765 true
let fib = fn(n: I32) { n < 2 ? n : fib(n - 1) + fib(n - 2) };
let isEven = fn(n: I32) { n == 0 ? true : isOdd(n - 1) };
let isOdd = fn(n: I32) { n == 0 ? false : isEven(n - 1) };
Std.string(fib(20)) + " " + Std.string(isEven(10))
```

### Self tail calls

A call of a function by itself in return position does not grow the stack: it reuses the
function's frame. Return position is a `return` statement's value, the branches of an `if`,
`?:` or `match` in return position, and the final expression of the body or of a block in
return position. So this loop of a million calls runs in constant stack space:

```jello
// expect: 1000000
let count = fn(n: I32, acc: I32): I32 {
  if (n == 0) { return acc; }
  count(n - 1, acc + 1)
};
count(1000000, 0)
```

A call inside a `try` body is not in return position, because the `catch` must still be able to
run after it.

## Function equality

`==` on functions compares identity. A function value is equal to itself and to copies of the
same value. A function without captures is one value however often it is read, while each
evaluation of a closure makes a new function, even with the same captured values.

```jello
// expect: true false false true
let f = fn() { 1 };
let g = fn() { 1 };
let h = f;
let make = fn(n: I32): () -> I32 { fn() { n } };
let c = make(1);
Std.string(h == f) + " " + Std.string(f == g) + " " + Std.string(make(1) == make(1)) + " "
  + Std.string(c == c)
```

Tuples, arrays, enums and records that hold functions compare those items the same way.
