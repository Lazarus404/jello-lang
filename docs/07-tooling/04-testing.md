# Testing

Tests are functions in test files. The compiler collects them into a test binary, and the
standard library's `std.testing` runs them and reports the results, in the manner of Go's
`testing` package.

```text
// tests/app/Names.test.jello
package app;

import type { Test } from std.testing;
import std.testing as T;
import { _default, pick } from app.Names;

let testDefault = fn(t: Test) {
  T.equal(t, pick(List.nil<Bytes>()), _default);
  null
};

let testFirstArgument = fn(t: Test) {
  T.equal(t, pick(list["Ann"]), "Ann");
  null
};
```

```sh
$ jelloc test
=== RUN   app/Names/testDefault
=== RUN   app/Names/testFirstArgument
ok
```

## Test files

A test file is named `NAME.test.jello`. It is a module like any other, except in three ways:

- Its import path comes from the innermost source root it is under: `tests/app/Names.test.jello`
  under the root `tests` is `app.Names.test`. So it declares `package app;`, as `app.Names` does.
- When it imports the module it tests, the one whose path is its own less `.test` (here
  `app.Names`), it may import that module's private names as well as its exports. See
  [Modules](../05-modules.md#test-files-and-private-names).
- Its top-level names that start with `test` and then an upper-case letter are tests, and
  `beforeAll` and `afterAll` are hooks. Their signatures are checked.

A test takes the running test, of type `Test`, and returns `Null`:

```text
let testParsesNumbers = fn(t: Test) {
  T.equal(t, parse("42"), 42);
  null
};
```

Any other signature is an error (E0210), such as a test that takes something else or returns a
value:

```text
Names.test.jello:5:1: wrong signature for testReturnsAValue, must be: let testReturnsAValue = fn(t: Test) { ... } [E0210]
```

`testing`, `tester` and `test_case` are not tests: only `test` followed by an upper-case letter
names one.

`beforeAll` and `afterAll` take nothing and return `Null`. `beforeAll` runs before the file's
tests; if it throws, the file is reported as failed and its tests do not run. `afterAll` runs
after them, whether or not they passed. Neither runs when no test of the file is selected.

A test file with no tests compiles and runs nothing. Tests may also be made by a macro; the
compiler looks for them after expansion.

## Assertions

`std.testing` has five assertions. The compiler expands each call in place, so they take values
of any type that `==` compares, evaluate each argument once, in order, and report the place of
the call. A passing assertion builds no strings.

| Assertion | Passes when |
|-----------|-------------|
| `T.equal(t, got, want)` | `got == want` |
| `T.notEqual(t, got, want)` | `got != want` |
| `T.isTrue(t, condition)` | the `Bool` condition holds |
| `T.isNull(t, value)` | the nullable value is `null` |
| `T.throws(t, fn() { ... })` | the function, of type `() -> Null`, throws |

A failing assertion fails the test, which goes on to its end. The failure shows the place and the
values, as `Std.string` shows them, with `Bytes` quoted:

```text
--- FAIL: Shapes/testValues (tests/Shapes.test.jello:8)
    tests/Shapes.test.jello:9:3: got 2, want 3
    tests/Shapes.test.jello:10:3: got "ab", want "ba"
    tests/Shapes.test.jello:11:3: got Shape.Circle(1), want Shape.Dot
    tests/Shapes.test.jello:12:3: got (1, "one"), want (1, "uno")
    tests/Shapes.test.jello:13:3: got "same", want any other value
    tests/Shapes.test.jello:14:3: got false, want true
    tests/Shapes.test.jello:16:3: got 4, want null
    tests/Shapes.test.jello:17:3: did not throw
```

The assertions can be imported by name too, `import { equal } from std.testing;`. Since the
compiler expands them, they can only be called: assigning one, or using it as a value, is an
error. Their arguments are checked as the table says; `T.equal(t, 1, "one")` is an error, since
`==` does not compare `I32` with `Bytes`, and `T.isNull(t, "here")` is one, since `Bytes` is not
nullable.

## Failing, skipping and logging

| Function | Does |
|----------|------|
| `T.fail(t, message)` | Marks the test failed and logs `message`; the test goes on. |
| `T.fatal(t, message)` | Marks the test failed, logs `message` and stops it. |
| `T.skip(t, message)` | Logs `message` and stops the test, which counts as passed and shows as `SKIP`. |
| `T.log(t, message)` | Logs `message`, shown when the test fails or under `-v`. |
| `T.name(t)` | The test's full name. |

A test that throws, and does not catch it, fails with `threw` and the thrown value; the next test
still runs. `fatal` and `skip` stop the test by throwing; a test that catches that throw is
stopped all the same.

A trap is not caught: it ends the whole test binary, after `=== RUN` has named the test that
trapped.

## Subtests

`T.run(t, name, fn(t: Test) { ... })` runs a subtest named `name` under the test `t`, and
returns whether it passed. Its full name is the parent's, a slash and `name`; a failed subtest
fails its parent.

```text
let testParse = fn(t: Test) {
  let cases = [("1", 1), ("-2", -2), ("0x10", 16)];
  let i = 0;
  while (i < cases.len()) {
    let (text, want) = cases[i];
    T.run(t, text, fn(t: Test) { T.equal(t, parse(text), want); null });
    i++;
  }
  null
};
```

`T.selected(t, name)` says whether `--run` selects the subtest `name` of `t`, so that a test can
skip setting up a subtest that would not run.

## Options for tests

A test file, or a library it uses, can declare options of its own, which the test binary takes
after a second `--`. They are declared at the top level, so that they exist before the tests run,
and read in the tests:

```text
T.declareFlag("--slow", "also run the slow tests");
T.declareOption("--seed", "the random seed");

let testSlow = fn(t: Test) {
  if (!T.flag("--slow")) { T.skip(t, "pass --slow to run it"); }
  let seed = T.option("--seed", "1");
  null
};
```

- `T.declareFlag(name, help)` declares an option without a value; `T.flag(name)` says whether it
  was given.
- `T.declareOption(name, help)` declares one that takes a value; `T.option(name, fallback)` gives
  its last value, or `fallback`.
- Declaring a name twice, or reading one that was never declared, or reading one before the tests
  run, throws.
- An unknown option, or one missing its value, prints the usage, with the declared options and
  their help, and exits 2.

## Running tests

In a project, `jelloc test` compiles every test file under the project's test directories, by
default `tests`, or under the directories given, and runs the binary (see
[Projects](03-projects.md#running-tests)). Arguments for the binary follow `--`:

```sh
jelloc test
jelloc test -- -v
jelloc test tests/app -- --run app/Names/testDefault
jelloc test -- -v -- --slow --seed 7
```

`jelloc --test` compiles test files into a binary without running it. Each file must be under one
of the `--path` roots, which give the test files their import paths; for a test binary, these
roots alone are searched, not the files' own directories.

```sh
jelloc --test tests/app/Names.test.jello --path tests --path src --out build/test.jlo
jello build/test.jlo -v
```

The binary takes:

| Argument | Meaning |
|----------|---------|
| `-v` | Show every test's result and log, not only failures'. |
| `--run prefix` | Run only the tests and subtests whose full names start with `prefix`, and the tests that hold them. |
| `--list` | Print the names of the tests `--run` selects, and run none. |
| `-- options...` | The tests' own options. |

## Output and exit status

Each test's name is its file's import path with slashes, less `.test`, then a slash and the
test's name: `app/Names/testDefault`. The binary prints `=== RUN` and the name as each test or
subtest starts. A failed test, or every test under `-v`, gets a result line, with where the test
is declared, and then its log, indented. With `-v`, for the file of the options example above
and a test that fails:

```text
=== RUN   Misc/testSlow
--- SKIP: Misc/testSlow (tests/Misc.test.jello:7)
    pass --slow to run it
=== RUN   Misc/testFails
--- FAIL: Misc/testFails (tests/Misc.test.jello:13)
    about to check
    tests/Misc.test.jello:15:3: got 2, want 3
    stop here
FAIL
```

The last line is `ok` when every test and hook passed, and the binary exits 0; otherwise it is
`FAIL` and the binary exits 1. A bad command line exits 2, and a trap exits 1 as any program's
does.
