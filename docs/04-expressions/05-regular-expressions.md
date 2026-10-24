# Regular expressions

A regular expression literal, `~r/pattern/flags`, is a compiled regexp. It is used as a value with
the functions of `std.core.RegExp`, or as a pattern that matches `Bytes` in `match`, `if (let ...)`
and the other places patterns go. Regexps are PCRE2 regular expressions over UTF-8 text.

```jello
// expect: 2024 10
import { find, group } from std.core.RegExp;
let date = find("due 2024-10", ~r/(\d+)-(\d+)/);
group(date, 1) + " " + group(date, 2)
```

## Literals

A literal starts with `~r/` and ends at the next `/` that is not escaped with a backslash; the
letters straight after it are its flags. The pattern keeps its backslashes, so `\d` and `\/` reach
the regexp engine as written, and `\/` matches a `/`. An unescaped
line break before the closing `/` is an error.

| Flag | Meaning |
| --- | --- |
| `i` | case-insensitive matching |
| `m` | multi-line: `^` and `$` match at the start and end of each line |
| `s` | `.` matches a newline too |
| `x` | extended: whitespace in the pattern is ignored, and `#` starts a comment |
| `U` | ungreedy: quantifiers match as little as they can, and `?` after one makes it greedy |

```jello
// expect: true true a
import { find, group, matches } from std.core.RegExp;
Std.string(matches("a/b", ~r/a\/b/)) + " " + Std.string(matches("HeLLo", ~r/hello/i)) + " " +
  group(find("aaa", ~r/a+/U), 0)
```

### Compile-time checking

The compiler checks every literal's pattern and flags, with the same regexp engine the program runs
with, wherever the literal is, even in code that never runs. An invalid pattern or an unknown flag
is an error.

```jello
// error: 2:9: invalid regexp pattern: missing closing parenthesis, at byte 2 [E0809]
let r = ~r/a(/;
1
```

```jello
// error: 2:9: unknown regexp flag g; the flags are i, m, s, x and U [E0809]
let r = ~r/a/g;
1
```

There is no `g` flag: `replace` replaces every match, and `find` and `findAt` find one.

## Regexp values

A literal is a value of type `Dynamic`, holding a handle to the compiled regexp. Each literal is
compiled once, however often it is evaluated, so a literal in a loop or a function costs nothing
after the first time. A regexp value can be stored, passed and exported like any other `Dynamic`.

`compile(pattern, flags)` from `std.core.RegExp` makes a regexp from `Bytes` at run time. It accepts
the same patterns and flags as a literal, and for an invalid one it throws what is wrong with it,
instead of the compile error a literal gives.

```jello
// expect: RegExp.compile: invalid regexp pattern: missing closing parenthesis, at byte 2
import { compile } from std.core.RegExp;
try { compile("a(", ""); "compiled" } catch (e) { e as Bytes }
```

To match text literally, make the pattern with `escapeLiteral(text)`, which escapes every character
that has a meaning in a pattern.

## Matching with std.core.RegExp

The functions of `std.core.RegExp` take the subject `Bytes` first and the regexp second. A match is
a `Dynamic` handle, and a failed match is `null`.

| Function | Result |
| --- | --- |
| `matches(s, re)` | whether `re` matches anywhere in `s` |
| `find(s, re)` | the first match anywhere in `s`, or null |
| `matchPrefix(s, re)` | the match at the start of `s`, or null |
| `fullMatch(s, re)` | whether `re` matches the whole of `s` |
| `replace(s, re, with)` | `s` with every match replaced by `with` |
| `split(s, re)` | the parts of `s` between the matches, as JavaScript's `split` gives them |
| `escapeLiteral(text)` | a pattern that matches `text` itself |
| `source(re)`, `flags(re)` | the pattern and flags `re` was made with |

```jello
// expect: true false true a|b|c a-b-c
import { fullMatch, matchPrefix, replace, split } from std.core.RegExp;
let digits = ~r/[0-9]+/;
Bytes.join([Std.string(fullMatch("123", digits)), Std.string(fullMatch("123x", digits)),
  Std.string(matchPrefix("12ab", digits) != null), Bytes.join(split("a1b22c", digits), "|"),
  replace("a1b22c", digits, "-")], " ")
```

In the replacement text of `replace`, `$0` to `$99` stand for the match's numbered captures,
`${name}` for a named one, and `$$` for a `$`. An unset capture is empty, and an unknown one traps.

```jello
// expect: [1]b[2] 10/2024 $
import { replace } from std.core.RegExp;
replace("a1b2", ~r/a?([0-9])/, "[$1]") + " " + replace("2024-10", ~r/(?<y>\d+)-(?<m>\d+)/, "${m}/${y} $$")
```

### Captures

A match gives its captures by number, from 0 for the whole match, or by name for a group written
`(?<name>...)`.

| Function | Result |
| --- | --- |
| `group(m, i)` | the text of capture `i`; empty if the group did not take part |
| `named(m, name)` | the text of the named capture; empty if unset, and a trap if the regexp has no such name |
| `start(m, i)` | the byte offset in the subject where capture `i` starts |
| `len(m)` | the length in bytes of the whole match |
| `captureCount(m)` | the number of captures, counting capture 0 |

```jello
// expect: hi ! 4 3 3 []
import { captureCount, find, group, len, named, start } from std.core.RegExp;
let m = find("say hi!", ~r/(?<word>[a-z]+)(!)$/);
Bytes.join([named(m, "word"), group(m, 2), Std.string(start(m, 0)), Std.string(len(m)),
  Std.string(captureCount(m)), "[" + group(find("ac", ~r/a(b)?c/), 1) + "]"], " ")
```

Offsets and lengths count bytes, not characters, and match the indexes of `Bytes.slice`.

### Scanning a text

`text(s)` checks `s` once for valid UTF-8 and gives a text that `matchAt(text, re, at)` and
`findAt(text, re, at)` match from a byte position without checking the subject again. A scanner that
steps through a long text with them pays for what it matches, not for the rest of the text.

## Regexp patterns

A regexp literal is also a pattern. It matches a `Bytes` subject, or a nullable one that holds
`Bytes`, when the regexp matches at the subject's start; it need not reach the end. `as name` binds
the match, for the capture functions above. The pattern can be written `when ~r/.../`, with the
same meaning.

```jello
// expect: 42 not at start
import { group } from std.core.RegExp;
let size = fn(s: Bytes): Bytes {
  match (s) {
    ~r/(\d+)px/ as m => group(m, 1),
    _ => "not at start",
  }
};
size("42px wide") + " " + size("width 42px")
```

To match anywhere, start the pattern with `.*?` (adding the `s` flag if the subject has newlines);
to require the whole subject, end it with `$`.

A regexp pattern works wherever a pattern can fail: a match arm, `if (let ...)`, `while (let ...)`,
`let ... else`, a `with` clause, a list comprehension's generator, and inside a tuple or other
pattern. A tokenizer, for example, can repeatedly match a prefix and step past it with `advance`:

```jello
// expect: ab|12|cd|
import { advance, group } from std.core.RegExp;
let rest = "ab 12  cd";
let tokens = "";
while (let ~r/\s*(\w+)/ as m = rest) {
  tokens = tokens + group(m, 1) + "|";
  rest = advance(rest, m);
}
tokens
```

A regexp can fail for any subject, so a plain `let` cannot use one; and the subject must be `Bytes`.

```jello
// error: 2:5: this pattern can fail: use `if (let ...)` or `let ... else` [E0702]
let ~r/a/ as m = "a";
1
```

```jello
// error: 2:6: a regexp pattern cannot match I32 [E0703]
with ~r/a/ <- 1 { 1 }
```

See [Patterns](../03-statements/03-patterns.md) for how patterns combine.
