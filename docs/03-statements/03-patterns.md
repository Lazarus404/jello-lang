# Patterns

A pattern describes the shape of a value. Matching a value against a pattern tests that shape and
binds names to the parts. Patterns appear in:

| Where | Form | If the pattern fails |
|-------|------|----------------------|
| `match` arms | `match (v) { p => ... }` | the next arm is tried |
| `let` and `const` | `let p = v;` | not allowed: the pattern must not be able to fail |
| `let ... else` | `let p = v else { ... };` | the `else` block runs and must leave |
| `if` and `while` conditions | `if (let p = v) { ... }` | the condition is `false` |
| `with` clauses | `with p <- v { ... }` | the `with` gives `v`, or its `else` arms match it |
| list comprehensions | `list[for p <- xs do: ...]` | the item is skipped |

## Pattern forms

| Pattern | Matches |
|---------|---------|
| `_` | anything, binding nothing |
| `name` | anything, binding it to `name` |
| `42`, `-1`, `2.5`, `true`, `"text"`, `"a".code` | a value equal to the literal |
| `:ok` | the atom `:ok` |
| `null` | an absent value of a nullable type |
| `NAME` | a value equal to the constant `NAME` |
| `^name` | a value equal to the current value of `name` |
| `is T`, `is T as name` | a Dynamic or nullable value that holds a `T` |
| `(p, q)`, `(p, ...rest)` | a tuple, item by item |
| `[p, q]`, `[p, ...rest, q]`, `[p \| rest]` | an array or list, element by element |
| `{ name: p, other, ...rest }` | an `Object` with those fields, or a record |
| `Ctor`, `Ctor(p, q)` | an enum value made by that constructor |
| `~r/re/flags`, `~r/re/ as m` | Bytes that start with a match of the regular expression |
| `bits[u8(p), u16.be(q), rest(r)]` | Bytes laid out as those segments |

Patterns nest: any place a pattern goes in a tuple, array, object, constructor or bits segment
can hold another pattern. A name is bound at most once in a pattern; binding it twice is an error.

### Wildcards and bindings

`_` matches anything and binds nothing. A name that starts with a lower-case letter matches
anything and binds the value to that name, which is in scope in the arm's guard and body. The
names a `match` arm, an `if` or `while` test or a `with` clause binds cannot be assigned; those of
a `let` can, and those of a `const` cannot. When the whole pattern of a `let`, or of a `let` test,
is a single name, it binds that name whatever its case.

```jello
// expect: zero, 7
let describe = fn(n: I32): Bytes {
  match (n) {
    0 => "zero",
    other => Std.string(other),
  }
};
describe(0) + ", " + describe(7)
```

### Literals

Integer, float, Bool, Bytes and atom literals match a value equal to them. A number may be
negative, and `"c".code` stands for the code point of a single character. A literal must suit the
type of the value it is matched against: a string pattern cannot match an `I32`.

```jello
// expect: keyword digit 2 ok half
let kind = fn(word: Bytes): Bytes { match (word) { "let", "fn" => "keyword", _ => "name" } };
let char = fn(c: I32): Bytes { match (c) { "0".code => "digit", -1 => "end", _ => "other" } };
let both = fn(p: Tuple<Bool, Bool>): I32 { match (p) { (true, _) => 1, (false, true) => 2, (false, false) => 3 } };
let status = fn(a: Atom): Bytes { match (a) { :ok => "ok", _ => "error" } };
let half = fn(x: F64): Bytes { match (x) { 0.5 => "half", _ => "other" } };
[kind("let"), char("0".code), Std.string(both((false, true))), status(:ok), half(0.5)].join(" ")
```

```jello
// error: 2:39: a string pattern cannot match I32 [E0703]
let f = fn(n: I32): I32 { match (n) { "a" => 1, _ => 0 } };
f(1)
```

### Constants and pins

A capitalised name that is not a constructor of the matched enum names a constant, a `const`
binding. The value matches when it is equal, with `==`, to the constant.

`^name` pins a name: the value matches when it is equal to the current value of the variable
`name`, which may be any binding in scope. A pin cannot name a binding made by the same pattern.

```jello
// expect: limit small base other
const LIMIT: I32 = 10;
let base = 4;
let classify = fn(n: I32): Bytes {
  const Small = 2;
  match (n) { LIMIT => "limit", Small => "small", ^base => "base", _ => "other" }
};
[classify(10), classify(2), classify(4), classify(5)].join(" ")
```

A `let` binding can be assigned, so it cannot be used as a constant; pin it instead:

```jello
// error: 3:39: Top can be assigned, so it is no constant; pin it as ^Top [E0705]
let Top = 3;
let f = fn(n: I32): I32 { match (n) { Top => 1, _ => 0 } };
f(1)
```

### `null` and `is`

`null` matches the absence of a value of a nullable type `?T`. `is T` matches a nullable value
that is present, or a `Dynamic` value that holds a `T`; `is T as name` also binds the value, with
type `T`. `is` can test for `Bool`, `I32`, `I64`, `F16`, `F32`, `F64`, `Atom`, `Bytes`, an enum
type and `Object`.

```jello
// expect: none 3 | int 1, text a, object, other
let show = fn(x: ?I32): Bytes { match (x) { null => "none", is I32 as n => Std.string(n) } };
let kind = fn(d: Dynamic): Bytes {
  match (d) {
    is I32 as n => "int " + Std.string(n),
    is Bytes as s => "text " + s,
    is Object => "object",
    _ => "other",
  }
};
show(null) + " " + show(3) + " | "
  + [kind(1 as Dynamic), kind("a" as Dynamic), kind({ a: 1 } as Dynamic), kind(2.5 as Dynamic)].join(", ")
```

`is` applies only to a `Dynamic` or nullable value, and `null` only to a nullable one:

```jello
// error: 2:39: is tests a Dynamic or nullable value, not I32 [E0406]
let f = fn(n: I32): I32 { match (n) { is I32 => 1, _ => 0 } };
f(1)
```

The other patterns match a nullable value too, and never match `null`: on a `?Color`, the arms
`null`, `Red` and `Green` together are exhaustive.

### Tuples

A tuple pattern lists a pattern for each item, and needs a comma: `(p,)` is a one-item tuple, and
`(p)` is just `p`. One item may be a spread, `...rest` or `..._`, which stands for the items
between those before and after it; `...rest` binds them as a tuple.

```jello
// expect: 3 9 6
let first = fn(p: Tuple<I32, Bytes, Bool>): I32 { match (p) { (a, ...rest) => a + rest.0.len() } };
let last = fn(p: Tuple<I32, I32, I32>): I32 { let (..._, c) = p; c };
let ends = fn(p: Tuple<I32, I32, I32>): I32 { let (a, ..._, c) = p; a + c };
[first((1, "ab", true)), last((1, 2, 9)), ends((1, 2, 5))].map(fn(n: I32): Bytes { Std.string(n) }).join(" ")
```

### Arrays and lists

An array pattern, `[p, q]`, matches an `Array` or a `List` of exactly that many elements. A spread,
`...rest` or `..._`, matches any number of elements between the others; `[p | rest]` is another
way to write `[p, ...rest]`. A bound spread of an array is a new array of those elements; of a
list, it is the rest of the list itself. In a list pattern the spread may only come last.

```jello
// expect: 0 4 12 104 | 2 3
let f = fn(xs: Array<I32>): I32 {
  match (xs) {
    [] => 0,
    [a] => a,
    [a, b] => a * 10 + b,
    [first, ..._, last] => first * 100 + last,
  }
};
let sum: (List<I32>) -> I32 = fn(xs) { 0 };
sum = fn(xs) { match (xs) { [] => 0, [h | t] => h + sum(t) } };
let middle = fn(xs: Array<I32>): I32 { let [_, ...mid, _] = xs else { return -1; }; mid.len() };
[f([]), f([4]), f([1, 2]), f([1, 2, 3, 4])].map(fn(n: I32): Bytes { Std.string(n) }).join(" ")
  + " | " + Std.string(sum(list[1, 1])) + " " + Std.string(middle([1, 2, 3, 4, 5]))
```

A pattern may spread only once:

```jello
// error: 2:58: a pattern may spread only once [E0704]
let f = fn(xs: Array<I32>): I32 { match (xs) { [..._, a, ...rest] => a, _ => 0 } };
f([1])
```

### Objects and records

An object pattern, `{ name: p }`, matches an `Object` that has each named field, its own or one it
delegates to, and matches each field's value against its pattern. `{ name }` is short for
`{ name: name }`. A last `...rest` binds an object of the remaining fields. An `Object`'s fields
are `Dynamic`, so a field's pattern is usually a name or an `is` test.

```jello
// expect: circle 2, x 1 and 2 more, named ada, other
let describe = fn(v: Object): Bytes {
  match (v) {
    { r: is I32 as r } => "circle " + Std.string(r),
    { x: is I32 as x, ...others } => "x " + Std.string(x) + " and " + Std.string(Object.keys(others).len()) + " more",
    { name } => "named " + (name as Bytes),
    _ => "other",
  }
};
[describe({ r: 2 }), describe({ x: 1, y: 2, z: 3 }), describe({ name: "ada" }), describe({ r: "big" })].join(", ")
```

Matched against a record (see [Typedefs and records](../02-types/06-typedefs-and-records.md)), an
object pattern names fields the record declares, each with its declared type, and cannot fail on
the fields' presence, so it can destructure in a plain `let`. A record pattern takes no `...rest`.

```jello
// expect: 3 20
typedef Point = { let x: I32; let y: I32; };
let p: Point = { x: 1, y: 2 };
let { x, y } = p;
let r = match (p) { { x: 1, y } => y * 10, _ => 0 };
Std.string(x + y) + " " + Std.string(r)
```

### Enum constructors

`Ctor(p, q)` matches an enum value made by the constructor `Ctor`, matching its fields in order;
a constructor without fields is written `Ctor`. The constructor is named without its enum, which
the type of the matched value decides.

```jello
// expect: 7 2 5 1
enum Expr { Num(value: I32); Neg(inner: Expr); Add(left: Expr, right: Expr); }
let shape = fn(e: Expr): I32 {
  match (e) {
    Neg(Neg(Num(n))) => n,
    Neg(Num(_)) => 2,
    Add(Num(0), Num(n)) => n,
    _ => 1,
  }
};
[shape(Expr.Neg(Expr.Neg(Expr.Num(7)))), shape(Expr.Neg(Expr.Num(9))),
  shape(Expr.Add(Expr.Num(0), Expr.Num(5))), shape(Expr.Num(3))].map(fn(n: I32): Bytes { Std.string(n) }).join(" ")
```

A name that is neither a constructor of the enum nor a constant is an error:

```jello
// error: 3:44: unknown variant Branch of Pair [E0802]
enum Pair { Two(left: Pair, right: Pair); Leaf; }
let f = fn(p: Pair): I32 { match (p) { Two(Branch, _) => 1, _ => 0 } };
f(Pair.Leaf)
```

### Regular expressions

`~r/re/flags` matches Bytes that start with a match of the regular expression; `as m` binds the
match, which `std.core.RegExp`'s functions read. It may be written `when ~r/re/`, which reads
better in a `match` arm. See [Regular expressions](../04-expressions/05-regular-expressions.md).

```jello
// expect: number 42, word, other
import { group } from std.core.RegExp;
let lex = fn(s: Bytes): Bytes {
  match (s) {
    ~r/[0-9]+/ as m => "number " + group(m, 0),
    when ~r/[a-z]+/i => "word",
    _ => "other",
  }
};
[lex("42abc"), lex("Hello"), lex("!")].join(", ")
```

A regular expression may fail on any Bytes, so arms of regular expressions are never exhaustive
on their own.

### Bits

`bits[...]` matches Bytes laid out as a sequence of segments, each a type and a pattern for its
value: `u8`, `u16`, `u32`, `i32` and `f32`, with `.be` or `.le` for the byte order of the wider
ones (little-endian when left out), a number of bits (1 to 8, 16 or 32), and `rest` for the
bytes that remain. The length is tested before any segment is read. See
[Bits](../04-expressions/04-bits.md).

```jello
// expect: v1 len 3 body abc, version 9, empty
let header = fn(b: Bytes): Bytes {
  match (b) {
    bits[u8(1), u16.be(len), rest(body)] => "v1 len " + Std.string(len) + " body " + body,
    bits[u8(v), rest(_)] => "version " + Std.string(v),
    _ => "empty",
  }
};
[header(bits[u8(1), u16.be(3), rest("abc")]), header(bits[u8(9)]), header("")].join(", ")
```

### `unquote`

Inside a quote in a macro body, `unquote(value)` stands for a pattern, or a name to bind, that the
macro supplies. It is allowed nowhere else; see [Macros](05-macros.md).

## `match`

`match (subject) { arms }` evaluates the subject once and tries each arm in order. The first arm
whose pattern matches, and whose guard, if any, is true, runs. Each arm is `pattern => body`,
where the body is an expression or a block, and arms are separated by commas; a trailing comma is
allowed.

```jello
// expect: 36 small 2
enum Token { Ident(name: Bytes); Int(value: I32); Eof; }
let classify = fn(t: Token): Bytes {
  match (t) {
    Int(n) when (n > 9) => Std.string(n * 2),
    Int(_) => "small",
    Ident(name) => {
      let size = name.len();
      Std.string(size)
    },
    Eof => "end",
  }
};
[classify(Token.Int(18)), classify(Token.Int(3)), classify(Token.Ident("ab"))].join(" ")
```

A `match` is an expression. Used as a value, its arms must give values of one type; an arm that
always leaves gives none. As a statement, an arm's block may give nothing, and `{}` does nothing.

### Exhaustiveness

The arms must cover every value of the subject's type. The compiler names a value that no arm
matches, or asks for a `_` arm when the type has too many values to list, as integers, Bytes and
atoms do:

```jello
// error: 3:3: match is not exhaustive: missing false [E0700]
let f = fn(b: Bool): I32 {
  match (b) { true => 1 }
};
f(true)
```

```jello
// error: 2:27: match is not exhaustive: add a `_` arm [E0700]
let f = fn(n: I32): I32 { match (n) { 1 => 1, 2 => 2 } };
f(1)
```

An arm with a guard covers nothing, since its guard may be false. A regular expression or a pin
covers nothing either.

### Unreachable arms

An arm that can match nothing the arms before it do not already match is an error:

```jello
// error: 2:64: unreachable match arm: earlier arms already match [b] [E0701]
let f = fn(xs: Array<I32>): I32 { match (xs) { [a, ..._] => a, [b] => b, _ => 0 } };
f([1])
```

### Guards

`pattern when (condition) => body` adds a guard: a `Bool` expression, in parentheses, that is
tested after the pattern matches, and that sees the names the pattern binds. When the guard is
false, matching goes on with the next arm.

```jello
// expect: negative zero small large
let size = fn(n: I32): Bytes {
  match (n) {
    0 => "zero",
    x when (x < 0) => "negative",
    x when (x < 10) => "small",
    _ => "large",
  }
};
[size(-3), size(0), size(4), size(50)].join(" ")
```

```jello
// error: 4:28: match guard must be a Bool [E0400]
enum Token { Int(value: I32); Eof; }
let f = fn(t: Token): I32 {
  match (t) { Int(n) when (n) => 1, _ => 2 }
};
f(Token.Eof)
```

### Alternatives

Several patterns separated by commas share one body: `A, B => body` runs for either. Alternatives
that bind names must each bind the same names, with the same types. A guard written before `=>`
applies to the arm whatever alternative matched; a guard written after an earlier alternative
applies to that one alone.

```jello
// expect: 1 2 3 4 | 1 2 1
enum Token { Plus; Minus; Star; Int(n: I32); Neg(n: I32); }
let kind = fn(t: Token, unary: Bool): I32 {
  match (t) {
    Plus, Minus when (unary) => 1,
    Plus, Minus => 2,
    Int(n), Neg(n) when (n > 9) => 3,
    _ => 4,
  }
};
let spaced = fn(t: Token, left: Bool): I32 {
  match (t) { Plus when (left), Minus => 1, _ => 2 }
};
[kind(Token.Minus, true), kind(Token.Plus, false), kind(Token.Neg(12), false), kind(Token.Int(3), false)]
  .map(fn(n: I32): Bytes { Std.string(n) }).join(" ")
  + " | " + [spaced(Token.Plus, true), spaced(Token.Plus, false), spaced(Token.Minus, false)]
  .map(fn(n: I32): Bytes { Std.string(n) }).join(" ")
```

```jello
// error: 3:49: n must be bound by every alternative [E0704]
enum Token { Int(value: I32); Eof; }
let f = fn(t: Token): I32 { match (t) { Int(n), Eof => 1 } };
f(Token.Eof)
```

## `let` and `let ... else`

A `let` or `const` destructures with any pattern that cannot fail: names, `_`, tuples, records, an
array pattern that is only a spread, and the constructor of an enum that has just one. A pattern
that can fail takes `let ... else`, whose `else` block runs when the pattern does not match, and
must leave by `return`, `throw`, `break` or `continue`. The names it binds are in scope after the
statement, not in the `else`. See also [Bindings](01-bindings.md#destructuring).

```jello
// expect: 5 empty 9
enum Shape { Circle(r: I32); Square(s: I32); }
let head = fn(xs: Array<I32>): Bytes {
  let [h, ..._] = xs else { return "empty"; };
  Std.string(h)
};
let total = 0;
let rows = [[1, 9], [], [8]];
let i = 0;
while (i < rows.len()) {
  let row = rows[i];
  i++;
  let [first, ..._] = row else { continue; };
  total += first;
}
[head([5, 6]), head([]), Std.string(total)].join(" ")
```

A nullable value can fail on `null`, so destructuring one needs `else`:

```jello
// error: 2:44: this pattern can fail, as on null: use `if (let ...)` or `let ... else` [E0702]
let f = fn(m: ?Tuple<I32, I32>): I32 { let (a, b) = m; a };
f(null)
```

## `if let`

In the condition of an `if`, `let pattern = value` is a test: it is `true` when the value
matches. The names the pattern binds are in scope to its right in the condition, and in the then
branch; not in the `else`.

```jello
// expect: 3 none 7
enum Shape { Circle(r: I32); Square(s: I32); }
let radius = fn(s: Shape): Bytes { if (let Circle(r) = s) { Std.string(r) } else { "none" } };
let maybe: ?I32 = 7;
let shown = if (let is I32 as v = maybe) { Std.string(v) } else { "absent" };
[radius(Shape.Circle(3)), radius(Shape.Square(1)), shown].join(" ")
```

A pattern that is just a name binds it, as in a `let` statement, unless it names a variant of the
value's enum; then it tests for that variant, as a `match` arm would. A null value fails the
test:

```jello
// expect: stop go go
enum Light { Red; Green; }
let say = fn(l: ?Light): Bytes { if (let Red = l) { "stop" } else { "go" } };
`${say(Light.Red)} ${say(Light.Green)} ${say(null)}`
```

Tests chain with `&&`, each seeing the names bound before it. Under `||` or `!`, a `let` only
tests, and binds nothing for the rest of the condition or the branch.

```jello
// expect: 2 small
let xs = [1, 2, 3];
let a = if (let [h, ..._] = xs && h > 0 && let [_, second, ..._] = xs) { Std.string(second) } else { "no" };
let b = if (let [h, ..._] = xs && h > 1) { "big" } else { "small" };
a + " " + b
```

A `let` test may only be the condition of an `if` or a `while`, or part of one:

```jello
// error: 3:1: a `let` test can only be the condition of an `if` or a `while` [E0109]
let xs = [1, 2];
(let [h, ..._] = xs) ? 1 : 0
```

## `while let`

A `let` test in a `while` condition is tested again before each pass, and binds fresh names each
time. The loop ends when the pattern does not match.

```jello
// expect: 10 567
let sum = 0;
let rest = list[1, 2, 3, 4];
while (let [h | t] = rest) {
  sum += h;
  rest = t;
}
let xs = [5, 6, 7, 8];
let seen = "";
while (let [h, ...t] = xs && h < 8) {
  seen = seen + Std.string(h);
  xs = t;
}
Std.string(sum) + " " + seen
```

## `with`

`with` runs a sequence of clauses, each matching a value against a pattern, and runs its body when
every clause matches. A clause is `pattern <- value`, optionally with a guard,
`pattern when (condition) <- value`, or `let pattern = value`, which cannot fail. Each clause sees
the names that earlier clauses bind, and the body sees them all. When a clause fails, its `else`
arms, described below, match the value that failed.

```jello
// expect: 7 short
let heads = fn(xs: List<I32>): Bytes {
  with [h | t] <- xs, let k = h + 1, [m | _] <- t { Std.string(k + m) } else { _ => "short" }
};
heads(list[2, 4]) + " " + heads(list[2])
```

When a clause fails, the `with` stops. Without an `else`, its value is the value that failed to
match, so that value's type must fit the `with`'s:

```jello
// expect: 8 null
let f = fn(v: ?I32): ?I32 { with is I32 as x <- v { x * 2 } };
Std.string(f(4)) + " " + Std.string(f(null))
```

```jello
// error: 3:23: a clause that fails gives its value, of type ?I32, but the `with` gives Bytes; add an else to give another [E0706]
let f = fn(v: ?I32): Bytes {
  with is I32 as x <- v { "ok" }
};
f(1)
```

An `else` takes `match` arms, which match the value that failed. When the clauses' values have
different types, the arms match it as a `Dynamic`. The arms must be exhaustive, and the names the
clauses bind are not in scope in them.

```jello
// expect: circle of radius 2; not a small circle; no limit
enum Shape { Circle(r: I32); Square(s: I32); }
let area = fn(size: ?I32, shape: Shape): Bytes {
  with is I32 as limit <- size, Circle(r) when (r <= limit) <- shape {
    "circle of radius " + Std.string(r)
  } else {
    is Shape => "not a small circle",
    _ => "no limit",
  }
};
[area(5, Shape.Circle(2)), area(5, Shape.Square(2)), area(null, Shape.Circle(2))].join("; ")
```

The body must end with an expression. A `let` clause takes only patterns that cannot fail, and a
`<-` clause whose pattern cannot fail draws a warning, ``this clause cannot fail: use `let` ``. A
`with` of only `let` clauses cannot fail, so it takes no `else`.
