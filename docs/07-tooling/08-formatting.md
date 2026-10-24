# Formatting

`jelloc fmt` lays out source files in one house style. Like Elixir's `mix format`, it reads what to
format, and how, from a `.formatter` file in the project.

```sh
$ jelloc fmt                    # the inputs of the nearest .formatter
$ jelloc fmt src/app tests      # these files and directories instead
$ jelloc fmt --check            # list the files that would change; change none
```

## Usage

```text
jelloc fmt [--check] [--strip-comments] [path...]
```

| Option | Does |
|--------|------|
| `path` | A file, formatted whatever its name, or a directory, whose files with one of the `extensions` are formatted, but those `exclude` names. With none, the `.formatter`'s `inputs`. |
| `--check` | Writes nothing; prints the files that would change, and exits with 1 if there are any. |
| `--strip-comments` | Removes every comment, as `strip_comments` does, doc comments too unless `keep_doc_comments` keeps them; but none from the files the `.formatter`, or an override, gives `strip_comments: false`. |

`jelloc fmt` looks for `.formatter` in the current directory and then in each directory above it.
Outside a project with one, it needs paths, and formats them in the default style. Each file is
formatted once, however many paths lead to it.

A file that does not lex is reported and left as it is, and `fmt` exits with 1. Newlines and spaces
are only whitespace to the lexer, so laying a file out never changes what it means: the result is
lexed again and must give the same tokens as before, or the file is left as it is and `fmt` exits
with 3, since that is a bug in the formatter. Formatting a formatted file changes nothing.

## The `.formatter` file

A `.formatter` is a JSON object. Paths are relative to the directory that holds it.

```text
{
  "inputs": ["src", "tests"],
  "exclude": ["tests/fixtures"],
  "extensions": [".jello"],
  "indent": 2,
  "line_width": 100,
  "keep_doc_comments": false,
  "rewrite_comments": true,
  "blank_before_one_line_blocks": false,
  "overrides": [
    { "paths": ["tests"], "rewrite_comments": false, "strip_comments": false },
    { "extensions": [".test.jello"], "line_width": 120 }
  ]
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `inputs` | none | The files and directories `jelloc fmt` formats when given no paths. |
| `exclude` | none | Files and directories under the inputs to leave alone, such as fixtures whose line and column numbers tests check. |
| `extensions` | `[".jello"]` | The endings of the files a directory gives. |
| `indent` | `2` | The spaces of one level of indentation. |
| `line_width` | `100` | The code points a comment-only line may take, its indentation included. |
| `strip_comments` | `false` | Remove every comment. Set to `false`, at the top or in an override, it keeps `--strip-comments` from those files. |
| `keep_doc_comments` | `false` | With `strip_comments`, keep doc comments, those that open with `/**`. |
| `rewrite_comments` | `true` | Move, join and wrap comments as below. When `false`, each comment's text is kept byte for byte, and only a comment line's indentation changes; `strip_comments` still removes comments. |
| `blank_before_one_line_blocks` | `false` | Put a blank line above a block or function that fits on one line too. |
| `overrides` | none | An array of objects, each with `paths` or `extensions`, or both, arrays of strings, and any of the settings from `indent` on. They apply, in order, to the files under their paths, by default those under the `.formatter`'s directory, whose names end with one of their extensions, by default any, so the last that applies wins. |

Keys it does not know are ignored. A bad value is an error, [E0215](../08-errors/01-compile-errors.md#e0215-an-invalid-formatter),
located in the file.

`rewrite_comments: false` is for files whose comments are data, such as test fixtures whose
comment lines tell the test runner what they print; give them `strip_comments: false` too, so
that `--strip-comments` leaves them alone.

## The layout

The rules, in the order they apply:

- **Trailing whitespace** is removed, and the file ends with one newline.
- **Comments at the end of a line** move to a line of their own above it; after an opening
  brace, to the first line inside the block.
- **Statements** sharing a line get a line each. A block that holds one statement stays on its
  line, as `if (done) { return x; }`; a block that holds more is opened up, its statements on lines
  of their own between its braces. An enum's variants are not statements, and stay as written.
- **An opening brace** on a line of its own joins the line above, as `if (ready) {`, unless that
  line ends a statement or an item. `else` and `catch` join the closing brace above them.
- **Indentation** is one level per line that leaves brackets open, however many it opens. A line
  that starts by closing brackets is level with the line that opened them. A line that continues a
  statement in a block, such as one that starts with `+` or `&&`, is one level further in.
- **Blank lines**: two or more become one, and none follow an opening bracket or precede a closing
  one. A block or function, `if`, `match`, `while`, `for`, `do`, `try`, `with`, `prototype`, `enum`,
  `typedef`, `macro`, or a `let` or `const` of `fn`, `if`, `match`, `try` or `with`, that starts a
  statement and spans lines gets a blank line above it, or above the comments directly on top of
  it, unless it is the first thing in its block.
- **Comment runs**: a comment alone on one line is a `//` comment, even one written `/* ... */` or
  `/** ... */`. Two or more `//` or `#` comment lines in a row become one block comment, each line
  after the first starting with ` * `. A run whose text holds `*/` stays as it is, since a block
  comment would end there. A block comment of more than one line keeps its opening, `/*` or `/**`,
  and its lines are lined up behind ` * `. With `strip_comments` and `keep_doc_comments`, a
  one-line doc comment keeps its `/** */`, so that it survives the next run.
- **Comment width**: a comment line wider than `line_width` is wrapped at spaces. A word wider
  than the line keeps a line of its own, and text in a code fence, or indented, is not wrapped. A
  `#!` line at the top of a file is left alone.

Nothing inside a bytes literal, a regexp literal or a template, holes and all, is ever changed.

```text
// Before.
let total = 0; let rest = items;
while (!List.is_nil(rest))
{
    total = total + List.head(rest);   // add it
    rest = List.tail(rest);
}



// After.
let total = 0;
let rest = items;

while (!List.is_nil(rest)) {
  // add it
  total = total + List.head(rest);
  rest = List.tail(rest);
}
```
