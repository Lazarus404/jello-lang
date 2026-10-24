# Enums

An enum is a type whose values are one of a fixed set of variants. A variant may carry fields,
and `match` takes a value apart by variant.

```jello
// expect: 12.0 1.0
enum Shape {
  Square(side: F64);
  Rect(width: F64, height: F64);
  Dot;
}

let area = fn(s: Shape): F64 {
  match (s) {
    Square(side) => side * side,
    Rect(w, h) => w * h,
    Dot => 1.0,
  }
};
Std.string(area(Shape.Rect(3.0, 4.0))) + " " + Std.string(area(Shape.Dot))
```

## Declaring an enum

```text
enum Name {
  Variant;
  Variant(field: Type, field: Type);
}
```

Each variant ends with `;`. A variant's fields are written `name: Type`; the names document the
fields, and the values are given and matched by position. A field name may be a keyword. The `;`
after the closing brace is optional, and `export enum` makes the enum visible to other modules
(see [Modules](../05-modules.md)).

An enum is nominal: two enums with the same variants are different types. Variant names belong
to their enum, so two enums may both have a variant called `None`. An enum's name may not be
used for another top-level declaration.

An enum may refer to itself, and enums may refer to each other, directly or through arrays,
tuples and other types:

```jello
// expect: 6
enum Expr {
  Num(value: I32);
  Add(left: Expr, right: Expr);
  Sum(items: Array<Expr>);
}

let eval = fn(e: Expr): I32 {
  match (e) {
    Num(n) => n,
    Add(a, b) => eval(a) + eval(b),
    Sum(items) => items.reduce(0, fn(total, item) { total + eval(item) }),
  }
};
eval(Expr.Add(Expr.Num(1), Expr.Sum([Expr.Num(2), Expr.Num(3)])))
```

## Constructing values

A variant is constructed through its enum: `Shape.Rect(3.0, 4.0)` for a variant with fields, and
`Shape.Dot`, without parentheses, for one without. Each argument must have its field's type, and
the number of arguments must match the number of fields.

```jello
// error: 3:1: Color.Red has no fields, so it is written without parentheses [E0803]
enum Color { Red; Green; }
Color.Red()
```

```jello
// error: 3:5: wrong payload type for enum variant E.B [E0400]
enum E { B(x: I32); }
E.B("not int")
```

The enum's name on its own is a type, not a value:

```jello
// error: 3:1: Color is a type, not a value [E0301]
enum Color { Red; }
Color
```

## Matching

A `match` arm names a variant without its enum, and binds its fields by position. `_` skips a
field, and a field can itself be matched by a nested pattern. Several variants that bind the same
names can share an arm, separated by commas.

```jello
// expect: one 2 rest
enum Token { Int(value: I32); Pair(left: I32, right: I32); Eof; }

let describe = fn(t: Token): Bytes {
  match (t) {
    Int(1) => "one",
    Int(n), Pair(n, _) => Std.string(n),
    Eof => "rest",
  }
};
describe(Token.Int(1)) + " " + describe(Token.Pair(2, 3)) + " " + describe(Token.Eof)
```

A match on an enum must cover every variant, or end with a `_` arm. The compiler names the
variants that are missing, and rejects an arm that earlier arms already cover:

```jello
// error: 4:9: match is not exhaustive: missing Blue [E0700]
enum Color { Red; Green; Blue; }
let c = Color.Red;
let n = match (c) { Red => 1, Green => 2 };
n
```

```jello
// error: 3:39: unreachable match arm: earlier arms already match Red [E0701]
enum Color { Red; Green; }
let n = match (Color.Red) { Red => 1, Red => 2, Green => 3 };
n
```

Patterns, guards, `with` and `let ... else` are described in
[Patterns](../03-statements/03-patterns.md).

## Enum values

Enum values are ordinary values: they can be stored in arrays, tuples and records, passed and
returned, and captured by closures.

- `==` and `!=` compare variants, then fields, structurally. `e == Color.Red` tests for a
  variant without fields. Both sides must be the same enum.
- `<` and the other ordering operators are errors on enums.
- `e.tag()` is the variant's position in the declaration, from 0.
- `e.toString()`, which `Std.string` uses, shows the enum, the variant and its fields.

```jello
// expect: true false 1 Shape.Circle(3) Shape.Dot
enum Shape { Dot; Circle(r: I32); }
let s = Shape.Circle(3);
Std.string(s == Shape.Circle(3)) + " " + Std.string(s == Shape.Dot) + " "
  + Std.string(s.tag()) + " " + s.toString() + " " + Std.string(Shape.Dot)
```

```jello
// error: 4:1: enums support only == and != [E0500]
enum K { Rest; Int; }
let k = K.Int;
k < K.Rest ? 1 : 0
```

## Generic enums

An enum may take type parameters, written after its name. Each field may use them.

```jello
// expect: 7 Option.Some(7) Option.None hi
enum Option<T> { Some(value: T); None; }

let unwrapOr<T> = fn(o: Option<T>, fallback: T): T {
  match (o) { Some(v) => v, None => fallback }
};
let a = Option.Some(7);
let b: Option<I32> = Option.None;
let s = Option.Some("hi");
Std.string(unwrapOr(a, 0)) + " " + Std.string(a) + " " + Std.string(b) + " " + unwrapOr(s, "")
```

The type arguments are usually inferred from the fields and from later uses of the value. When
nothing gives them, as for `Option.None` alone, annotate the binding or write them on the enum,
`Option<I32>.None`. They are never written on the variant.

```jello
// expect: Option.None
enum Option<T> { Some(value: T); None; }
Std.string(Option<I32>.None)
```

```jello
// error: 3:5: cannot infer the type of o; annotate it [E0403]
enum Option<T> { Some(value: T); None; }
let o = Option.None;
match (o) { Some(_) => 1, None => 0 }
```

Each use of a generic enum at different type arguments is a separate type, with its own `==`
and `toString`. Generic enums may be recursive, as in
`enum Tree<T> { Leaf; Node(left: Tree<T>, value: T, right: Tree<T>); }`. See
[Generics](07-generics.md) for how instances are made and the limit on how deeply they nest.
