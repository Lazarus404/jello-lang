# Operators

Jello's operators work on values of one type at a time. Arithmetic never mixes integer widths or
integers with floats, and comparison and equality are defined for each kind of value separately.
This page describes every prefix, binary and assignment operator, and ends with the precedence
table.

The postfix forms (calls, `.member`, `[index]`) are in
[Calls and members](02-calls-and-members.md); `bits[...]` is in [Bits](04-bits.md).

## Arithmetic

`+`, `-`, `*`, `/` and `%` take two operands of the same number type and give that type. Prefix `-`
negates a number.

```jello
// expect: 17 3 -2 3.75
let a = 7;
let b = 2;
let x = 7.5;
let y = 2.0;
Std.string(a * b + 3) + " " + Std.string(a / b) + " " + Std.string(-a / 3) + " " + Std.string(x / y)
```

The operands must have the same type. There is no implicit widening, from `I32` to `I64` or from an
integer to a float; convert one side with `as` (see [Casts](02-calls-and-members.md#casts)).

```jello
// error: 3:1: integer operands must have the same type: I32 and I64 [E0500]
let small: I32 = 1; let wide: I64 = 2;
small + wide
```

```jello
// expect: 3
let small: I32 = 1; let wide: I64 = 2;
Std.string((small as I64) + wide)
```

An unsuffixed number literal takes its type from the other operand, so `wide + 1` is an `I64`
addition and `x * 2` is an `F64` multiplication when `x` is an `F64`. A variable bound to an
unsuffixed integer literal, with no other use that fixes its type, also takes the type of the
operand it is combined with; otherwise it is an `I32`.

```jello
// expect: 5.0 3
let x = 2.5;
let n = 1;
let wide: I64 = 2;
Std.string(x * 2) + " " + Std.string(n + wide)
```

### Integer overflow

Integer arithmetic wraps on overflow, in every width, signed or unsigned. Negating the smallest
signed value gives it back, and negating an unsigned value wraps too.

```jello
// expect: -2147483648 44 4294967291
let max = 2147483647;
let a: U8 = 200; let b: U8 = 100;
let u: U32 = 5;
Std.string(max + 1) + " " + Std.string(a + b) + " " + Std.string(-u)
```

### Integer division and remainder

`/` truncates toward zero, and `%` takes the sign of the left operand, so `a == (a / b) * b + a % b`
always holds. Unsigned types divide as unsigned numbers. The smallest signed value divided by `-1`
wraps to itself, and its remainder is 0.

```jello
// expect: -3 -1 -3 1 -2147483648
let q = fn(a: I32, b: I32): I32 { a / b };
let r = fn(a: I32, b: I32): I32 { a % b };
let min = -2147483648;
Bytes.join([Std.string(q(-7, 2)), Std.string(r(-7, 2)), Std.string(q(7, -2)), Std.string(r(7, -2)),
  Std.string(q(min, -1))], " ")
```

An integer `/` or `%` by zero traps with `ARITHMETIC` (see
[Runtime traps](../08-errors/02-runtime-traps.md)).

```jello
// trap: ARITHMETIC 2:40
let divide = fn(a: I32, b: I32): I32 { a / b };
divide(1, 0)
```

### Floats

`F16`, `F32` and `F64` support `+`, `-`, `*` and `/` with IEEE 754 semantics. Dividing by zero
gives an infinity or NaN rather than trapping. `%` is not defined for floats.

```jello
// expect: Infinity NaN
let zero = 0.0;
Std.string(1.0 / zero) + " " + Std.string(zero / zero)
```

```jello
// error: 3:1: operator % is not defined for F64 [E0500]
let x = 5.5;
x % 2.0
```

## Bitwise operators and shifts

`&`, `|` and `^` are bitwise and, or and exclusive or. `<<` shifts left and `>>` shifts right. All
of them take two operands of one integer type.

- `>>` is arithmetic (it copies the sign bit) for a signed type, and logical (it fills with zeros)
  for an unsigned type.
- The shift count is taken modulo the width of the type, so `x << 32` of an `I32` is `x`, and
  `x << -1` is `x << 31`.

```jello
// expect: 8 14 6 -4 2000000000 1
let a = 12; let b = 10;
let negative = -8;
let big: U32 = 4000000000;
let one = 1;
Bytes.join([Std.string(a & b), Std.string(a | b), Std.string(a ^ b), Std.string(negative >> 1),
  Std.string(big >> 1), Std.string(one << 32)], " ")
```

There is no prefix bitwise not; write `x ^ -1`.

The bitwise operators are not defined for `Bool`; use `&&`, `||` and `!=` there. Because comparisons
bind more tightly than `&`, `|` and `^` (see [Precedence](#precedence-and-associativity)), a test
such as `flags & 1 == 0` parses as `flags & (1 == 0)`, and the compiler says so.

```jello
// error: 3:12: operator & is not defined for I32 and Bool; comparisons bind tighter, so write (a & b) == c [E0500]
let flags = 6;
let even = flags & 1 == 0;
even
```

Shifts bind more tightly than `+` and `-`, unlike in C: `1 << 2 + 1` is `(1 << 2) + 1`, which is 5.

## Comparison

`<`, `<=`, `>` and `>=` compare two numbers of the same type and give a `Bool`. Unsigned types
compare as unsigned numbers. A comparison involving a float NaN is false.

```jello
// expect: true true false false
let big: U32 = 4000000000; let one: U32 = 1;
let zero = 0.0; let nan = zero / zero;
Bytes.join([Std.string(big > one), Std.string(2.5 <= 2.5), Std.string(nan < 1.0), Std.string(nan >= 1.0)], " ")
```

Only numbers are ordered. `Bytes`, atoms, `Bool` and other values support `==` and `!=` alone; to
order text, compare it byte by byte or use a library function.

```jello
// error: 2:1: Bytes support only +, == and != [E0500]
"apple" < "banana"
```

Comparisons do not chain: `a < b < c` compares the `Bool` result of `a < b` with `c`, which is an
error.

## Equality

`==` and `!=` give a `Bool`, and `!=` is always the negation of `==`. Both sides must have the same
type, except that either side may be a `null` literal, a nullable value or a `Dynamic` value, as
described below. Each operand is evaluated once, left first.

What equality means depends on the kind of value:

| Values | `a == b` when |
| --- | --- |
| Integers | they are the same number |
| Floats | they are equal under IEEE 754: NaN equals nothing, not even itself, and `-0.0 == 0.0` |
| `Bool`, atoms | they are the same value |
| `Bytes` | they hold the same bytes |
| Tuples, arrays, lists, records | they have the same length and their items are equal, in order, to any depth |
| Enums | they are the same variant and their fields are equal |
| Objects, prototype values | they are the same Object (identity) |
| Functions | they are the same function value (identity) |
| Nullable `?T` | both are null, or neither is and their values are equal |

Structural equality goes as deep as the values do:

```jello
// expect: true true true false
enum Shape { Circle(r: I32); Square(side: I32); }
Bytes.join([
  Std.string((1, "one") == (1, "one")),
  Std.string([[1, 2], [3]] == [[1, 2], [3]]),
  Std.string(Shape.Circle(2) == Shape.Circle(2)),
  Std.string(Shape.Circle(2) == Shape.Square(2))
], " ")
```

Objects, including the values a prototype makes, compare by identity, so two Objects with the same
fields are not equal unless they are the same Object:

```jello
// expect: false true
let a = { x: 1 };
let b = { x: 1 };
let alias = a;
Std.string(a == b) + " " + Std.string(a == alias)
```

### Function identity

Two function values are equal when they are the same value. A function that captures nothing is a
single value however often it is read, while each evaluation of a `fn` expression that captures
variables makes a new closure, unequal to any other.

```jello
// expect: true false false true
let f = fn() { 1 };
let g = fn() { 1 };
let h = f;
let make = fn(n: I32): () -> I32 { fn() { n } };
let c = make(1);
Bytes.join([Std.string(h == f), Std.string(f == g), Std.string(make(1) == make(1)), Std.string(c == c)], " ")
```

### Null and nullable values

A comparison with the `null` literal tests whether a nullable value is null. It is an error on a
value whose type cannot hold null. A nullable value compared with a value of its inner type is
equal when it is present and its value is equal. `x != null` also narrows `x` to its inner
type where the test is known to hold; see
[Nullability and Dynamic](../02-types/02-nullability-and-dynamic.md).

```jello
// expect: true false true
let some: ?I32 = 3;
let none: ?I32 = null;
Std.string(some == 3) + " " + Std.string(none == 3) + " " + Std.string(none == null)
```

```jello
// error: 3:1: only Null<T> values can be compared with null, not I32 [E0408]
let n: I32 = 3;
n == null
```

### Dynamic values

A `Dynamic` value can be compared with a value of a known type. It is equal when it holds a value of
that type and that value is equal. Two `Dynamic` values cannot be compared with each other; extract
one with `as T` first.

```jello
// expect: true false
let d = 5 as Dynamic;
Std.string(d == 5) + " " + Std.string(d == "5")
```

```jello
// error: 3:1: two Dynamic values cannot be compared; extract one with `as T` [E0407]
let a = 1 as Dynamic; let b = 1 as Dynamic;
a == b
```

### Type parameters

Inside a generic function, `==` and `!=` on values of a type parameter `T` need the bound `Eq`,
written `<T: Eq>`; see [Generics](../02-types/07-generics.md).

```jello
// error: 2:38: == on T needs the bound Eq: write <T: Eq> [E0501]
let same<T> = fn(a: T, b: T): Bool { a == b };
same(1, 1)
```

```jello
// expect: true
let same<T: Eq> = fn(a: T, b: T): Bool { a == b };
Std.string(same("x", "x"))
```

## Logical operators

`&&`, `||` and prefix `!` take `Bool` operands and give a `Bool`. There is no truthiness: an
integer, a nullable value or `Bytes` is not a `Bool`.

`&&` and `||` short-circuit. The right operand is evaluated only when the left one does not
already decide the result.

```jello
// expect: false true 0
let calls = 0;
let tick = fn(): Bool { calls++; true };
let a = false && tick();
let b = true || tick();
Std.string(a) + " " + Std.string(b) + " " + Std.string(calls)
```

Because the right operand of `&&` runs only when the left is true, a null test on the left narrows
a nullable variable on the right:

```jello
// expect: true
let name: ?Bytes = "Ada";
Std.string(name != null && name.len() == 3)
```

```jello
// error: 3:10: && and || require Bool operands [E0500]
let count = 1;
let ok = count && true;
ok
```

## The conditional operator

`c ? a : b` evaluates `c`, which must be a `Bool`, and then only one of `a` and `b`. Both branches
must have the same type. Without `: b`, the result is null when `c` is false, so the expression has
a nullable type.

```jello
// expect: big null
let size = 12;
let flag = false;
let label = size > 10 ? "big" : "small";
let maybe: ?I32 = flag ? 1;
label + " " + Std.string(maybe)
```

The operator is right-associative, so `a ? b : c ? d : e` is `a ? b : (c ? d : e)`.

## The null-coalescing operator

`value ?? fallback` is `value` unless that is null, in which case it is `fallback`. The left side
is evaluated once, and the right side only when it is needed, so `x ?? expensive()` costs nothing
when `x` is present. The two sides join as the branches of `?:` do: a `?Bytes` with a `Bytes`
fallback gives a `Bytes`, and with a `?Bytes` fallback, a `?Bytes`. As there, a `Dynamic` left
side needs a `Dynamic` fallback: `d ?? (0 as Dynamic)`.

```jello
// expect: anon Ada 0
let greet = fn(name: ?Bytes): Bytes { name ?? "anon" };
let first = fn(xs: Array<I64>): ?I64 { xs.len() > 0 ? xs[0] : null };
`${greet(null)} ${greet("Ada")} ${first([]) ?? 0}`
```

The operator is right-associative, so `a ?? b ?? c` tries each in turn. The fallback can leave
instead of giving a value:

```jello
// expect: 21 -1
let halve = fn(n: ?I32): I32 {
  let value = n ?? { return -1; };
  value / 2
};
`${halve(42)} ${halve(null)}`
```

A left side that cannot be null is simply the value; its fallback never runs.

## String concatenation

`+` on two `Bytes` values gives a new `Bytes` holding the left bytes followed by the right ones,
and `+=` appends to a variable. Nothing is converted to text implicitly; call `Std.string(value)`,
or use a template (see [Scalars](../02-types/01-scalars.md)).

```jello
// expect: n = 42
let text = "n";
text += " = ";
text + Std.string(42)
```

```jello
// error: 2:1: operator + is not defined for Bytes and I32 [E0500]
"n = " + 42
```

## Assignment

`target = value` stores `value` in `target`, which must be a variable declared with `let`, an
element `a[i]`, or a field `o.f`. The value of an assignment expression is the value assigned.

Each arithmetic and bitwise operator has a compound form that applies the operator to the target
and the value: `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=` and `^=`. There is no compound
form of `&&` or `||`.

```jello
// expect: 4 [1, 9, -4] 6
let x = 17;
x %= 5; x <<= 3; x >>= 2;
let a = [9, 10, -16];
a[0] %= 4; a[1] ^= 3; a[2] >>= 2;
let y = 1;
let result = (y += 5);
Std.string(x) + " " + Std.string(a) + " " + Std.string(result)
```

Assignments do not chain, because an assignment is not itself something that can be assigned:

```jello
// error: 3:7: ParseError: assignments do not chain; assign each target in its own statement [E0103]
let a = 1; let b = 2;
a = b = 3;
a
```

The target must be assignable. A `const` cannot be assigned. An Object's field holds a `Dynamic`, so
`o.f = value` stores any value, but a compound assignment to it is an error; read, update and store
it explicitly. Bytes values are immutable, so an index of `Bytes` cannot be assigned (a buffer from
`Bytes.new` can be; see [Bits](04-bits.md#byte-buffers)).

## Increment and decrement

`++` and `--` add or subtract one from an integer variable, element or typed field, wrapping like
`+` and `-`. The prefix forms, `++x` and `--x`, give the new value; the postfix forms, `x++` and
`x--`, give the value before the change.

```jello
// expect: [1, 3, 3, 2] [2, 3]
let i = 1;
let a = i++;
let b = ++i;
let c = i--;
let xs = [1, 2];
xs[0]++; ++xs[1];
Std.string([a, b, c, i]) + " " + Std.string(xs)
```

The target must be an integer and assignable:

```jello
// error: 3:1: increment and decrement require an integer variable: f [E0500]
let f = 1.5;
f++;
f
```

## Other operators

These are described with the types they work on:

- `value as T` converts or extracts a value; see [Casts](02-calls-and-members.md#casts).
- `value is T` tests what a `Dynamic` holds; see
  [Nullability and Dynamic](../02-types/02-nullability-and-dynamic.md).
- `value |> body` passes a value through a pipeline; see
  [Pipelines](02-calls-and-members.md#pipelines).
- `...value` spreads a tuple or array into a call or literal; see
  [Spread arguments](02-calls-and-members.md#spread-arguments).

## Precedence and associativity

The table lists the operators from the most tightly binding to the least. Operators in one row
have the same precedence.

| Precedence | Operators | Associativity |
| --- | --- | --- |
| 1 (tightest) | `a.b`, `a[i]`, `f(x)`, `f<T>(x)`, `x++`, `x--` | left |
| 2 | prefix `!`, `-`, `++`, `--`; `new P(args)` | right |
| 3 | `as T` | left |
| 4 | `*`, `/`, `%` | left |
| 5 | `<<`, `>>` | left |
| 6 | `+`, `-` | left |
| 7 | `<`, `<=`, `>`, `>=`, `is T` | left; `is` does not chain |
| 8 | `==`, `!=` | left |
| 9 | `&` | left |
| 10 | `^` | left |
| 11 | `\|` | left |
| 12 | `&&` | left |
| 13 | `\|\|` | left |
| 14 | `??` | right |
| 15 | `c ? a : b`, `c ? a` | right |
| 16 | `\|>` | left |
| 17 (loosest) | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `\|=`, `^=` | does not chain |

Some consequences that differ from C and its relatives:

- Shifts bind more tightly than `+` and `-`: `1 << 2 + 1` is 5.
- Postfix operations bind more tightly than prefix ones: `-a.b` is `-(a.b)`, and `-x as I64` is
  `(-x) as I64`.
- `as` binds more tightly than every binary operator: `a + b as I64` converts only `b`.
- The then-branch of `?:` is a whole expression, and the else-branch is parsed at the conditional's
  own level, which is what makes it right-associative.
- `??` binds more loosely than `||` and more tightly than `?:`, so `a ?? b ? c : d` tests
  `a ?? b`, and `x ?? 1 + 1` is `x ?? (1 + 1)`.
- A pipeline binds more loosely than `?:`, so `x |> % > 0 ? 1 : 2` gives the whole conditional the
  topic.

Parentheses group an expression without changing its value.

```jello
// expect: 5 7 -3 true
let x = 3;
let shifted = 1 << 2 + 1;
let sum = 1 + 2 * 3;
let negated = -x as I64;
let grouped = (1 + 2) * 3 == 9;
Bytes.join([Std.string(shifted), Std.string(sum), Std.string(negated), Std.string(grouped)], " ")
```
