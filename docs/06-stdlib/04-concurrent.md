# Processes, jobs and supervisors

Jello runs concurrent code in *processes*: lightweight threads of Jello code, each with its own
heap and a typed *inbox*. Processes share no mutable values; they talk by sending messages, which
behave as copies. The `std.concurrent` package holds the whole interface, as a library:

| Module | Covers |
| --- | --- |
| `std.concurrent.Process` | spawning, messages, replies, timers, stopping, joining, scopes and cleanups |
| `std.concurrent.Job` | fork/join work: a value computed in a process, and parallel loops |
| `std.concurrent.Buffer` | shared, mutable, fixed-length memory |
| `std.concurrent.Supervisor` | processes that restart their children as they finish |

Each module exports both functions and types, so a program imports it both ways: as a namespace
for the functions, and with `import type` for the types, which may have the same name.

```text
import std.concurrent.Process as Process;
import type { Process, Inbox, Reply, Exit } from std.concurrent.Process;
```

## The model

- **Processes are lightweight.** They run on a pool of scheduler threads, one per processor by
  default; the environment variable `JELLO_SCHEDULERS` sets how many. A process that loops without
  waiting does not starve the others.
- **Waiting parks only the caller.** Receiving, joining, sleeping, a call to another process, and
  file, command and socket I/O park the process that waits and let others run. No function has to
  be marked to be able to wait.
- **A message behaves as a copy.** After `send`, neither the sender nor the receiver sees the
  other's writes. Immutable Bytes are shared rather than copied, and a
  [`Buffer`](#stdconcurrentbuffer) is the one kind of memory that processes share and change.
- **An inbox is typed.** A process receives messages of one type, usually an enum, so receiving
  is an ordinary `match` checked for exhaustiveness. `Process<M>` can only be sent an `M`.
- **Globals are per process.** A new process starts with a copy of its spawner's globals as they
  are at the spawn; writes to them afterwards are its own.
- **Lifetimes are explicit.** A process is owned by the process that spawned it and is stopped
  when that one finishes, unless it was spawned detached.
- **Deadlock is detected.** A wait that no process or timer could ever end traps `DEADLOCK`
  rather than hanging.

Times are in milliseconds, measured on a monotonic clock, and are lower bounds: a process runs
again once it has waited that long and its turn comes.

```jello
// expect: 15 Exit.Normal
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;

enum Msg { Add(n: I32); Total(to: Process<I32>); Done; }

let adder = Process.spawn<Msg>(fn(inbox: Inbox<Msg>) {
  let total = 0;
  while (true) {
    match (inbox.receive()) {
      Add(n) => { total = total + n; },
      Total(to) => { to.send(total); },
      Done => { return; },
    }
  }
});

let replies = new Inbox<I32>();
adder.send(Msg.Add(10));
adder.send(Msg.Add(5));
adder.send(Msg.Total(Process.self<I32>(replies)));
let total = replies.receive();
adder.send(Msg.Done);
Std.string(total) + " " + Std.string(adder.join())
```

```jello
// expect: 101 1
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;

let count = 1;
let results = new Inbox<I32>();
let main = Process.self<I32>(results);
Process.spawn<I32>(fn(_: Inbox<I32>) { count = count + 100; main.send(count); });
let seen = results.receive();
Std.string(seen) + " " + Std.string(count)
```

## std.concurrent.Process

### Spawning

| Function | Description |
| --- | --- |
| `spawn<M>(body: (Inbox<M>) -> Void): Process<M>` | starts a process running `body` with its inbox, owned by the caller |
| `spawnDetached<M>(body: (Inbox<M>) -> Void): Process<M>` | the same, but owned by no process; it runs until the program ends |
| `spawnIn<M>(scope: Scope, body: (Inbox<M>) -> Void): Process<M>` | the same as `spawn`, and also stopped when `scope`'s block ends; see [scopes](#scopes) |
| `spawnOnMain<M>(body: (Inbox<M>) -> Void): Process<M>` | the same as `spawn`, but it only ever runs on the program's main thread |
| `onMain(): Bool` | whether the caller is running on the main thread |
| `self<M>(inbox: Inbox<M>): Process<M>` | the process whose inbox is `inbox`, as a handle others can send to |
| `yield()` | lets every process waiting to run run first |
| `sleep(ms: I32)` | parks for `ms` |
| `now(): I32` | the milliseconds since processes were first used; it wraps after 24 days |

A new process runs after those already waiting to run. Its body is copied into it as a message
is, so what the body captures is a copy too. `spawnOnMain` is for libraries that must be called on
the main thread, such as a window system's; such a process runs while the main process waits.

### Inboxes

`Inbox<M>` is the inbox of the process that makes it, with `new Inbox<M>()`. A process has one
inbox, so it makes one, of the one type it receives; a message of another type traps where it is
received. A body given to `spawn` receives its process's inbox.

| Method | Description |
| --- | --- |
| `receive(): M` | the next message, in the order they arrived, parking until one does |
| `tryReceive(): ?M` | the next message, or `null` when none has arrived |
| `receiveWithin(ms: I32): ?M` | the next message, parking at most `ms`; `null` when none arrives in time |
| `waiting(): I32` | how many messages have arrived and wait to be received |
| `sendAfter(ms: I32, message: M): Timer` | sends `message` to this inbox once `ms` have passed |

### Process handles

`Process<M>` is a handle to a process that receives messages of type `M`. Handles can be sent in
messages.

| Method | Description |
| --- | --- |
| `send(message: M): Bool` | sends a copy of `message`, never waiting; whether it went, which it does not once the process has finished |
| `stop(): Bool` | asks the process to stop where it next runs; whether it had yet to finish |
| `join(): Exit` | parks until the process has finished; how it did |
| `joinWithin(ms: I32): ?Exit` | the same, parking at most `ms`; `null` if it is still running |
| `exited(): ?Exit` | how the process finished, or `null` while it runs |

How a process finished is an `Exit`:

```text
export enum Exit { Normal; Stopped; Crashed(value: Dynamic); ParentExited; }
```

| Variant | Meaning |
| --- | --- |
| `Normal` | its body returned |
| `Stopped` | it was stopped |
| `Crashed(value)` | a throw escaped its body; `value` is a copy of what was thrown, `null` for a trap |
| `ParentExited` | the process that owned it finished |

To name a variant outside a `match`, as in `exit == Exit.Normal`, import the enum's values too:
`import { Exit } from std.concurrent.Process;`.

A crash ends only its own process. A stop takes effect when the process next runs, even if it is
parked or in a loop; it unwinds without running `catch` handlers, so a stop cannot be caught.

```jello
// expect: Exit.Crashed("boom") Exit.Stopped Exit.Normal false null
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;

let crasher = Process.spawn<I32>(fn(inbox: Inbox<I32>) { throw "boom"; });
let waiter = Process.spawn<I32>(fn(inbox: Inbox<I32>) {
  try { inbox.receive(); } catch (e) { inbox.receive(); }
  null
});
let quick = Process.spawn<I32>(fn(inbox: Inbox<I32>) { });
Process.yield();
waiter.stop();
let running = Process.spawn<I32>(fn(inbox: Inbox<I32>) { inbox.receive(); });
let still = running.exited();
running.send(1);
Std.string(crasher.join()) + " " + Std.string(waiter.join()) + " " + Std.string(quick.join())
  + " " + Std.string(quick.send(1)) + " " + Std.string(still)
```

### Calls and replies

A `Reply<T>` is a one-shot channel back to a process that asked for something. It travels inside
a message like any other value, and the asker parks until it is answered.

| Function or method | Description |
| --- | --- |
| `call<M, T>(p: Process<M>, make: (Reply<T>) -> M): T` | sends `p` the message `make` builds around a new reply, and parks until `p` answers |
| `callWithin<M, T>(p: Process<M>, ms: I32, make: (Reply<T>) -> M): ?T` | the same, parking at most `ms`; `null` when `p` has not answered in time |
| `Reply<T>.send(value: T): Bool` | answers the caller with a copy of `value`; whether it went, which it does only the first time |

`call` and `callWithin` throw `the process finished without replying` when `p` finishes first.

```jello
// expect: 1 2 3 null the process finished without replying
import std.concurrent.Process as Process;
import type { Process, Inbox, Reply } from std.concurrent.Process;

enum Ask { Next(reply: Reply<I32>); Ignore(reply: Reply<I32>); Quit(reply: Reply<I32>); }

let counter = Process.spawn<Ask>(fn(inbox: Inbox<Ask>) {
  let n = 0;
  while (true) {
    match (inbox.receive()) {
      Next(reply) => { n++; reply.send(n); },
      Ignore(_) => {},
      Quit(_) => { return; },
    }
  }
});

let next = fn(): I32 { Process.call<Ask, I32>(counter, fn(r: Reply<I32>): Ask { Ask.Next(r) }) };
let got = [next(), next(), next()];
let ignored = Process.callWithin<Ask, I32>(counter, 20, fn(r: Reply<I32>): Ask { Ask.Ignore(r) });
let failure = try {
  Process.call<Ask, I32>(counter, fn(r: Reply<I32>): Ask { Ask.Quit(r) });
  ""
} catch (e) { e as Bytes };
Std.string(got[0]) + " " + Std.string(got[1]) + " " + Std.string(got[2]) + " "
  + Std.string(ignored) + " " + failure
```

### Timers

`inbox.sendAfter(ms, message)` sends to the caller's own inbox once `ms` have passed, and returns
a `Timer`. A process's pending timers are cancelled when it finishes.

| Method | Description |
| --- | --- |
| `Timer.cancel(): Bool` | stops the message being sent; whether it had yet to be |

```jello
// expect: early late true false null
import std.concurrent.Process as Process;
import type { Inbox } from std.concurrent.Process;

let inbox = new Inbox<Bytes>();
let late = inbox.sendAfter(30, "late");
let early = inbox.sendAfter(5, "early");
let never = inbox.sendAfter(10, "never");
let cancelled = never.cancel();
let first = inbox.receive();
let second = inbox.receive();
first + " " + second + " " + Std.string(cancelled) + " " + Std.string(late.cancel()) + " "
  + Std.string(inbox.receiveWithin(30))
```

### Monitors

| Function | Description |
| --- | --- |
| `monitor<M, N>(p: Process<M>, to: Process<N>, map: (Exit) -> N): Process<Exit>` | once `p` finishes, sends `to` the message `map` makes of how it did |

The watcher is a process owned by the caller, so it stops watching when the caller finishes.

```jello
// expect: worker: Exit.Crashed("lost")
import std.concurrent.Process as Process;
import type { Process, Inbox, Exit } from std.concurrent.Process;

let worker = Process.spawn<I32>(fn(inbox: Inbox<I32>) { inbox.receive(); throw "lost"; });
let notes = new Inbox<Bytes>();
Process.monitor<I32, Bytes>(worker, Process.self<Bytes>(notes),
  fn(e: Exit): Bytes { "worker: " + Std.string(e) });
worker.send(1);
notes.receive()
```

### Ownership

A process owns the processes it spawns. When it finishes, however it does, each of them still
running is stopped and finishes with `ParentExited`, and they stop the processes they own in turn.
A process from `spawnDetached` has no owner and runs on until the program ends. When the main
process finishes, its cleanups run, then every remaining process is stopped, and the program
exits.

```jello
// expect: Exit.ParentExited null
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;

enum Note { Started(owned: Process<I32>, detached: Process<I32>); }

let notes = new Inbox<Note>();
let main = Process.self<Note>(notes);
let wait = fn(inbox: Inbox<I32>) { inbox.receive(); };
Process.spawn<I32>(fn(_: Inbox<I32>) {
  main.send(Note.Started(Process.spawn<I32>(wait), Process.spawnDetached<I32>(wait)));
}).join();
match (notes.receive()) {
  Started(owned, detached) => Std.string(owned.join()) + " " + Std.string(detached.exited()),
}
```

### Scopes

| Function | Description |
| --- | --- |
| `scoped(body: (Scope) -> Void)` | runs `body` with a new scope, then stops every process spawned on it and parks until they have all finished |

`scoped` cleans up whether `body` returns or throws; a throw then goes on. If the process running
`body` is stopped instead, the scope's processes stop with it, as processes it owns. `spawnIn`
throws once the scope's block has ended. Call it from the block's own process: a copy of the scope
sent to another process is not the block's.

```jello
// expect: Exit.Stopped Exit.Normal boom Exit.Stopped
import std.concurrent.Process as Process;
import type { Process, Inbox, Scope } from std.concurrent.Process;

let seen = Array.new<Process<I32>>(0);
let wait = fn(inbox: Inbox<I32>) { inbox.receive(); };
Process.scoped(fn(scope: Scope) {
  seen.push(Process.spawnIn<I32>(scope, wait));
  let quick = Process.spawnIn<I32>(scope, fn(_: Inbox<I32>) { });
  quick.join();
  seen.push(quick);
});
let failure = try {
  Process.scoped(fn(scope: Scope) {
    seen.push(Process.spawnIn<I32>(scope, wait));
    throw "boom";
  });
  ""
} catch (e) { e as Bytes };
Std.string(seen[0].exited()) + " " + Std.string(seen[1].exited()) + " " + failure + " "
  + Std.string(seen[2].exited())
```

### Cleanups

| Function | Description |
| --- | --- |
| `onExit(cleanup: () -> Void)` | runs `cleanup` when the calling process finishes, however it does |

Cleanups run last registered first, before the processes the finishing one owns are stopped. A
stop waits until they are done. A park inside a cleanup ends that cleanup, a throw from one
changes nothing about how the process finished, and each has a budget of 100,000 reductions,
after which it is stopped. The main process's cleanups run when the program ends, before its
other processes are stopped.

```jello
// expect: second first | stopped Exit.Stopped
// expect: main done
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;
import { writeText } from std.io.File;

let notes = new Inbox<Bytes>();
let me = Process.self<Bytes>(notes);
Process.onExit(fn() { writeText("/dev/stdout", "main done\n"); });

Process.spawn<I32>(fn(_: Inbox<I32>) {
  let to = me;
  Process.onExit(fn() { to.send("first"); });
  Process.onExit(fn() { to.send("second"); });
}).join();
let order = notes.receive() + " " + notes.receive();

let parked = Process.spawn<I32>(fn(inbox: Inbox<I32>) {
  let to = me;
  Process.onExit(fn() { to.send("stopped"); });
  inbox.receive();
});
Process.yield();
parked.stop();
let exit = parked.join();
writeText("/dev/stdout", order + " | " + notes.receive() + " " + Std.string(exit) + "\n");
null
```

### Deadlock

A process that waits for a message no other process can send traps with `DEADLOCK`:

```text
import type { Inbox } from std.concurrent.Process;

let inbox = new Inbox<I32>();
inbox.receive();
"never printed"
```

## std.concurrent.Job

A job computes one value in a process of its own, owned by the caller; `parallelFor` spreads a
loop across the schedulers. The work runs in other processes, so what it captures is copied, as a
message is; large data reaches it through a [`Buffer`](#stdconcurrentbuffer), which is shared
rather than copied. A job's value is copied back, so it is of a type a reply carries: an enum, a
scalar, Bytes, an Atom or an Object.

```text
import std.concurrent.Job as Job;
import type { Job } from std.concurrent.Job;
```

| Function or method | Description |
| --- | --- |
| `start<T>(work: () -> T): Job<T>` | starts `work` in a process owned by the caller |
| `Job<T>.join(): T` | parks until the work has finished; its value, or what it threw, thrown again |
| `parallelFor(count: I32, grain: I32, body: (I32, I32) -> Void)` | calls `body(start, end)` for the chunks of `[0, count)`, each at most `grain` long, and returns once all have finished |

A job that trapped throws `the job trapped` from `join`. `parallelFor` runs at most one process
per scheduler; chunks run in no set order, each once. A body that throws stops its process taking
more chunks, and once every process has finished, the first failure, in the order the processes
started, is thrown again. Nothing runs when `count` is not positive, and a `grain` that is not
positive throws.

```jello
// expect: 42 boom the job trapped 332833500
import std.concurrent.Job as Job;
import type { Job } from std.concurrent.Job;
import type { Buffer } from std.concurrent.Buffer;

let answer = Job.start<I32>(fn(): I32 { 6 * 7 });
let thrower = Job.start<I32>(fn(): I32 { throw "boom"; 1 });
let thrown = try { thrower.join(); "" } catch (e) { e as Bytes };
let trapper = Job.start<I32>(fn(): I32 { [1][2] });
let trapped = try { trapper.join(); "" } catch (e) { e as Bytes };

let squares = new Buffer(4 * 1000);
Job.parallelFor(1000, 64, fn(first: I32, end: I32) {
  let i = first;
  while (i < end) { squares.setI32(i, i * i); i++; }
});
let sum = 0;
let i = 0;
while (i < 1000) { sum = sum + squares.getI32(i); i++; }
Std.string(answer.join()) + " " + thrown + " " + trapped + " " + Std.string(sum)
```

## std.concurrent.Buffer

A `Buffer` is shared, mutable, fixed-length memory. Sent in a message, captured by a process or a
job, or held in a global that a new process copies, it is the same memory there, not a copy. It
is made with `new` after a type import:

```text
import type { Buffer } from std.concurrent.Buffer;
```

| Method | Description |
| --- | --- |
| `init(bytes: I32)`, as `new Buffer(bytes)` | a buffer of `bytes` zero bytes; a negative length traps `BOUNDS` |
| `len(): I32` | its length in bytes |
| `getI8`, `getU8`, `getI16`, `getU16`, `getI32`, `getU32`, `getI64`, `getU64`, `getF32`, `getF64` `(i: I32)` | the element `i` of that type |
| `setI8`, `setU8`, `setI16`, `setU16`, `setI32`, `setU32`, `setI64`, `setU64`, `setF32`, `setF64` `(i: I32, v)` | writes `v`, of that type, to element `i` |
| `addI32(i: I32, v: I32): I32`, `addI64(i: I32, v: I64): I64` | adds `v` to element `i`, wrapping, in one atomic step; what it held before |
| `casI32(i: I32, old: I32, v: I32): Bool`, `casI64(i: I32, old: I64, v: I64): Bool` | sets element `i` to `v` if it holds `old`, in one atomic step; whether it did |
| `slice(start: I32, length: I32): Bytes` | a copy of `length` bytes from byte `start` |

An index counts elements of the access's own width, so `getF32(i)` reads bytes `4 * i` to
`4 * i + 3`. Every access is atomic: racing writes to one element leave one of the values written,
and a read gives one of them, never a mixture. An index outside the buffer traps `BOUNDS`. Values
are in the machine's byte order.

```jello
// expect: 64 250 -6 1.5 0 true 11 9
import type { Buffer } from std.concurrent.Buffer;

let b = new Buffer(64);
b.setU8(0, 250 as U8);
b.setF32(1, 1.5 as F32);
let old = b.addI32(9, 7);
let swapped = b.casI32(9, 7, 11);
b.setU8(1, 9 as U8);
let copied = b.slice(0, 2);
Std.string(b.len()) + " " + Std.string(b.getU8(0)) + " " + Std.string(b.getI8(0)) + " "
  + Std.string(b.getF32(1)) + " " + Std.string(old) + " " + Std.string(swapped)
  + " " + Std.string(b.getI32(9)) + " " + Std.string(copied.get(1))
```

## std.concurrent.Supervisor

A supervisor is a process that starts children and restarts them when they finish, as a restart
policy and a strategy say, and gives up once they restart too often.

```text
import std.concurrent.Supervisor as Supervisor;
import type { Supervisor, Child, Strategy, Restart } from std.concurrent.Supervisor;
import { Strategy, Restart } from std.concurrent.Supervisor;
```

| Export | Description |
| --- | --- |
| `enum Strategy { OneForOne; OneForAll; RestForOne; }` | which children restart with one that does: only it, all of them, or it and those started after it |
| `enum Restart { Permanent; Transient; Temporary; }` | when a finished child restarts: always, only after a crash, or never |
| `new Child(name: Bytes, run: () -> Void, restart: Restart)` | a child: its name, the function its process runs, and its policy |
| `start(strategy: Strategy, children: Array<Child>, max: I32, within: I32): Supervisor` | starts a supervisor of `children`, in order |
| `child<M>(sup: Supervisor, name: Bytes): ?Process<M>` | the process of the child `name` as it runs now, or `null` |
| `Supervisor.stop()` | stops the supervisor and its children, parking until all have finished |
| `Supervisor.join(): Exit` | parks until the supervisor has finished |

When children restart together, the others are stopped first, last started first, and start
again in order, except those that had already finished and temporary ones. Once more than `max`
restarts happen within `within` milliseconds, the supervisor stops its children and crashes,
throwing `too many restarts`. The children belong to the supervisor, so stopping it stops them,
and the supervisor belongs to the process that started it.

A child that receives messages makes its own inbox, `new Inbox<M>()`; others find it with
`child`, which gives its current process after any restart. `M` is the child's message type,
which only the caller knows.

```jello
// expect: 3 starts, answered 3
// expect: Exit.Crashed("too many restarts")
import std.concurrent.Process as Process;
import type { Process, Inbox, Reply } from std.concurrent.Process;
import std.concurrent.Supervisor as Supervisor;
import type { Supervisor, Child, Strategy, Restart } from std.concurrent.Supervisor;
import { Strategy, Restart } from std.concurrent.Supervisor;
import type { Buffer } from std.concurrent.Buffer;
import { writeText } from std.io.File;

enum Ask { Starts(reply: Reply<I32>); }

let starts = new Buffer(8);
// A child that crashes on its first two runs, then answers how many times it has started.
let flaky = new Child("flaky", fn() {
  let run = starts.addI32(0, 1) + 1;
  if (run < 3) { throw "crash"; };
  let inbox = new Inbox<Ask>();
  while (true) { match (inbox.receive()) { Starts(reply) => reply.send(run) }; }
}, Restart.Permanent);

let sup = Supervisor.start(Strategy.OneForOne, [flaky], 5, 60000);
let found: ?Process<Ask> = null;
while (found == null) {
  Process.sleep(1);
  if (starts.getI32(0) >= 3) { found = Supervisor.child<Ask>(sup, "flaky"); }
}
let answered = match (found) {
  null => -1,
  p => Process.call<Ask, I32>(p, fn(r: Reply<I32>): Ask { Ask.Starts(r) }),
};
writeText("/dev/stdout", Std.string(starts.getI32(0)) + " starts, answered "
  + Std.string(answered) + "\n");
sup.stop();

let doomed = Supervisor.start(Strategy.OneForOne,
  [new Child("doomed", fn() { throw "always"; }, Restart.Permanent)], 2, 60000);
Std.string(doomed.join())
```
