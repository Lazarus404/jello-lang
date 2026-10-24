# std.macro

`std.macro` is what a macro's body uses while it runs, as the program that calls the macro
compiles. A macro takes each argument as syntax, a `Fragment`, and emits the syntax that replaces
its call. Inside a macro body the module is in scope as `Macro`, and its type `Fragment` likewise,
without an import. For declaring macros, `quote` and `unquote`, hygiene and importing macros from
other modules, see [macros](../03-statements/05-macros.md).

```jello
// expect: 10
macro twice(x) {
  Macro.emit(quote.expr { unquote(x) + unquote(x) });
}

Std.string(twice(5))
```

## Fragment

```text
export enum Fragment {
  Argument(index: I32);
  Quoted(index: I32, holes: Array<Fragment>);
  Parsed(source: Bytes, kind: Bytes);
  Name(name: Bytes);
  Literal(value: Dynamic);
}
```

A `Fragment` is a piece of syntax: an argument of the call, a `quote`, text from `parse`, a name
from `gensym` or a literal. Its variants are for the compiler; a macro makes fragments with
`quote`, `parse` and `gensym` and passes them on without looking inside. `unquote(value)` in a
quote takes a `Fragment`, or an `I32`, `I64`, `F64`, `Bool`, `Bytes` or `null` value, which
becomes a literal.

## Functions

| Function | Description |
| --- | --- |
| `emit(fragment: Fragment)` | emits `fragment`; with any others emitted, it replaces the call |
| `emit_all(fragments: Array<Fragment>)` | emits each fragment, in order |
| `parse(source: Bytes, kind: Bytes = "auto"): Fragment` | the syntax of `source`, read as `kind` |
| `gensym(prefix: Bytes): Fragment` | a name no other `gensym` call gives while the program compiles |
| `literal(fragment: Fragment): Dynamic` | the value of a fragment that is a literal |

- **Emitting.** A `quote` written as a statement on its own is emitted too, as if passed to
  `emit`. What a macro emits is expanded again, so it may call macros.
- **`parse`** reads `kind`, one of `"expr"`, `"stmt"`, `"stmts"`, `"type"` and `"pat"`, or
  `"auto"`, which reads statements when `source` starts with a keyword that starts one and an
  expression otherwise. Any other kind throws. The text is parsed once the macro has finished, and
  text that does not parse fails the call.
- **`gensym`** gives the prefix, two underscores and a number. The prefix is letters, digits and
  `_`, and does not start with a digit; anything else throws.
- **`literal`** reads an argument written as a literal, with an optional minus, or a literal
  unquoted. Its type is the literal's, so it is read with `as`, as in `Macro.literal(n) as I32`.
  A fragment that is not a literal throws.

```jello
// expect: 2 1 21 3 4 ababab
macro swap(a, b) {
  let t = Macro.gensym("t");
  quote.stmts { let unquote(t) = unquote(a); unquote(a) = unquote(b); unquote(b) = unquote(t); };
}

macro product() {
  Macro.emit(Macro.parse("3 * 7", "expr"));
}

macro push_both(items) {
  Macro.emit_all([quote.stmt { unquote(items).push(3); }, quote.stmt { unquote(items).push(4); }]);
}

macro repeat(text, n) {
  let count = Macro.literal(n) as I32;
  let out = quote.expr { "" };
  let i = 0;
  while (i < count) { out = quote.expr { unquote(out) + unquote(text) }; i++; }
  Macro.emit(out);
}

let p = 1;
let q = 2;
swap(p, q);
let xs = Array.new<I32>(0);
push_both(xs);
Std.string(p) + " " + Std.string(q) + " " + Std.string(product()) + " " + Std.string(xs[0]) + " "
  + Std.string(xs[1]) + " " + repeat("ab", 3)
```

`quoted` and `run` are also exported; the compiler uses them to run a macro's body and read back
what it emitted, and a program does not call them.
