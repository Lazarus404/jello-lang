# Nullability and Dynamic

Most types in Jello cannot hold `null`, and every value's type is known when the program compiles.
Two kinds of type relax this. A nullable type, `?T`, holds either a `T` or `null`. `Dynamic` holds
a value of any of several types, which the program tests and extracts at run time.

## Nullable types

`?T` is the type of values that are either a `T` or `null`. `Null<T>` is another way to write the
same type.

```jello
// expect: found at 1, not found
let find = fn(items: Array<Bytes>, wanted: Bytes): ?I32 {
  let i = 0;
  while (i < items.len()) {
    if (items[i] == wanted) { return i; }
    i++;
  }
  null
};
let show = fn(at: ?I32): Bytes { at == null ? "not found" : "found at " + Std.string(at) };
show(find(["a", "b"], "b")) + ", " + show(find(["a"], "z"))
```

A `T` is accepted wherever a `?T` is needed, so a function returning `?I32` can return `i` or
`null`. Only a nullable type holds `null`; giving `null` to a plain type is an error:

```jello
// error: 2:19: only Null<T> can hold null, not Bytes [E0408]
let name: Bytes = null;
name
```

`null` alone has no type that the compiler can infer, so a variable that starts as `null` needs an
annotation: `let x: ?Bytes = null;`.

Any type except a list can be made nullable: `?I32`, `?Bytes`, `?Array<I32>`, `?Shape`. A list
is never null; its empty value is `nil` (see [Collections](03-collections.md)).

### Comparing with null

`x == null` and `x != null` test whether a nullable value is present. Only a nullable value can be
compared with `null`; comparing a plain `Bytes` with it is an error, since it can never be null.

A nullable value can also be compared with a value of its inner type: `null` equals no such value,
and a present value compares as a `T` would. Each side is evaluated once.

```jello
// expect: true false true
let some: ?I32 = 5;
let none: ?I32 = null;
`${some == 5} ${none == 5} ${none != 5}`
```

### Using a nullable value

A `?T` may be used where a `T` is needed: as an operand, an argument, a receiver of a method, and
so on. Where the compiler cannot prove the value present, it checks it when the program runs, and
a `null` there traps [`NULL_DEREF`](../08-errors/02-runtime-traps.md#null_deref):

```jello
// trap: NULL_DEREF 4:1
let count: ?I32 = null;
// Using a null as an I32 traps.
count + 1
```

To use a nullable value safely, test it first. After a test, the compiler knows that the local is
present and uses it without a check; this is called narrowing.

```jello
// expect: 5 missing 0
let length = fn(text: ?Bytes): I32 {
  if (text == null) { return 0; }
  text.len()
};
let both = fn(a: ?Bytes, b: ?Bytes): Bytes {
  if ((a != null) && (b != null)) { a + b } else { "missing" }
};
`${length("jello")} ${both("x", null)} ${length(null)}`
```

A local is narrowed:

- in the branch where `x != null` holds, or `x == null` does not;
- after an `if (x == null)` whose branch always leaves, by `return`, `throw`, `break` or
  `continue`;
- in the right operand of `&&` after `x != null`, and in a loop body under `while (x != null)`;
- after it is assigned a value that is not null.

Assigning a value that may be null to a narrowed local forgets the narrowing, as does an assignment
in code that may or may not run, such as one branch of an `if`, from the point where the two paths
join. A local that a closure assigns is always checked where it is used, since a call may run the
closure at any time.

### Unwrapping with `as`

`x as T`, for an `x` of type `?T`, is the value as a `T`, checked in the same way: it traps
`NULL_DEREF` when `x` is null.

### Producing null

A conditional without its `: else` part, `condition ? value`, is `value` when the condition holds
and `null` otherwise, so its type is `?T`. Branches of an `if`, a conditional or a `match` that give
`T` and `null`, or `T` and `?T`, join to `?T`, whichever comes first.

```jello
// expect: 3 null null
let half = fn(n: I32): ?I32 { n % 2 == 0 ? n / 2 };
let quarter = fn(n: I32) { n == 0 ? 0 : half(n / 2) };
`${half(6)} ${half(5)} ${quarter(6)}`
```

### Defaulting with `??`

`value ?? fallback` gives `value` when it is present and `fallback` when it is null, evaluating
`value` once; see
[The null-coalescing operator](../04-expressions/01-operators.md#the-null-coalescing-operator).

```jello
// expect: 3 0
let half = fn(n: I32): ?I32 { n % 2 == 0 ? n / 2 };
`${half(6) ?? 0} ${half(5) ?? 0}`
```

There is no null-safe member operator, `?.`; test the value with `== null` or `if (let ...)`
first.

### Matching nullable values

In a `match`, the pattern `null` matches a null value, and `is T as name` matches a present one,
binding it as a `T`. See [Patterns](../03-statements/03-patterns.md).

```jello
// expect: absent, 30
let describe = fn(x: ?I32): Bytes {
  match (x) { null => "absent", is I32 as n => Std.string(n * 10) }
};
describe(null) + ", " + describe(3)
```

## Dynamic

A `Dynamic` value holds a value together with what kind of value it is, so a program can test and
extract it at run time. Thrown values, the fields of an `Object`, and the results of some library
functions are `Dynamic`.

```jello
// expect: number 42, text hi, other
let describe = fn(value: Dynamic): Bytes {
  if (value is I32) { "number " + Std.string(value as I32) }
  else if (value is Bytes) { "text " + (value as Bytes) }
  else { "other" }
};
describe(42 as Dynamic) + ", " + describe("hi" as Dynamic) + ", " + describe(true as Dynamic)
```

### Making a Dynamic

A value becomes `Dynamic` only with `as Dynamic`; there is no implicit conversion, in either
direction:

```jello
// error: 2:18: a value becomes Dynamic only with `as Dynamic` [E0404]
let d: Dynamic = 5;
d
```

Almost any value can become `Dynamic`, including `null`. The exceptions are values that would not
read back as themselves: a list, whose empty value would read as `null`; a buffer from `Bytes.new`;
a `U64`, whose large values would read as negative; and a `?U64`, or a `?U32`, for the same reason.

Integers narrower than 32 bits, `I8`, `I16`, `U8` and `U16`, become `Dynamic` as an `I32` of
the same value, and a `U32` as an `I64` of the same value.

### Testing with `is`

`d is T` tests whether the `Dynamic` `d` holds a `T`. The types it can test are `Bool`, `I32`,
`I64`, `F16`, `F32`, `F64`, `Atom`, `Bytes`, an enum type, `Object`, and a prototype's type. An
enum value is tested for exactly that enum type, and an object for whether it delegates to the
prototype. Other types, such as `Array<I32>` or `U8`, cannot be tested, which is an error when the
program compiles.

`is` tests only a `Dynamic`. For a nullable value use `== null`, or the `is T` pattern in a
`match` (see above).

```jello
// error: 3:1: is tests a Dynamic value, not I32 [E0406]
let n = 1;
n is I32
```

`x is A is B` does not chain; it is an error.

`d is fn` tests whether `d` holds a function of any type. The VM does not record a function's
signature, so `is fn` binds no name in a pattern, and a function cannot be extracted with `as`:

```jello
// expect: fn other
let kind = fn(d: Dynamic): Bytes { match (d) { is fn => "fn", _ => "other" } };
let double = fn(n: I32): I32 { n * 2 };
kind(double as Dynamic) + " " + kind(2 as Dynamic)
```

### Extracting with `as`

`d as T` extracts a `T` from a `Dynamic`, for the same types that `is` can test. It succeeds
exactly when `d is T` would be true, and a `Dynamic` holding an `I32` may also be extracted as an
`I64`. Otherwise it traps when the program runs:

- [`TYPE_MISMATCH`](../08-errors/02-runtime-traps.md#type_mismatch) when `d` holds a value of
  another kind;
- [`NULL_DEREF`](../08-errors/02-runtime-traps.md#null_deref) when `d` holds `null`.

```jello
// trap: TYPE_MISMATCH 3:9
let d = "text" as Dynamic;
let n = d as I32;
Std.string(n)
```

An `I64` is extracted only as an `I64`, even when its value would fit an `I32`.

`d as Null` extracts the one value of `Null`: it is `null` when `d` holds `null`, and traps with
`TYPE_MISMATCH` otherwise. Generic code that extracts its type parameter, such as a process's
`Inbox<Null>`, works for `Null` this way.

```jello
// expect: null
let d = null as Dynamic;
`${d as Null}`
```

A `Dynamic` must be extracted before it is used as anything else. Arithmetic and arguments of
other types do not accept it, and it has no methods:

```jello
// error: 3:13: a Dynamic value must be extracted with `as T` before use [E0404]
let d = 1 as Dynamic;
let total = d + 1;
total
```

In a `match`, the pattern `is T as name` tests a `Dynamic` and binds the extracted value:

```jello
// expect: int 8, flag, text, other
let kind = fn(d: Dynamic): Bytes {
  match (d) {
    is I32 as n => "int " + Std.string(n + 1),
    is Bool => "flag",
    is Bytes => "text",
    _ => "other",
  }
};
`${kind(7 as Dynamic)}, ${kind(true as Dynamic)}, ${kind("x" as Dynamic)}, ${kind(1.5 as Dynamic)}`
```

### Comparing a Dynamic

A `Dynamic` compares with `==` and `!=`:

- with `null`, which it equals when it holds `null`;
- with a value of a type that `is` can test, which it equals when it holds a value of that type
  equal to it. An integer compares by its number, so a `Dynamic` holding the `I32` 7 equals the
  `I64` 7 and the `I8` 7.

Two `Dynamic` values cannot be compared with each other; extract one first. Nor can a `Dynamic` be
compared with a `U64`, which it never holds.

```jello
// expect: true true false true
let d = 7 as Dynamic;
let empty: Dynamic = null;
`${d == 7} ${d != "7"} ${d == null} ${empty == null}`
```

### Showing a Dynamic

`Std.string` of a `Dynamic` shows the value it holds, as it would show that value itself.

```jello
// expect: 1.5 ok null
let values = [1.5 as Dynamic, :ok as Dynamic, null as Dynamic];
`${values[0]} ${values[1]} ${values[2]}`
```
