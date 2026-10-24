# std.testing

`std.testing` is the test harness that `jelloc --test` builds on. A test is a top-level
`let testName = fn(t: Test) { ... }` in a file whose name ends in `.test.jello`; the compiler finds
every test and passes them to the harness. How to build and run tests, and the runner's command
line, are in [testing](../07-tooling/04-testing.md).

```text
import type { Test } from std.testing;
import std.testing as T;

let testSquares = fn(t: Test) {
  T.equal(t, 3 * 3, 9);
  T.isTrue(t, 2 < 3);
  T.throws(t, fn() { throw "no"; });
};
```

## Test

`Test` is the running test, which every function below takes first. Its name is its file's path
under the `--path` root, less `.test.jello`, then the test function's name and any subtests'
names, joined by slashes, as in `Arith/testTable/2`.

## Assertions

The compiler expands each call of an assertion where it is written. So an assertion takes values
of any type `==` compares, evaluates each argument once, reports the line and column where it was
called, and builds no strings when it passes. A failing assertion marks the test failed and the
test goes on.

| Assertion | Passes when |
| --- | --- |
| `equal(t, got, want)` | `got == want` |
| `notEqual(t, got, want)` | `got != want` |
| `isTrue(t, condition)` | the `Bool` `condition` holds |
| `isNull(t, value)` | the nullable `value` is `null` |
| `throws(t, fn() { ... })` | the function throws |

A failure shows each value as a literal, as `Std.string` prints a tuple or an enum: Bytes quoted,
`Shape.Circle(1)`, `(1, "one")`, `[1, 2]`, `null`.

```text
--- FAIL: Arith/testLists (/home/me/project/tests/Arith.test.jello:23)
    /home/me/project/tests/Arith.test.jello:25:3: got [1, 2], want [1, 3]
```

## Reporting and control

| Function | Description |
| --- | --- |
| `name(t: Test): Bytes` | the test's full name |
| `log(t: Test, message: Bytes)` | records `message`, shown when the test fails or under `-v` |
| `fail(t: Test, message: Bytes)` | marks the test failed, saying `message`; the test goes on |
| `fatal(t: Test, message: Bytes)` | marks the test failed, saying `message`, and stops it |
| `skip(t: Test, message: Bytes)` | stops the test, saying why; a skipped test passes |
| `run(t: Test, subtest: Bytes, body: (Test) -> Null): Bool` | runs `body` as the subtest `subtest`; whether it passed. A failed subtest fails `t` |
| `selected(t: Test, subtest: Bytes): Bool` | whether `--run` selects the subtest, so a library can skip work for one that would not run |

A throw that escapes a test fails it, and the next test still runs. `fatal` and `skip` stop the
test by throwing; a test that catches that throw is still stopped.

```text
let testTable = fn(t: Test) {
  let cases = [(2, 4), (3, 9)];
  let i = 0;
  while (i < cases.len()) {
    let (n, want) = cases[i];
    T.run(t, Std.string(n), fn(sub: Test) { T.equal(sub, n * n, want); });
    i++;
  }
};
```

## Hooks

A test file may declare `let beforeAll = fn() { ... };` and `let afterAll = fn() { ... };`, which
run around the file's selected tests, if it has any. Its tests run only when `beforeAll` returns,
and `afterAll` runs either way. A hook that throws fails the file.

## Options

A test file, or a library it uses, can take options of its own, which follow `--` on the test
runner's command line, as in `tests.jlo -v -- --update --seed 7`. Each is declared as its module
loads, at the top level, and read while the tests run.

| Function | Description |
| --- | --- |
| `declareFlag(name: Bytes, help: Bytes)` | declares the option `name`, which takes no value; `help` is shown in the usage |
| `declareOption(name: Bytes, help: Bytes)` | declares the option `name`, which takes a value |
| `flag(name: Bytes): Bool` | whether the flag `name` was given |
| `option(name: Bytes, fallback: Bytes): Bytes` | the value given for `name`, the last if given more than once, or `fallback` |

Declaring a name twice throws, as does reading an undeclared option, reading a flag as an option
or the other way round, or reading one before the tests run. An undeclared option on the command
line prints the usage, with the declared options, and exits with status 2.

```text
T.declareFlag("--update", "rewrite the expected files");
T.declareOption("--seed", "the random seed");

let testOptions = fn(t: Test) {
  let seed = T.option("--seed", "1");
  T.log(t, "update " + Std.string(T.flag("--update")) + ", seed " + seed);
};
```

## The compiler's exports

`main`, `throwsAt` and the reporters `failEqual`, `failNotEqual`, `failNotNull` and `failFalse`
are also exported. The compiler's generated `main` and the expanded assertions call them, and a
test does not.
