# Lexical structure

This page describes how the compiler reads a source file into tokens: the characters it accepts,
comments, names and keywords, literals, operators, and how statements are separated.

## Source text

A source file is a sequence of bytes that must be valid UTF-8. The compiler rejects a file that is
not, pointing at the first byte that starts no valid UTF-8 sequence (`invalid UTF-8 [E0001]`).

Outside literals and comments, only ASCII is meaningful: names, keywords, numbers and operators are
all ASCII. Any other character there, such as a letter with an accent or a control character, is
an error:

```jello
// error: 2:8: unexpected character [E0002]
let café = 1;
```

Non-ASCII text is fine inside bytes literals, templates, regexp literals and comments, where it is
kept as its UTF-8 bytes.

Whitespace is the space, tab, newline, vertical tab, form feed and carriage return. It separates
tokens and is otherwise ignored; in particular, a newline never ends a statement (see
[Semicolons and newlines](#semicolons-and-newlines)).

## Comments

There are three forms of comment:

| Form | Extent |
|---|---|
| `// text` | to the end of the line |
| `# text` | to the end of the line |
| `/* text */` | to the next `*/`, across lines |

```jello
// expect: 3
/* A block comment
   may span lines. */
let a = 1; // a line comment
let b = 2; # another line comment
Std.string(a + b)
```

Block comments do not nest: the first `*/` ends the comment, whatever `/*` came before it. The
`*/` must come after the opening `/*`, so `/*/` only opens a comment. A block comment that reaches
the end of the file is an error (`unterminated block comment [E0005]`).

A documentation comment, `/** ... */`, is an ordinary block comment by convention; the compiler
gives it no meaning. [`jelloc fmt`](07-tooling/08-formatting.md) writes a comment of one line as a
`//` comment, and a run of `//` lines as one block comment.

## Names

A name (an identifier) starts with an ASCII letter or `_`, followed by any number of ASCII letters,
digits and `_`. Case matters, so `count` and `Count` are different names.

```text
x   total   _unused   parseHeader   Shape   I32   point_2d
```

By convention, values and functions are `camelCase`, and types, enum variants and prototypes are
`PascalCase`. A leading `_` conventionally marks a helper that a module does not export, though
only `export` decides that (see [Modules](05-modules.md)). `_` alone is a pattern that binds
nothing (see [Patterns](03-statements/03-patterns.md)).

## Keywords

These words are reserved and cannot be used as names:

```text
as        await     bits      break     catch     const     continue  do
else      enum      export    false     fn        for       from      if
import    is        let       list      load      macro     match     new
null      package   prototype return    super     this      throw     true
try       type      typedef   when      while     with
```

`await` is reserved for future use and is not accepted in any expression. `list` is a keyword only
when `[` follows it, as in `list[1, 2]`; otherwise it can name a variable.

After a `.`, a keyword may be used as a member name, so `o.type` reads the field `type` and a
module path may contain a keyword, as in `std.macro`.

A few words are names to the lexer but have a meaning in certain positions:

| Word | Where it is special |
|---|---|
| `method` | before `fn`, a function that sees `this` (see [Objects and prototypes](04-expressions/03-objects-and-prototypes.md)) |
| `quote`, `unquote` | in macro bodies (see [Macros](03-statements/05-macros.md)) |
| `code` | after a one-character literal, `"A".code` (see [Character codes](#character-codes)) |
| `bytes`, `layout` | in `typedef T = bytes-layout { ... }` (see [Typedefs and records](02-types/06-typedefs-and-records.md)) |
| `le`, `be` | byte order in bit segments (see [Bits](04-expressions/04-bits.md)) |

## Integer literals

An integer literal is a run of decimal digits, or a radix prefix followed by digits:

| Form | Digits | Example |
|---|---|---|
| decimal | `0` to `9` | `255` |
| hexadecimal, `0x` or `0X` | `0` to `9`, `a` to `f`, `A` to `F` | `0xFF` |
| binary, `0b` or `0B` | `0`, `1` | `0b1111_1111` |
| octal, `0o` or `0O` | `0` to `7` | `0o377` |

A radix literal may separate its digits with `_`, anywhere after the prefix; a decimal literal may
not. A radix prefix with no digits, such as `0x`, is an error.

```jello
// expect: [255, 255, 255, 255, 65535]
Std.string([255, 0xFF, 0b1111_1111, 0o377, 0xFFFF])
```

A literal may end in a suffix that fixes its type: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`
or `u64`. Without a suffix, an integer literal takes its type from its context, and is an `I32`
when nothing fixes it. A float suffix, `f16`, `f32` or `f64`, makes an integer literal a float.

```jello
// expect: (7, 200, 4294967296, 1.0)
let wide = 4294967296i64;
let byte = 200u8;
Std.string((7i16, byte, wide, 1f32))
```

The literal's value must fit its type, or the program does not compile; a minus sign written
directly before a literal is part of its value for this check. See [Scalars](02-types/01-scalars.md)
for the integer types and their ranges.

```jello
// error: 2:11: integer literal 2147483648 is out of range for I32 (-2147483648 to 2147483647) [E0007]
let big = 2147483648;
big
```

A number may not run straight into a letter, a digit that its radix does not have, or a second dot:
`12ab`, `0b102` and `1.5.2` are errors.

## Float literals

A float literal has a decimal point, an exponent, or both:

```text
1.5    0.25    .5    2.    1e3    1.5e-3    2E+8
```

The forms are digits with a point and optional fraction (`2.`, `2.5`), a fraction alone (`.5`),
and either of those, or plain digits, followed by an exponent, `e` or `E`, an optional sign and
digits. A plain run of digits is an integer.

A float literal may end in `f16`, `f32` or `f64`. Without a suffix, it takes its type from its
context, and is an `F64` when nothing fixes it.

```jello
// expect: (1000.0, 0.5, 2.0, 0.0015, 0.25)
let quarter: F32 = 0.25;
Std.string((1e3, .5, 2., 1.5e-3f32, quarter))
```

A dot after a value is member access, not the start of a float: in `pair.1.0` the `1` and `0` are
tuple field numbers, and in `16.le` the `le` is a member of `16`.

## Bytes literals

A bytes literal is text between double quotes or single quotes; the two are the same. Its value is
the `Bytes` of its UTF-8 encoding, after escapes are decoded. A bytes literal must end on the line
it starts.

```jello
// expect: ["double", "single", "it's"]
Std.string(["double", 'single', "it's"])
```

A backslash starts an escape:

| Escape | Byte or bytes |
|---|---|
| `\0` | 0 (NUL) |
| `\a` | 7 (bell) |
| `\b` | 8 (backspace) |
| `\t` | 9 (tab) |
| `\n` | 10 (newline) |
| `\v` | 11 (vertical tab) |
| `\f` | 12 (form feed) |
| `\r` | 13 (carriage return) |
| `\"` `\'` `` \` `` `\\` | the character itself |
| `\xHH` | the byte with hex value `HH`; exactly two hex digits |
| `\uHHHH` | the UTF-8 encoding of the code point `HHHH`; exactly four hex digits |
| `\u{H...}` | the UTF-8 encoding of a code point of one to six hex digits |

A `\u` escape must name a Unicode scalar value: at most `10FFFF`, and not a surrogate (`D800` to
`DFFF`). A `\x` escape can give any byte, so a bytes literal need not hold valid UTF-8.

```jello
// expect: 9: 65 9 233 195 169 240 159 152 128
let s = "A\t\xe9é\u{1F600}";
let out = Std.string(s.len()) + ":";
let i = 0;
while (i < s.len()) { out += " " + Std.string(s[i]); i++; }
out
```

Any other character after a backslash is an error, located at the backslash:

```jello
// error: 2:12: unknown escape \q [E0004]
let s = "ok\q";
s
```

### Character codes

A bytes literal of exactly one Unicode character has the member `.code`, its code point as an
`I32` constant. It is an error for a literal of no character or of more than one.

```jello
// expect: [65, 10, 233]
Std.string(["A".code, "\n".code, "é".code])
```

## Templates

A template is text between backticks. It takes the same escapes as a bytes literal, plus `\$` for a
literal `$`, and it may span lines, keeping their line breaks. Each `${expression}` in it is a
hole: the expression's value, shown as [`Std.string`](02-types/01-scalars.md#showing-values) shows
it, takes the hole's place. A template's value is `Bytes`.

```jello
// expect: 3 + 4 = 7
// expect: names: ["ann", "bo"], not ${this}
let a = 3;
let b = 4;
let names = ["ann", "bo"];
`${a} + ${b} = ${a + b}
names: ${names}, not \${this}`
```

A hole holds one expression, which may contain braces, bytes literals and other templates. An empty
hole, `${}`, is an error. A backslash cannot be the last character of a line in a template.

## Atoms

An atom is a `:` followed by a name, such as `:ok` or `:not_found`. Atoms are interned names: two
atoms with the same name are the same value. The name may be a keyword, as in `:if`. See
[Atoms](02-types/01-scalars.md#atoms) for what can be done with them.

```jello
// expect: true false
let status = :ok;
Std.string(status == :ok) + " " + Std.string(status == :error)
```

## Regexp literals

A regexp literal is `~r/pattern/flags`. The pattern runs to the next `/` not escaped by a
backslash and keeps its escapes as written; it must end on the line it starts. The flags are the
letters after the closing `/`: `i`, `m`, `s`, `x` and `U`. The pattern is checked when the program
compiles. See [Regular expressions](04-expressions/05-regular-expressions.md).

```jello
// expect: true
import { matches } from std.core.RegExp;
Std.string(matches("Hello, World", ~r/world$/i))
```

## Operators and punctuation

The lexer always takes the longest operator it can, so `<<=` is one token, not `<<` and `<=`.

| Tokens | Use |
|---|---|
| `(` `)` `[` `]` `{` `}` | grouping, calls, tuples, arrays, indexing, blocks, objects |
| `,` `;` `:` `.` | separators, statement end, annotations, member access |
| `...` | spread and rest |
| `?` | nullable types `?T`, the conditional `c ? a : b`, optional parameters |
| `??` | null-coalescing, `value ?? fallback` |
| `+` `-` `*` `/` `%` | arithmetic |
| `++` `--` | increment and decrement |
| `==` `!=` `<` `<=` `>` `>=` | comparison |
| `&&` `\|\|` `!` | logical and, or, not |
| `&` `\|` `^` `<<` `>>` | bitwise and, or, exclusive or, shifts |
| `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | assignment |
| `->` | function types |
| `=>` | match arms |
| `<-` | generators and `with` clauses |
| `\|>` `%` | pipelines, and the piped value inside a pipeline's body |

`~` appears only at the start of a regexp literal; there is no bitwise-not operator. See
[Operators](04-expressions/01-operators.md) for precedence and meaning.

## Semicolons and newlines

A statement ends with `;`. Newlines are whitespace, so a statement may span lines and two
statements may share one, and a missing `;` is an error rather than being inferred:

```jello
// error: 3:1: ParseError: expected ';' or end of input [E0100]
let x = 1
let y = 2;
y
```

The last expression of a block or of a program, written without `;`, is that block's or that
program's value. A `;` after it makes it a statement, and the block then has no value, which is an
error where a value is needed.

```jello
// expect: 6
let triple = fn(n: I32): I32 { n * 3 };
triple(2)
```

A few forms need no `;`:

- An expression statement that starts with `if`, `match`, `with`, `try` or a block's `{` ends at
  its closing `}`. So `if (c) { ... }` on one line and `-1` on the next are two statements, not a
  subtraction.
- After a `while` or `for` loop, an `enum` declaration or a `macro` declaration, the `;` is
  optional.
- `break`, `continue`, `return` and `throw` may omit their `;` just before the `}` that closes their
  block, as in `{ next(); return value }`.

```jello
// expect: 10
let total = 0;
let i = 0;
while (i < 5) { total += i; i++; }
if (total > 100) { total = 100; }
enum Unit { One; }
Std.string(total)
```

Other declarations, `let`, `const`, `prototype`, `typedef`, `import` and `do ... while`, always end
with `;`.
