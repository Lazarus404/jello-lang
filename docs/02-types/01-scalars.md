# Scalars

Scalars are the simple values that every other type is built from: integers, floats, `Bool`,
`Bytes`, atoms and `null`. This page describes each type, its literals, the operations on it, and
how values convert from one scalar type to another.

## Integers

```jello
// expect: 42 -7 255 9000000000
let answer = 42;
let delta: I32 = -7;
let byte: U8 = 255;
let big: I64 = 9000000000;
`${answer} ${delta} ${byte} ${big}`
```

There are eight integer types, signed and unsigned at four widths:

| Type | Bits | Range |
|---|---|---|
| `I8` | 8 | -128 to 127 |
| `I16` | 16 | -32 768 to 32 767 |
| `I32` | 32 | -2 147 483 648 to 2 147 483 647 |
| `I64` | 64 | -9 223 372 036 854 775 808 to 9 223 372 036 854 775 807 |
| `U8` | 8 | 0 to 255 |
| `U16` | 16 | 0 to 65 535 |
| `U32` | 32 | 0 to 4 294 967 295 |
| `U64` | 64 | 0 to 18 446 744 073 709 551 615 |

### Literal types

An integer literal without a suffix takes its type from its context: an annotation, a parameter's
type, the other operand of an arithmetic operator, and so on. When nothing fixes it, it is an `I32`.
A suffix such as `i64` or `u8` fixes the type outright (see
[Integer literals](../01-lexical.md#integer-literals)).

```jello
// expect: 4294967296 300
let wide: I64 = 4294967295;
let sum = wide + 1;
let small = 3i16 * 100;
`${sum} ${small}`
```

A literal's value must be in its type's range, or the program does not compile:

```jello
// error: 2:13: integer literal 256 is out of range for U8 (0 to 255) [E0007]
let b: U8 = 256;
b
```

### Arithmetic

The arithmetic operators `+`, `-`, `*`, `/` and `%` take two operands of the same integer type
and give a result of that type. Operands are never widened to match: adding an `I64` variable to an
`I32` variable is an error, and one of them must be converted with `as`.

```jello
// error: 4:1: integer operands must have the same type: I64 and I32 [E0500]
let wide: I64 = 1;
let narrow: I32 = 2;
wide + narrow
```

Integer arithmetic wraps: a result that does not fit its type is reduced modulo 2 to the power of
the width, as two's complement hardware does. Nothing traps on overflow.

```jello
// expect: -2147483648 0 1 -128
let max: I32 = 2147483647;
let top: U8 = 255;
let low: I8 = -128;
`${max + 1} ${top + 1} ${top * 255} ${-low}`
```

Division truncates toward zero, and the remainder has the sign of the dividend, so that
`(a / b) * b + a % b == a`. Dividing the least value of a signed type by -1 wraps to that least
value, with remainder 0. Dividing by zero, with `/` or `%`, traps
[`ARITHMETIC`](../08-errors/02-runtime-traps.md#arithmetic).

```jello
// expect: -3 -1 -3 1 -2147483648
let min = -2147483648;
`${-7 / 2} ${-7 % 2} ${7 / -2} ${7 % -2} ${min / -1}`
```

Unary `-` negates, wrapping too, so negating an unsigned value gives its two's complement:
negating a `U8` that holds 1 gives 255. The increment and decrement operators `++` and `--` and
the compound assignments `+=`, `-=`, `*=`, `/=` and `%=` follow the same rules.

Comparisons, `==`, `!=`, `<`, `<=`, `>` and `>=`, also take two operands of one type, and compare by
that type's signedness: a `U32` of 4 000 000 000 is greater than 1, though its bits read as an
`I32` would be negative.

### Bitwise operations and shifts

`&`, `|` and `^` combine the bits of two integers of one type. `<<` shifts left, and `>>` shifts
right: arithmetically, copying the sign bit, for a signed type, and logically, filling with zeros,
for an unsigned one. The shift count is taken modulo 32, or modulo 64 for `I64` and `U64`; for the
narrower types the shifted value is then wrapped to the type, so `1i8 << 8i8` is 0.

```jello
// expect: 8 -4 2147483646 1
let neg = -8;
let word: U32 = 4294967292;
`${12 & 10} ${neg >> 1} ${word >> 1u32} ${1 << 32}`
```

The compound assignments `&=`, `|=`, `^=`, `<<=` and `>>=` exist too. See
[Operators](../04-expressions/01-operators.md) for precedence, and
[Bits](../04-expressions/04-bits.md) for building and matching binary data.

### Widening

A value of one integer type is accepted where another is required only when every value of its
type is a value of the other: an `I8` where an `I64` is needed, a `U8` where an `I16` is needed,
and a `U32` where an `I64` is needed, but not a `U8` where an `I8` is needed. This applies to
annotations, arguments, results, assignments and elements.

```jello
// expect: 200 -4
let byte: U8 = 200;
let wide: I64 = byte;
let add = fn(a: I64, b: I64): I64 { a + b };
let small: I8 = -5;
`${wide} ${add(small, 1)}`
```

### Converting with `as`

`value as T` converts an integer to any other integer type. The conversion keeps the low bits and
reinterprets them in the new type, so it wraps rather than failing:

```jello
// expect: 232 255 -1 4294967295
let n = 1000;
let big: U32 = 4294967295;
`${n as U8} ${-1 as U8} ${big as I32} ${-1 as U32}`
```

An unsuffixed literal before `as` is an `I32` that `as` converts, wrapping, so `300 as U8` is 44.
A literal too wide for an `I32` is instead a literal of the target type, so `5000000000 as I64` is
`5000000000i64`; it must fit that type.

### Parsing

`Integer.parse(text)` reads an optional sign and decimal digits as the integer type the context
requires, or `I32` without one. It throws (see [Exceptions](../03-statements/04-exceptions.md)) if
the text is not that form, is out of the type's range, or has a minus sign for an unsigned type.
The thrown value is the `Bytes` `"Integer.parse: not I32"`, naming the type.

```jello
// expect: 42 9000000000 Integer.parse: not U8
let n = Integer.parse("42");
let wide: I64 = Integer.parse("9000000000");
let bad = try { let b: U8 = Integer.parse("300"); "parsed" } catch (e) { e as Bytes };
`${n} ${wide} ${bad}`
```

`Integer.parse` reads decimal only: `"0x10"`, `" 1"` and `"1_000"` are all rejected.

## Floats

```jello
// expect: 1.75 0.30000000000000004 3.0
let x = 1.5;
let y = 0.25;
`${x + y} ${0.1 + 0.2} ${2 * x}`
```

There are three float types, all IEEE 754 binary floating point:

| Type | Bits | Precision |
|---|---|---|
| `F16` | 16 | half precision, about 3 decimal digits; greatest finite value 65 504 |
| `F32` | 32 | single precision, about 7 decimal digits |
| `F64` | 64 | double precision, about 16 decimal digits |

A float literal takes its type from its context, and is an `F64` when nothing fixes it; a suffix,
`f16`, `f32` or `f64`, fixes it. An integer literal in a float context is the float of its value,
which must be exactly representable: `let x: F64 = 3;` is `3.0`, but `16777217` is no `F32`.

`+`, `-`, `*` and `/` take two floats of one type, and each result is rounded to that type. There
is no `%` for floats; see `Math` below. Comparisons work as for integers, and follow IEEE 754:

- NaN, such as `0.0 / 0.0`, is unordered. Every comparison with it is false except `!=`, so
  `nan != nan` is true.
- Dividing a non-zero value by zero gives `Infinity` or `-Infinity`; nothing traps.
- `-0.0 == 0.0` is true, though the two print differently.

```jello
// expect: NaN Infinity -Infinity -0.0 false true
let nan = 0.0 / 0.0;
`${nan} ${1.0 / 0.0} ${-1.0 / 0.0} ${-0.0} ${nan == nan} ${-0.0 == 0.0}`
```

### Widening and conversions

A float widens implicitly to a wider float type, `F16` to `F32` to `F64`, and an integer widens to a
float type that holds every value of its type exactly: `I8` and `U8` to any float type; `I16` and
`U16` to `F32` or `F64`; `I32` and `U32` to `F64`. `I64` and `U64` never widen to a float.

`as` converts between any two number types:

- A float to a float rounds to the nearest value of the target, giving `Infinity` when it is too
  large: `70000.0 as F16` is `Infinity`.
- An integer to a float rounds to the nearest float.
- A float to an integer truncates toward zero: `-2.9 as I32` is -2. A value whose truncation is
  outside the target's range, or NaN, or an infinity, traps
  [`ARITHMETIC`](../08-errors/02-runtime-traps.md#arithmetic).

```jello
// expect: 7.0 -2 255 0.1 Infinity
`${7 as F64} ${-2.9 as I32} ${255.9 as U8} ${0.1 as F32} ${70000.0 as F16}`
```

```jello
// trap: ARITHMETIC 3:13
// 300 has no U8, so the conversion traps.
let level = 300.5 as U8;
Std.string(level)
```

### Float functions

| Function | Meaning |
|---|---|
| `Float.parse(text)` | Reads an optional sign, digits with an optional point, and an optional exponent, or `NaN`, `Infinity` or `-Infinity`, as the nearest `F64`. Other text throws `"Float.parse: not a number"`. |
| `Float.is_nan(x)` | Whether the number `x`, of any number type, is NaN. |
| `Float.is_infinite(x)` | Whether `x` is `Infinity` or `-Infinity`. |

`Math` provides the usual functions: `sqrt`, `cbrt`, `abs`, `floor`, `ceil`, `trunc`, `round`,
`sign`, `pow`, `exp`, `expm1`, `log`, `log10`, `log2`, `log1p`, the trigonometric and hyperbolic
functions and their inverses, `atan2`, `hypot`, `min`, `max`, `fround` and `random`. Each takes
numbers of any type and returns an `F64`. `Math.clz32` and `Math.imul` are the exceptions: they
take and return `I32`.

```jello
// expect: 1500.0 3.0 -2.0 7.5 true
let parsed = Float.parse("1.5e3");
let nan = Float.is_nan(Math.sqrt(-1));
`${parsed} ${Math.sqrt(9)} ${Math.floor(-1.5)} ${Math.max(1, 7.5, 3)} ${nan}`
```

## Bool

`Bool` has two values, `true` and `false`. `&&` and `||` evaluate their right operand only when
the left does not decide the result, and `!` negates. `==` and `!=` compare two `Bool`s.

```jello
// expect: false true false
let ready = true;
let done = false;
`${ready && done} ${ready || done} ${!ready}`
```

A condition, of `if`, `while`, the conditional operator `c ? a : b` and so on, must be a `Bool`;
nothing else, not even an integer or a nullable value, is accepted in its place. `Bool` does not
convert to or from integers with `as`.

```jello
// error: 3:5: if condition must be Bool, not I32 [E0400]
let count = 3;
if (count) { "some" } else { "none" }
```

## Bytes

`Bytes` is an immutable sequence of bytes. Text is `Bytes` holding UTF-8, but a `Bytes` value may
hold any bytes at all. Bytes literals, `"..."` or `'...'`, and templates, `` `...${hole}...` ``,
make `Bytes` (see [Lexical structure](../01-lexical.md#bytes-literals)).

```jello
// expect: 6 104 é true
let s = "héllo";
`${s.len()} ${s[0]} ${s.slice(1, 2)} ${s == "h" + "éllo"}`
```

| Operation | Meaning |
|---|---|
| `a + b` | A new `Bytes`, `a` followed by `b`. |
| `a == b`, `a != b` | Whether the two hold the same bytes. `Bytes.eq(a, b)` is the same test. |
| `s.len()` | The number of bytes, as an `I32`. Text with non-ASCII characters has more bytes than characters. |
| `s[i]`, `s.get(i)` | The byte at index `i`, from 0, as an `I32` from 0 to 255. |
| `s.slice(start, length)` | A new `Bytes` of `length` bytes from `start`. |

An index or slice outside the bytes traps [`BOUNDS`](../08-errors/02-runtime-traps.md#bounds).
`Bytes` have no ordering operators; `<` on them is an error. Each method is also a function of
`Bytes`: `s.len()` is `Bytes.len(s)`, and `s.slice(1, 2)` is `Bytes.slice(s, 1, 2)`.

The module `std.core.Bytes` adds functions such as `split`, `join`, `indexOf`, `startsWith`,
`trim` and `replace`, which can also be called as methods, and the `utf8...` functions that count
in code points rather than bytes. See the [standard library](../06-stdlib/README.md).

Because `Bytes` cannot change, assigning to an element is an error:

```jello
// error: 3:1: Bytes values are immutable; build bytes in a buffer from Bytes.new [E0412]
let s = "abc";
s[0] = 65;
s
```

To build bytes piece by piece, use a buffer. `Bytes.new(n)` makes a mutable buffer of `n` zero
bytes. Its elements can be assigned, with `buffer[i] = byte`, `buffer.set(i, byte)` or
`Bytes.set_u8(buffer, i, byte)`, and `buffer.toString()` copies its bytes out as `Bytes`. A byte
outside 0 to 255 traps `BOUNDS`. A buffer is not `Bytes`: it cannot be passed where `Bytes` are
needed until it is copied out.

```jello
// expect: jel
let buffer = Bytes.new(3);
buffer[0] = "j".code;
buffer.set(1, "e".code);
Bytes.set_u8(buffer, 2, "l".code);
buffer.toString()
```

## Atoms

An atom is an interned name, written `:name` with no space after the colon. Atoms are cheap to
compare and are useful as tags and keys; two atoms are equal exactly when their names are. Their
type is `Atom`.

```jello
// expect: true false ready [:ready, :done]
let state = :ready;
let named = Atom.intern("ready");
`${state == named} ${state == :done} ${state} ${[state, :done]}`
```

| Operation | Meaning |
|---|---|
| `a == b`, `a != b` | Whether two atoms have the same name. Atoms have no ordering. |
| `Atom.intern("name")` | The atom of a bytes literal's name; the argument must be a literal. |
| `a.toString()` | The atom's name, as `Bytes`. |

`Std.string` of an atom is its name alone, but inside an array, tuple or other value it is shown
with its colon, so that it reads back as an atom.

## Null

`null` is the value that means "nothing here". Only a nullable type, `?T` (also written
`Null<T>`), holds it, and only a nullable value can be compared with it. See
[Nullability and Dynamic](02-nullability-and-dynamic.md).

```jello
// expect: none
let found: ?I32 = null;
found == null ? "none" : "some"
```

## Showing values

`Std.string(value)` returns any value as `Bytes`, and a template's hole shows its value the same
way.

- `Bytes` are shown as themselves.
- An integer is shown in decimal, by its type's signedness.
- A float is shown with the fewest digits that read back as the same value of its type, and always
  with a point or an exponent: `3.0`, `0.1`, `1e+21`, `NaN`, `-Infinity`.
- `true` and `false`, `null`, and an atom's name.
- Anything else is shown as a literal that would read back: arrays as `[1, 2]`, tuples as
  `(1, "one")`, enum values as `Shape.Circle(2)`, with `Bytes` inside them quoted and escaped.

```jello
// expect: plain 3.0 null (1, "one", :a, [true])
let value = (1, "one", :a, [true]);
let shown = [Std.string("plain"), Std.string(3.0), Std.string(null), Std.string(value)];
`${shown[0]} ${shown[1]} ${shown[2]} ${shown[3]}`
```
