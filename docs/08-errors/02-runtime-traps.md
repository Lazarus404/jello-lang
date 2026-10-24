# Runtime traps

A trap is a fatal error that the virtual machine detects while a program runs: dividing by zero,
indexing past the end of an array, using a `null` as a value, and so on. A trap stops the process
it happens in at once. Unlike a thrown value, a trap cannot be caught.

```jello
// trap: BOUNDS 3:1
let items = [10, 20, 30];
items[3]
```

## What a trap looks like

When the program's main process traps, `jello` writes a line to standard error, located at the
source position of the operation that trapped, and exits with status 1:

```text
main.jello:3:1: jello: trap BOUNDS in function 0 at pc 12
```

The location comes first, as in a compiler diagnostic, followed by the trap's name. The function
index and `pc` locate the failing instruction in the compiled module, which is mostly of use when
debugging the compiler or the VM.

When the trap happens inside a function, the lines after it name the calls that led there,
innermost first, so a trap inside the standard library still points at your code:

```text
std/concurrent/Process.jello:105:18: jello: trap DEADLOCK in function 5 at pc 0
  called from main.jello:4:41
  called from main.jello:6:1
```

A run of one call, as in recursion too deep for the stack, is a single line ending `, repeatedly`,
and the calls that began it follow. At most 16 lines are kept, the innermost ones. A small
function the compiler inlines into its caller has no frame of its own, so its call is not listed.

Output that the program wrote before the trap, such as text written to `/dev/stdout`, is kept. The
program's result is not printed, since it has none.

## Traps and throws

A `throw` and a trap both stop the normal flow of a program, but they are for different things:

| | Throw | Trap |
|---|---|---|
| Raised by | a `throw` statement, or a library function that throws | the VM, when an operation cannot go on |
| Caught by `try ... catch` | yes | never |
| Typical cause | an expected failure the caller may handle, such as text that does not parse | a bug, such as an index out of range |
| Uncaught in the main process | the program ends with the trap `UNCAUGHT_THROW` | the program ends with that trap |

A `catch` never sees a trap, even one raised directly inside its `try`:

```jello
// trap: BOUNDS 5:19
// The catch does not run: an index out of range is a trap, not a throw.
let items = [1, 2];
let fallback = 0;
let value = try { items[5] } catch { fallback };
Std.string(value)
```

When a failure is something the caller should be able to handle, test for it and throw:

```jello
// expect: -1 index 5 is out of range
let at = fn(items: Array<I32>, i: I32): I32 {
  if ((i < 0) || (i >= items.len())) { throw "index " + Std.string(i) + " is out of range"; }
  items[i]
};
let message = try { at([1, 2], 5); "found" } catch (e) { e as Bytes };
let value = try { at([1, 2], 5) } catch { -1 };
`${value} ${message}`
```

See [Exceptions](../03-statements/04-exceptions.md) for `throw`, `try` and `catch`.

## The traps

### ARITHMETIC

An integer was divided by zero, with `/` or `%`, or a float was converted with `as` to an integer
type that cannot hold it: its truncated value is out of the type's range, or it is NaN or an
infinity. Integer overflow does not trap; integer arithmetic wraps (see
[Scalars](../02-types/01-scalars.md#arithmetic)).

```jello
// trap: ARITHMETIC 2:39
let ratio = fn(a: I32, b: I32): I32 { a / b };
ratio(1, 0)
```

```jello
// trap: ARITHMETIC 3:13
let nan = 0.0 / 0.0;
let count = nan as I32;
Std.string(count)
```

### BOUNDS

An index or range was outside the value it was used on, or a byte value was outside 0 to 255:

- an array index, read or written, that is negative or not less than the length;
- `pop` on an empty array;
- a `Bytes` index, or a `slice` that does not lie within the bytes;
- `Tuple.get` with an index the tuple does not have;
- `Bytes.new` with a negative length, or a byte outside 0 to 255 stored in a buffer.

```jello
// trap: BOUNDS 3:1
let text = "abc";
text.slice(2, 5)
```

### NULL_DEREF

A `null` was used where a value is required: a nullable value used as its inner type, whether
implicitly or with `as T`, or a `Dynamic` holding `null` extracted with `as T`. See
[Nullability and Dynamic](../02-types/02-nullability-and-dynamic.md#using-a-nullable-value).

```jello
// trap: NULL_DEREF 4:7
let name: ?Bytes = null;
let greet = fn(who: Bytes): Bytes { "hello " + who };
greet(name)
```

### TYPE_MISMATCH

A value had the wrong kind for an operation that checks it at run time:

- `d as T` on a `Dynamic` that holds a value of another kind;
- reading an `Object`'s field as a type it does not hold, or calling a field that holds a function
  of another type;
- `as P` on an object that does not delegate to the prototype `P`;
- a native function that rejected its arguments, such as a regexp group that does not exist.

When a native function is to blame, a line after the trap's says which, and why:
`jello: native regexp_capture_named failed: no group of that name`.

```jello
// trap: TYPE_MISMATCH 3:14
let value = "seven" as Dynamic;
let number = (value as I32) + 1;
Std.string(number)
```

### STACK_OVERFLOW

The call stack is full, almost always because of recursion that does not end, or recursion deeper
than the VM's stack allows: a million calls deep, in the main program or in any process, with up
to 1 GiB of registers beyond what the stack starts with. Calls in tail position of a function to
itself reuse its frame, so they do not grow the stack.

```jello
// trap: STACK_OVERFLOW
let forever = fn(n: I32): I32 { 1 + forever(n + 1) };
forever(0)
```

### UNCAUGHT_THROW

A value was thrown, and no `try` around it caught it. `jello` reports the trap where the value
was thrown, and then, on a second line, the value; a thrown `Bytes` is shown as its text:

```jello
// trap: UNCAUGHT_THROW 3:18
let check = fn(age: I32): I32 {
  if (age < 0) { throw "negative age"; }
  age
};
check(-1)
```

```text
main.jello:3:18: jello: trap UNCAUGHT_THROW in function 0 at pc 5
jello: thrown: negative age
```

A failed `System.assert` or `System.assertEq` throws a `Bytes` message naming its location, such as
`main.jello:1:1: assertEq failed: 2 != 3`, so, uncaught, it ends the program this way too.

### DEADLOCK

The program waited for something that can never happen: a receive or a join in the main process
when no other process can run and no timer is pending. The VM traps rather than hanging. For
example, a program that receives from an inbox no process can ever send to:

```text
import type { Inbox } from std.concurrent.Process;
let inbox = new Inbox<I32>();
inbox.receive()
```

See [the standard library](../06-stdlib/README.md) for processes and inboxes.

### STOPPED

The process was stopped by another process, with `stop` on its handle. A stopped process traps
`STOPPED` where it next runs, so no `catch` in it sees the stop, and its exit is reported to those
waiting for it as `Exit.Stopped` rather than as a crash.

### UNSENDABLE

A value leaving its process, in a message, a reply or the function given to a spawn, held a
handle from a native library that cannot be shared between processes.

### SEALED

A write through an `Object` or `Dynamic` would have changed a method that a prototype with a linked
typedef fixes: replacing or removing it, giving an Object that delegates to it a field of the same
name, or relinking such an Object's `__proto__`. See [linked
typedefs](../04-expressions/03-objects-and-prototypes.md#linked-typedefs).

```text
typedef Shape = { let w: I32; };
prototype Shape { area(): I32 { this.w * this.w }; };
let o = { __proto__: Shape, area: 1 };
```

### FUEL and LIMIT

A program embedded in another application can be given limits by its host. `FUEL` means the
program used up its budget of work: each loop iteration, call and throw spends a unit. `LIMIT`
means the program asked `Bytes.new` for more bytes than the host allows. `jello` sets neither
limit, so programs it runs never hit these traps.

## Traps in processes

A trap ends only the process it happens in. In a process started with `Process.spawn`, the trap
does not end the program: the process finishes, and `join` on it, or a monitor, reports
`Exit.Crashed(null)`. A process that throws a value no one catches also crashes, with
`Exit.Crashed(value)`.

```jello
// expect: worker trapped
import std.concurrent.Process as Process;
import type { Inbox } from std.concurrent.Process;
let worker = Process.spawn<I32>(fn(_: Inbox<I32>) {
  let items = [1, 2];
  items[5];
});
let how = match (worker.join()) {
  Normal => "finished",
  Stopped => "stopped",
  Crashed(value) => value == null ? "trapped" : "threw",
  ParentExited => "orphaned",
};
"worker " + how
```

A trap in the main process ends the whole program. In a test run with `std.testing`, a trap ends
the run, after the test that trapped has been named.

## Exit statuses

| Status | When |
|---|---|
| 0 | The program finished normally. |
| 1 | The program trapped, including an uncaught throw; or `jello` could not read or load the module. |
| 2 | `jello` was given a bad command line. |
| *n* | The program called `System.exit(n)`; the status is `n` modulo 256. |

`jelloc` exits 1 when the program does not compile, and 2 on a bad command line. See
[jelloc](../07-tooling/01-jelloc.md) and [jello](../07-tooling/02-jello.md).

```jello
// expect: bye
// status: 3
import { writeText } from std.io.File;
writeText("/dev/stdout", "bye\n");
System.exit(3);
"not reached"
```
