# Exceptions

`throw value` stops the current computation and hands `value` to the nearest enclosing `try`,
which may be in a calling function. `try { ... } catch (e) { ... }` runs its body, and runs its
handler if the body throws.

```jello
// expect: 4 caught: odd
let half = fn(n: I32): I32 {
  if (n % 2 != 0) { throw "odd"; }
  n / 2
};
let attempt = fn(n: I32): Bytes {
  try { Std.string(half(n)) } catch (e) { "caught: " + (e as Bytes) }
};
attempt(8) + " " + attempt(3)
```

## `throw`

`throw value;` is a statement. It never finishes, so a branch that throws gives no value, and the
rest of the block after it does not run.

Any value that can become `Dynamic` can be thrown: numbers, Bool, Bytes, atoms, enum values,
arrays, objects and `null`. A list cannot, since its empty list would read as `null`, nor a
buffer made by `Bytes.new`, nor a `U64`, `?U64` or `?U32`, whose largest values would read as
negative:

```jello
// error: 3:21: a list cannot be thrown: its nil would read as null [E0409]
let l = list[1, 2];
let r = try { throw l; 0 } catch (e) { 1 };
r
```

## `try` and `catch`

`try { body } catch (name) { handler }` runs the body. If the body, or anything it calls, throws,
the rest of the body is skipped and the handler runs with `name` bound to the thrown value. The
binding is `Dynamic`, and it cannot be assigned; read it with `as`, or test it with `is` or a
`match`. Its scope is the handler.

```jello
// expect: I32 42, Bytes text, Err, Object 7, other null
enum Err { NotFound(key: Bytes); }
let attempt = fn(k: I32): Bytes {
  try {
    if (k == 0) { throw 42; }
    if (k == 1) { throw "text"; }
    if (k == 2) { throw Err.NotFound("x"); }
    if (k == 3) { throw { code: 7 }; }
    throw null;
  } catch (e) {
    match (e) {
      is I32 as n => "I32 " + Std.string(n),
      is Bytes as s => "Bytes " + s,
      is Err => "Err",
      is Object as o => "Object " + Std.string(o.code),
      _ => "other " + Std.string(e),
    }
  }
};
[attempt(0), attempt(1), attempt(2), attempt(3), attempt(4)].join(", ")
```

When the handler does not need the value, the name and its parentheses may be left out:
`try { ... } catch { ... }`. There is no `finally`.

A body that cannot throw, because it makes no calls and has no `throw`, never runs its handler.

### `try` as an expression

A `try` is an expression. Used as a value, its body and handler must each end with an expression,
and the two must have the same type, unless one of them always leaves.

```jello
// expect: 6 -1
let parse = fn(text: Bytes): I32 {
  try {
    if (text == "") { throw "empty"; }
    text.len() * 2
  } catch {
    -1
  }
};
Std.string(parse("abc")) + " " + Std.string(parse(""))
```

```jello
// error: 2:27: try and catch values must have the same type, not I32 and Bytes [E0400]
let v = try { 1 } catch { "x" };
v
```

```jello
// error: 2:14: a try used as a value needs a value at the end of its body [E0410]
let v: I32 = try { 1; } catch { 2 };
v
```

As a statement, neither block needs a value. Leaving a `try` body early with `return`, `break` or
`continue` is allowed.

### Rethrowing

A `throw` in a handler goes to the next `try` out, so a handler can rethrow the value it caught,
or a new one:

```jello
// expect: 6 14
let fail = fn(code: I32): I32 { throw code; 0 };
let doubled = fn(): I32 {
  try {
    try { fail(3) } catch (e) { throw (e as I32) * 2; }
  } catch (e) { e as I32 }
};
let passed = fn(): I32 {
  try {
    try { fail(4) } catch (e) { throw e; }
  } catch (e) { (e as I32) + 10 }
};
Std.string(doubled()) + " " + Std.string(passed())
```

`try` blocks may nest at most 16 deep in one function; deeper nesting is an error (E0901). Split
the function instead.

## Uncaught throws

A value thrown with no `try` around it, in any calling function, ends the program. The VM reports
an `UNCAUGHT_THROW` trap with the place of the `throw` and the thrown value, and exits with status
1.

```jello
// trap: UNCAUGHT_THROW
let check = fn(n: I32): I32 {
  if (n < 0) { throw "negative"; }
  n
};
check(-2)
```

## Throws and traps

A trap, such as an index out of bounds or the unwrapping of a `null`, is not a throw. A `catch`
never sees it: it ends the program at once.

```jello
// trap: BOUNDS
let xs = [1, 2];
try { xs[5] } catch { 0 }
```

The traps, and what each means, are listed in [Runtime traps](../08-errors/02-runtime-traps.md).

Assertions throw rather than trap: a failing `System.assert` or `System.assertEq` throws Bytes
that start with the place of the call, so a test can catch it.
