# Collections

Jello has four built-in ways to hold several values:

| Type | Literal | Size | Mutable | Equality |
|------|---------|------|---------|----------|
| `Tuple<A, B, ...>` | `(a, b)` | fixed, each item its own type | no | structural |
| `Array<T>` | `[a, b]` | grows and shrinks | yes | structural |
| `List<T>` | `list[a, b]` | immutable, shares tails | no | structural |
| `Object` | `{ a: 1 }` | fields added and removed at run time | yes | identity |

A record typedef gives an object literal a fixed, typed shape instead; see
[Typedefs and records](06-typedefs-and-records.md).

## Tuples

A tuple groups a fixed number of values, each of its own type. Its items are read by position
with `.0`, `.1` and so on, or taken apart with a pattern.

```jello
// expect: 7 ok 7 2
let pair = (7, "ok");
let (n, word) = pair;
Std.string(pair.0) + " " + word + " " + Std.string(n) + " " + Std.string(pair.len())
```

The type of `(7, "ok")` is written `Tuple<I32, Bytes>`. A one-item tuple needs a trailing comma,
`(42,)`, because `(42)` is just `42` in parentheses; the empty tuple is `()`.

```jello
// expect: (1,) () 0
let one = (1,);
let none = ();
Std.string(one) + " " + Std.string(none) + " " + Std.string(Tuple.len(none))
```

In a type, parentheses mean a function's parameter list, not a tuple, so a tuple type is always
written `Tuple<...>`:

```jello
// error: 2:8: tuple types are written Tuple<A, B> [E0401]
let t: (I32, Bytes) = (1, "a");
t.0
```

Rules:

- A tuple's items cannot be assigned, incremented or decremented after it is made.
- `.N` must name an item the tuple has; `t.2` on a pair is a compile-time error, and so is
  indexing a tuple with `t[i]`.
- `==` and `!=` compare tuples item by item. Both sides must have the same type.
- `t.len()` (or `Tuple.len(t)`) is the number of items, and `t.toString()` is how `Std.string`
  shows it.
- `Tuple.get(t, i)` reads item `i` as a `Dynamic`, for code that does not know the index until
  run time. An index past the end traps with `BOUNDS`.
- A tuple literal may spread another tuple's items among its own: `(0, ...t, 5)`.

```jello
// error: 3:1: tuple fields cannot be assigned [E0103]
let pair = (1, 2);
pair.0 = 5;
pair.0
```

```jello
// expect: 3 true
let t = (1, 2, 3);
let u = (0, ...t);
let item: Dynamic = Tuple.get(u, 2);
Std.string(u.3) + " " + Std.string(item == 2)
```

## Arrays

An array is a mutable, growable sequence of values of one type, `Array<T>`. A literal lists its
items in brackets; `xs[i]` reads and writes an item, counting from 0.

```jello
// expect: [10, 2, 3, 4] 4
let xs = [1, 2, 3];
xs[0] = 10;
xs.push(4);
Std.string(xs) + " " + Std.string(xs.len())
```

A literal may end with a comma, which keeps a list written one item per line tidy:

```jello
// expect: [2, 3, 5, 7]
let primes = [
  2,
  3,
  5,
  7,
];
Std.string(primes)
```

The element type comes from the items, from an annotation, or from a later use. An empty literal
with none of these is an error, because nothing says what it holds:

```jello
// expect: [3] []
let later = [];
later.push(3);
let words: Array<Bytes> = [];
Std.string(later) + " " + Std.string(words)
```

```jello
// error: 2:5: cannot infer the type of xs; annotate it [E0403]
let xs = [];
0
```

Every item must have the element type; integer literals take the element width.

```jello
// error: 2:14: array element must be I32 [E0400]
let xs = [1, "two"];
0
```

`Array.new<T>(n)` makes an array of `n` items. Integers start as `0`, floats as `0.0` and `Bool`
as `false`; items of any other type are null until set, and reading one before then traps with
`NULL_DEREF`. `Std.string` shows an item not yet set as `null`.

```jello
// expect: [0, 0, 0] [false] ["a", null]
let counts = Array.new<I32>(3);
let flags = Array.new<Bool>(1);
let names = Array.new<Bytes>(2);
names[0] = "a";
Std.string(counts) + " " + Std.string(flags) + " " + Std.string(names)
```

### Indexing and bounds

The index is an `I32`. An index below 0 or at or past the length traps with `BOUNDS` when the
program runs:

```jello
// trap: BOUNDS 3:1
let xs = [1, 2];
xs[2]
```

An element can be the target of `=`, a compound assignment such as `+=`, and `++` or `--`. The
array, the index and the value are each evaluated once, in that order.

```jello
// expect: [11, 20] [[1, 12], [3, 4]]
let counts = [10, 20];
counts[0]++;
let grid = [[1, 2], [3, 4]];
grid[0][1] += 10;
Std.string(counts) + " " + Std.string(grid)
```

Arrays are shared by reference: assigning one to another variable, passing it, or capturing it
in a closure does not copy it. `xs.copy()` makes a new array with the same items.

### Built-in operations

These are part of every array and work as methods or as `Array.` functions (`xs.len()` or
`Array.len(xs)`):

| Operation | Result |
|-----------|--------|
| `Array.new<T>(n)` | a new array of `n` items |
| `xs.len()` | the number of items |
| `xs.push(v)` | appends `v`; returns the new length |
| `xs.pop()` | removes and returns the last item; traps with `BOUNDS` when empty |
| `xs.resize(n)` | sets the length to `n`, filling new items as `Array.new` does; returns `xs` |

The `std.core.Array` module adds more, which are also called as methods without an import:
`copy`, `slice(start, count)`, `indexOf`, `contains`, `first`, `last`, `map`, `filter`,
`reject`, `reduce`, `forEach` (and their `Indexed` forms), `insert`, `remove`, `shift`,
`unshift`, `splice`, `reverse`, `sort` and `join`. `reverse` and `sort` change the array in
place. See the [standard library](../06-stdlib/README.md) for each one.

```jello
// expect: [2, 4, 6] [2] 6 [1, 2, 3] a-b
let xs = [3, 1, 2];
let doubled = [1, 2, 3].map(fn(x) { x * 2 });
let evens = xs.filter(fn(x) { x % 2 == 0 });
let total = xs.reduce(0, fn(sum, x) { sum + x });
xs.sort(fn(a, b) { a - b });
Std.string(doubled) + " " + Std.string(evens) + " " + Std.string(total) + " " + Std.string(xs)
  + " " + ["a", "b"].join("-")
```

### Spreading arrays

An array literal may spread other arrays among its items with `...`. The result is a new array;
the spread arrays are not changed.

```jello
// expect: [0, 1, 2, 3, 9]
let xs = [1, 2, 3];
let ys = [0, ...xs, ...[], 9];
Std.string(ys)
```

The spread array's element type must be the literal's exactly; an `Array<U8>` does not spread
into an `Array<I32>`.

### Equality

`==` on two arrays of the same type compares their lengths and then their items in order.

```jello
// expect: true false
Std.string([1, 2] == [1, 2]) + " " + Std.string([1, 2] == [2, 1])
```

## Lists

A `List<T>` is an immutable singly linked list. Adding an item in front makes a new list that
shares the old one as its tail, so lists suit recursive and functional code.

```jello
// expect: [1, 2, 3] [0, 1, 2, 3] 1 [2, 3]
let xs = list[1, 2, 3];
let ys = list[0 | xs];
Std.string(xs) + " " + Std.string(ys) + " " + Std.string(xs.head()) + " " + Std.string(xs.tail())
```

The literal forms are:

| Form | Meaning |
|------|---------|
| `list[]` | the empty list, nil; it needs an annotation or a context that gives `T` |
| `list[a, b, c]` | the items in order |
| `list[a, b \| tail]` | `a` and `b` in front of the list `tail`, which is shared, not copied |
| `list[a, ...rest, b]` | `a`, then the items of `rest`, then `b`; `rest` is copied |
| `list[for p <- xs, do: body]` | a comprehension (below) |

Items, tails and spreads are evaluated left to right.

```jello
// expect: [7, 1, 2, 8] []
let xs = list[1, 2];
let empty: List<I32> = list[];
Std.string(list[7, ...xs, 8]) + " " + Std.string(empty)
```

The built-in operations are `List.nil<T>()`, `List.cons(head, tail)`, `List.head(xs)`,
`List.tail(xs)` and `List.is_nil(xs)`. The head or tail of nil traps with `NULL_DEREF`. The
`std.core.List` module adds methods much like the array ones (`len`, `map`, `filter`, `reduce`,
`reverse`, `append`, `concat`, `slice`, `toArray`, `join`, `sort` and others). Those that produce
a list return a new one and leave the original alone.

```jello
// expect: [11, 12, 13] 6 [3, 2, 1] 3
let xs = list[1, 2, 3];
Std.string(xs.map(fn(x: I32): I32 { x + 10 })) + " "
  + Std.string(xs.reduce(0, fn(sum: I32, x: I32): I32 { sum + x })) + " "
  + Std.string(xs.reverse()) + " " + Std.string(xs.len())
```

A list pattern takes a list apart: `[]` matches nil, `[a, b]` a list of exactly two items, and
`[head | tail]` any non-empty list. See [Patterns](../03-statements/03-patterns.md).

```jello
// expect: 6
let sum = fn(xs: List<I32>): I32 {
  match (xs) { [] => 0, [head | tail] => head + sum(tail) }
};
sum(list[1, 2, 3])
```

### Lists and null

The empty list is nil, not null, and the compiler keeps the two apart. A list type cannot be
nullable, and a list cannot become a `Dynamic` or be thrown, because its nil would then read as
null. Test for the empty list with `List.is_nil(xs)` or `xs.isEmpty()`.

```jello
// error: 2:12: a list cannot be null: its empty value is nil; test it with List.is_nil [E0408]
let f = fn(xs: ?List<I32>): I32 { 0 };
0
```

### Comprehensions

`list[for pattern <- source, do: body]` makes a list of `body` for each item of `source`, which
must be a `List`. A generator may have a guard, `when (condition)`, that skips items, and a
comprehension may have several generators separated by commas; each one loops inside the one
before it and sees the names bound before it. An item that the generator's pattern does not match
is skipped. The comma before `do:` is optional.

```jello
// expect: [10, 30] [1, 10, 2, 20] [3, 7]
let tens = list[for x <- list[1, 2, 3] when (x != 2), do: x * 10];
let pairs = list[for x <- list[1, 2], y <- list[x, x * 10] do: y];
let sums = list[for (a, b) <- list[(1, 2), (3, 4)], do: a + b];
Std.string(tens) + " " + Std.string(pairs) + " " + Std.string(sums)
```

The names a generator binds belong to the comprehension and are gone after it. `break` and
`continue` in the body cannot leave the comprehension for a loop around it.

```jello
// error: 2:24: comprehension source must be a List, not Array<I32> [E0400]
let ys = list[for x <- [1, 2, 3], do: x];
0
```

## Objects

An object literal in braces makes an `Object`, a mutable map from atom keys to values of any
type. Its fields read as `Dynamic`, so the value is checked when it is converted to a type.

```jello
// expect: 3 two
let o = { a: 1, b: "two" };
o.c = 2;
let sum = (o.a as I32) + o.get<I32>(:c);
Std.string(sum) + " " + (o.b as Bytes)
```

An entry of a literal is one of:

| Entry | Meaning |
|-------|---------|
| `name: value` | the field `name`; a keyword may be a field name |
| `name` | shorthand for `name: name` |
| `[key]: value` | a computed field; `key` is an `Atom` or `Bytes` |
| `...other` | every field of the `Object` `other`, copied in |

Entries are evaluated in source order, and a later entry for the same field replaces an earlier
one. `{}` is the empty object. `Std.string` and `keys()` list an object's fields in the order of
their atoms, which is not always the order the fields were added.

```jello
// expect: {a: 1, b: 3, c: 4} {k: true, x: 1}
let base = { a: 1, b: 2 };
let copied = { ...base, b: 3, c: 4 };
let x = 1;
let key = :k;
Std.string(copied) + " " + Std.string({ x, [key]: true })
```

Reading a field that the object does not have gives null. `o[key]` reads and writes a field by a
computed key, and the methods `get<T>(key)`, `set(key, value)`, `exists(key)`, `remove(key)`,
`keys()`, `values()`, `len()`, `isEmpty()`, `copy()`, `merge(other)` and `clear()` work on any
object. `get<T>` traps with `TYPE_MISMATCH` when the field holds another type, and with
`NULL_DEREF` when it is missing.

```jello
// expect: [:a, :b] true false 1
let o = { a: 1 };
o[:b] = 2;
let had = o.remove(:b);
Std.string(Object.keys({ a: 1, b: 2 })) + " " + Std.string(had) + " " + Std.string(o.exists(:b))
  + " " + Std.string(o.len())
```

Objects are shared by reference and `==` compares identity: two objects are equal only when they
are the same object.

```jello
// expect: true false
let o = { a: 1 };
let alias = o;
Std.string(o == alias) + " " + Std.string(o == { a: 1 })
```

When the context expects a record type, a literal in braces makes a record instead of an
`Object`; see [Typedefs and records](06-typedefs-and-records.md). Methods, prototypes and
`new` are described in [Objects and prototypes](../04-expressions/03-objects-and-prototypes.md).
