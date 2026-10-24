# Control flow

Jello has blocks, `if`, `match`, `with` and `try` as expressions, and `while`, `do ... while`,
`for`, `break`, `continue`, `return` and `throw` as statements. This page covers blocks, `if` and
the loops; [Patterns](03-patterns.md) covers `match` and `with`, and
[Exceptions](04-exceptions.md) covers `throw` and `try`.

## Blocks

A block, `{ ... }`, is a sequence of statements with a scope of its own. Each statement ends with
`;`. If the last thing in the block is an expression without a `;`, that expression is the
block's value.

```jello
// expect: 20
let v = {
  let a = 2;
  a * 10
};
v
```

A program works the same way: its value is the expression that ends it without a `;`. When that
value is Bytes, the VM prints it (see [jello](../07-tooling/02-jello.md)); a program that ends
with `;` has no value and prints only what it writes.

A block used as a value must end with an expression:

```jello
// error: 2:9: value block requires a trailing expression [E0410]
let v = {
  let a = 2;
};
v
```

A block, branch or arm whose every path leaves, by `return`, `throw`, `break` or `continue`, needs
no value: it never finishes, so it gives the enclosing expression no value to check. An
expression after the jump is allowed and never runs; a statement after it is an error.

```jello
// error: 4:3: unreachable statement [E0701]
let f = fn(n: I32): I32 {
  if (n < 0) { return 0; } else { return 1; }
  let x = 2;
  x
};
f(1)
```

A block on its own is a statement. Like a statement that starts with `if`, `match`, `with` or
`try`, it ends at its closing brace, so a following line that starts with `-` or `(` begins a new
statement rather than continuing an expression.

```jello
// expect: 98
let total = 1;
{
  let t = 4;
  total += t;
}
if (total > 0) { total = 100; }
-2 + total
```

## `if`

`if (condition) { ... } else { ... }` runs one branch. The condition must be a `Bool`, the
parentheses are required, and each branch is a block: braces are required even around a single
statement. `else if` chains further tests.

```jello
// expect: neg zero pos
let sign = fn(n: I32): Bytes {
  if (n < 0) {
    "neg"
  } else if (n == 0) {
    "zero"
  } else {
    "pos"
  }
};
[sign(-4), sign(0), sign(9)].join(" ")
```

```jello
// error: 3:7: if condition must be Bool, not I32 [E0400]
let f = fn(n: I32): I32 {
  if (n) { return 0; }
  1
};
f(1)
```

An `if` is an expression. Used as a value, it needs an `else`, and its branches must have the same
type. A branch that is `null` makes the result nullable; a branch that always leaves gives no
value, so the other branch alone gives the type.

```jello
// expect: big 1 null 3
let v = 20;
let size = if (v > 5) { "big" } else { "small" };
let maybe = fn(b: Bool) { if (b) { 1 } else { null } };
let half = fn(n: I32): I32 { if (n % 2 == 0) { n / 2 } else { throw "odd"; } };
[size, Std.string(maybe(true)), Std.string(maybe(false)), Std.string(half(6))].join(" ")
```

```jello
// error: 3:9: an if used as a value needs an else branch [E0410]
let v = 20;
let m = if (v > 100) { 1 };
m
```

```jello
// error: 2:32: if branches must have the same type, not I32 and Bytes [E0400]
let v = if (true) { 1 } else { "a" };
v
```

An `if` without `else` can only be a statement. As the last form of a function's body, an `if`
whose branches end with `;` is one, and the function can then end without a value, as
`fn(n: I32) { if (n > 0) { log.push(n); } }` does; a function with a result type other than `Null`
needs a value after it.

The condition of an `if` may test a pattern with `let`, as in `if (let [head, ..._] = xs) { ... }`;
see [Patterns](03-patterns.md#if-let).

### The conditional operator

`condition ? a : b` is the expression form of `if`: it evaluates `a` or `b`. Written without
`: b`, it gives `null` when the condition is false.

```jello
// expect: yes null
let v = 20;
let answer = v > 5 ? "yes" : "no";
let rare = v > 100 ? 7;
answer + " " + Std.string(rare)
```

The operator binds more loosely than `||` and more tightly than assignment, and groups to the
right, so `a ? b : c ? d : e` is `a ? b : (c ? d : e)`. See
[Operators](../04-expressions/01-operators.md).

## `while`

`while (condition) { ... }` tests the condition and runs the body for as long as it is `true`. The
condition must be a `Bool`. A loop is a statement and has no value.

```jello
// expect: 55
let total = 0;
let i = 1;
while (i <= 10) {
  total += i;
  i++;
}
total
```

```jello
// error: 3:10: while condition must be Bool, not I32 [E0400]
let f = fn(n: I32): I32 {
  while (n - 1) { return 0; }
  1
};
f(1)
```

The condition may also test a pattern, as in `while (let [h | t] = rest) { ... }`, which binds
fresh names before each pass; see [Patterns](03-patterns.md#while-let).

A `while (true)` that no `break` leaves never finishes, so nothing after it runs, and a function
that ends with one needs no value after it:

```jello
// expect: 15
let firstAbove = fn(threshold: I32): I32 {
  let i = 0;
  while (true) {
    if (i > threshold) { return i; }
    i++;
  }
};
firstAbove(14)
```

## `do ... while`

`do { ... } while (condition);` runs its body once before it first tests the condition. The
`;` after the condition is required.

```jello
// expect: 1 3
let runs = fn(limit: I32): I32 {
  let n = 0;
  do { n++; } while (n < limit);
  n
};
Std.string(runs(0)) + " " + Std.string(runs(3))
```

The body's locals are not in scope in the condition, and the condition cannot be a `let` test.

```jello
// error: 4:11: unknown name: done [E0300]
do {
  let done = true;
} while (!done);
"ok"
```

## `for`

`for (item in items) { ... }` runs its body once for each item of an `Array`, a `Bytes` or a
buffer from `Bytes.new` (each byte as an `I32`), or a `List`, in order. A nullable collection is
unwrapped as any operand is, so a null one traps with `NULL_DEREF`. `for (index, item in items) { ... }` also names the
item's position, counting from 0.

```jello
// expect: 6 0:a 1:b 2:c
let total = 0;
for (n in [1, 2, 3]) { total += n; }
let labels = Array.new<Bytes>(0);
for (i, word in list["a", "b", "c"]) { labels.push(`${i}:${word}`); }
`${total} ${Bytes.join(labels, " ")}`
```

The item may be a pattern that cannot fail, which takes each item apart:

```jello
// expect: 11
let total = 0;
for ((a, b) in [(1, 2), (3, 3)]) { total += a * b; }
total
```

Each pass binds fresh names, so a closure made in the body keeps the item it saw. `break` and
`continue` work as in `while`, and the names are not visible after the loop. An array's length
is read before each pass, so items the body pushes are walked too; a list is walked as it was when
the loop began.

## `break` and `continue`

`break` leaves the innermost loop. `continue` ends the current pass of the innermost loop: in a
`while`, it goes on to the next test of the condition, and in a `do ... while`, to the test that
follows the body. Neither takes a value or a label.

```jello
// expect: 18 6
let total = 0;
let i = 0;
while (i < 100) {
  i++;
  if (i == 3) { continue; }
  if (i > 6) { break; }
  total += i;
}
Std.string(total) + " " + Std.string(i - 1)
```

Both must be inside a loop of the same function; a function inside a loop is not part of it.

```jello
// error: 3:32: break outside loop [E0307]
let i = 0;
while (i < 3) { let g = fn() { break; }; i++; }
i
```

## `return`

`return value;` leaves the function with a value; `return;` leaves it with none, which is only
allowed when the function's result is `Null` or inferred. A function's last expression is its
value, so `return` is needed only to leave early. `return` is only allowed inside a function.

```jello
// expect: 0 3
let clamp = fn(n: I32): I32 {
  if (n < 0) { return 0; }
  n
};
Std.string(clamp(-5)) + " " + Std.string(clamp(3))
```

```jello
// error: 2:21: return needs a value of type I32 [E0400]
let f = fn(): I32 { return; };
f()
```

`return`, `throw`, `break` and `continue` may leave out their `;` just before the `}` that closes
their block:

```jello
// expect: 3
let f = fn(n: I32): I32 { if (n > 2) { return n } 0 };
f(3)
```
