# Macros

A macro is code that runs while the program compiles. It takes the syntax of its arguments and
emits syntax, which replaces its call.

```jello
// expect: 10 14
macro twice(x) {
  Macro.emit(quote.expr { unquote(x) + unquote(x) });
}
let n = 7;
Std.string(twice(5)) + " " + Std.string(twice(n))
```

Here `twice(5)` is replaced by `5 + 5` and `twice(n)` by `n + n` before the program is checked;
the macro's body never runs when the program does.

## Defining a macro

`macro name(a, b) { statements }` defines a macro. A macro is defined only at the top level of a
module, and a module cannot define, or import, two macros of one name.

```jello
// error: 3:1: macro m is already defined [E1000]
macro m() { quote.expr { 1 }; }
macro m() { quote.expr { 2 }; }
m()
```

Nor can a macro share its name with a value the module declares or imports at its top level,
since a call would reach the macro and every other use the value:

```jello
// error: 3:1: macro m has the name of a value of this module, which its calls would hide; rename one [E1000]
let m = fn(x: I32): I32 { x * 100 };
macro m(x) { quote.expr { unquote(x) + 1 }; }
m(5)
```

A macro is called as a function is, `name(arguments)`, anywhere in its module, before or after its
definition. Each argument is any expression, or a quote (see below), and the macro takes exactly
as many arguments as it has parameters:

```jello
// error: 5:1: macro m takes 1 argument, not 0 [E1002]
macro m(x) {
  quote.expr { unquote(x) };
}
m()
```

In the body, each parameter holds a `Fragment`: the argument's syntax, which the body cannot look
inside, only place. The body is an ordinary block of statements otherwise: it may declare locals
and functions, loop, branch and `return`.

## Quotes

`quote.kind { ... }` makes a Fragment of the syntax it holds. The kind says what the braces hold:

| Quote | Holds |
|-------|-------|
| `quote.expr { ... }` | one expression |
| `quote.stmt { ... }` | one statement |
| `quote.stmts { ... }` | one statement or more |
| `quote.type { ... }` | a type |
| `quote.pat { ... }` | a pattern |
| `quote { ... }` | statements if it starts with a keyword that starts a statement, else an expression |

The kinds may also be spelt `expression`, `statement`, `statements` and `pattern`. A quote is
allowed only in a macro body, and cannot hold another quote.

```jello
// error: 2:9: quote is only allowed in a macro body [E1001]
let q = quote.expr { 1 };
0
```

## `unquote`

Inside a quote, `unquote(value)` is a hole that the value fills when the quote is made. The value
is a Fragment, whose syntax goes in the hole, or an `I32`, `I64`, `F64`, `Bool`, `Bytes` or `null`
value, which goes in as a literal. A hole may stand for an expression, a statement, a pattern, a
name to bind or assign, or a type:

```jello
// expect: aabb 8 small!
macro twice_do(action) {
  quote.stmts { unquote(action); unquote(action); };
}
macro declare(name, ty, value) {
  quote.stmt { let unquote(name): unquote(ty) = unquote(value); };
}
macro unless(condition, body) {
  quote.stmt { if (!(unquote(condition))) { unquote(body); } };
}
let log = Array.new<Bytes>(0);
twice_do(log.push("a"));
twice_do(quote.stmt { log.push("b"); });
declare(big, I64, 7);
let out = "";
unless(big > 10, quote.stmts { out = out + "small"; out = out + "!"; });
log.join("") + " " + Std.string(big + 1) + " " + out
```

As the arguments show, a call's argument is an expression, so a macro that takes statements is
passed a quote of them. A quote passed to a macro cannot itself unquote. In a type, `unquote` takes
only the name of a variable that holds a Fragment.

`unquote` is allowed only inside a quote:

```jello
// error: 3:14: unquote is only allowed inside a quote [E1001]
macro m(x) {
  Macro.emit(unquote(x));
}
m(1)
```

## Emitting

What the body emits replaces the call. `Macro.emit(fragment)` emits a Fragment, and
`Macro.emit_all(fragments)` emits an array of them in order. A quote written on its own as a
statement of the body is emitted, so `quote.expr { 99 };` is short for
`Macro.emit(quote.expr { 99 });`.

What is emitted is expanded in turn, so it may call macros too.

### Where a macro is called

A call that is a statement, `m(...);`, is replaced by the statements the macro emits; an
expression it emits becomes an expression statement, and a macro that emits nothing removes the
call.

A call anywhere else is an expression, and the macro must emit exactly one expression there:

```jello
// error: 6:9: macro two is called where an expression is needed, so it must emit one expression, not 2 fragments [E1002]
macro two() {
  quote.stmt { 1; };
  quote.stmt { 2; };
}
let x = two();
x
```

A Fragment must also suit the hole it fills: a pattern cannot stand where an expression is
needed, nor statements where a pattern is. A macro cannot emit an `import` or a macro definition.

## The `std.macro` module

A macro body names `std.macro`'s module as `Macro`, and its `Fragment` type, without importing
them. Besides `emit` and `emit_all`:

| Function | Gives |
|----------|-------|
| `Macro.parse(source, kind = "auto")` | the syntax of the Bytes `source`, read as `"expr"`, `"stmt"`, `"stmts"`, `"type"`, `"pat"` or `"auto"` |
| `Macro.gensym(prefix)` | a name used nowhere else in the compilation |
| `Macro.literal(fragment)` | the value of an argument written as a literal, as a `Dynamic` |

```jello
// expect: 3 3 13
macro repeat(n, body) {
  let i = 0;
  while (i < (Macro.literal(n) as I32)) {
    Macro.emit(body);
    i++;
  }
}
macro define_adders() {
  Macro.emit(Macro.parse("let add = fn(): I32 { 1 + 2 };", "stmt"));
  Macro.emit(Macro.parse("let sub = fn(): I32 { 10 + 3 };"));
}
let ticks = 0;
repeat(3, quote.stmt { ticks++; });
define_adders();
Std.string(ticks) + " " + Std.string(add()) + " " + Std.string(sub())
```

`Macro.parse` reads its text once the body has finished, and text that does not parse as its kind
fails the call:

```jello
// error: 5:1: macro m emitted text that does not parse: at 1:4, expected expression [E1003]
macro m() {
  Macro.emit(Macro.parse("1 +", "expr"));
}
m()
```

### Literal arguments

`Macro.literal(x)` gives the value of an argument written as a literal, optionally with a minus:
an `I32` or `I64` integer, an `F64` float, a Bool, Bytes or `null`. Read it with `as`. Unquoting
the value makes a literal again, and a literal unquoted in a pattern matches its value.

```jello
// expect: 7 -7 2.5 hi | ten other
macro show(i, n, f, s) {
  quote.expr {
    [Std.string(unquote(Macro.literal(i) as I32)), Std.string(unquote(Macro.literal(n) as I32)),
      Std.string(unquote(Macro.literal(f) as F64)), unquote(Macro.literal(s) as Bytes)].join(" ")
  };
}
macro classify(value, expected) {
  let equal = quote.pat { unquote(expected) };
  quote.expr { match (unquote(value)) { unquote(equal) => "ten", _ => "other" } };
}
show(7, -7, 2.5, "hi") + " | " + classify(5 * 2, 10) + " " + classify(5, 10)
```

An argument that is not a literal has no value, and asking for one fails the call:

```jello
// error: 6:1: macro m failed: Macro.literal: the fragment is not a literal [E1003]
macro m(x) {
  quote.expr { unquote(Macro.literal(x) as I32) };
}
let y = 1;
m(y)
```

## Hygiene

The names in emitted syntax mean what they mean at the call. A name that a macro declares can
therefore clash with, or capture, one of the caller's:

```jello
// error: 6:1: duplicate top-level name: t [E0302]
macro bad_swap(a, b) {
  quote.stmts { let t = unquote(a); unquote(a) = unquote(b); unquote(b) = t; };
}
let t = 1; let u = 2;
bad_swap(t, u);
Std.string(t) + Std.string(u)
```

`Macro.gensym(prefix)` makes a name that nothing else in the compilation uses: the prefix, two
underscores and a number. Unquoted where a name goes, it declares or reads that name:

```jello
// expect: 21
macro swap(a, b) {
  let temp = Macro.gensym("t");
  quote.stmts { let unquote(temp) = unquote(a); unquote(a) = unquote(b); unquote(b) = unquote(temp); };
}
let t = 1; let u = 2;
swap(t, u);
Std.string(t) + Std.string(u)
```

A prefix is letters, digits and `_`, and does not start with a digit.

## Exporting and importing macros

`export macro` makes a macro importable, and `import macro { name } from module;` imports it, with
`as` to rename it. A plain `import` does not import macros, and a macro cannot be imported as part
of a namespace (`import macro a.b as m;` is an error).

Macro calls in the syntax a macro's quotes or `Macro.parse` make are calls of the macros of the
macro's own module, whether exported or not; calls in the arguments it is given are of the
caller's macros.

```jello file=lib/Macros.jello
package lib;

export macro twice(x) {
  quote.expr { unquote(x) + unquote(x) };
}

export macro quadruple(x) {
  quote.expr { twice(twice(unquote(x))) + bonus() };
}

macro bonus() {
  quote.expr { 100 };
}
```

```jello
// expect: 6 112
// flags: --path $DIR
import macro { twice as double, quadruple } from lib.Macros;
Std.string(double(3)) + " " + Std.string(quadruple(3))
```

## Limits

| Limit | Error |
|-------|-------|
| Expansions nest at most 32 deep: a call in what a call emitted is one deeper. | E0901 |
| The expansions of one module make at most 4096 syntax nodes. Each copy of an argument placed more than once counts. | E0901 |
| One run of a body takes at most 100,000,000 steps (backward jumps and calls). | E1003 |

```jello
// error: 5:1: macro forever expands 32 levels deep; does it call itself without end? [E0901]
macro forever(x) {
  quote.expr { forever(unquote(x)) };
}
forever(1)
```

```jello
// error: 8:1: macro grow made more than 4096 syntax nodes in this module [E0901]
macro grow(n) {
  let k = Macro.literal(n) as I32;
  if (k > 0) {
    quote.stmts { grow(unquote(k - 1)); grow(unquote(k - 1)); };
  }
}
grow(20);
```

```jello
// error: 5:1: macro spin runs too long; it took more than 100000000 steps [E1003]
macro spin() {
  while (true) { }
}
spin();
```

## What a body cannot do

A macro's body runs as a program of its own, on a VM of its own, before the module that defines it
is checked. So it:

- sees only its parameters, its own locals, `Macro`, `Fragment` and the built-in names such as
  `Std` and `Array`; not the module's other bindings, which do not exist yet;
- cannot `import` or `load`, or define a macro;
- communicates only through what it emits.

```jello
// error: 4:24: unknown name: limit [E0300]
let limit = 3;
macro m() {
  quote.expr { unquote(limit) };
}
m()
```

When the body throws, the call fails with the thrown value's text:

```jello
// error: 5:1: macro m failed: no thanks [E1003]
macro m() {
  throw "no thanks";
}
m();
```
