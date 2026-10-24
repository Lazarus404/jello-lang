# The standard library

Jello's standard library has two parts: functions the language provides without an import, and
modules under the `std` package that a program imports.

## Without an import

These are always in scope; [built-ins](01-builtins.md) describes each.

| Name | What it holds |
| --- | --- |
| `Std` | `Std.string(x)`, any printable value as text |
| `System` | `System.args`, `System.exit`, `System.assert` and `System.assertEq` |
| `Math` | numeric functions over `F64`, such as `sqrt`, `pow`, `min` and `random` |
| `Float`, `Integer` | parsing numbers from text; `Float.is_nan` and `Float.is_infinite` |
| `Array`, `List`, `Bytes`, `Object` | intrinsics such as `len`, `push`, `cons`, `slice` and `keys`, and the functions of their extension modules |
| `Tuple`, `Atom`, `Function` | `Tuple.len` and `Tuple.get`; `Atom.intern`; `Function.bind` and `Function.callWith` |

Values have methods too. A function of a type's namespace that takes a value of the type first is
also a method of the value, so `Array.len(xs)` is `xs.len()` and `Bytes.trim(s)` is `s.trim()`.
Tuples, atoms and enum values have `toString()`, and enum values `tag()`.

The functions of arrays, Bytes, lists and Objects that the compiler does not implement itself are
in the *extension modules* `std.core.Array`, `std.core.Bytes`, `std.core.List` and
`std.core.Object`. The compiler loads one when a program first calls one of its functions, so they
need no import either.

```jello
// expect: [2, 4] a+b 3 {a: 1, b: 2}
let evens = [1, 2, 3, 4].filter(fn(x: I32): Bool { x % 2 == 0 });
let joined = "a,b".split(",").join("+");
let list = List.cons(1, List.cons(2, List.cons(3, List.nil<I32>())));
let o = { a: 1 };
o.merge({ b: 2 });
Std.string(evens) + " " + joined + " " + Std.string(list.len()) + " " + o.toString()
```

## Modules

| Module | Import needed | Description |
| --- | --- | --- |
| [`std.core.Array`](02-core.md#stdcorearray) | no | searching, mapping, folding, inserting and sorting arrays |
| [`std.core.Bytes`](02-core.md#stdcorebytes) | no | searching, splitting and trimming text; UTF-8; hexadecimal and Base64; fixed-width numbers |
| [`std.core.List`](02-core.md#stdcorelist) | no | the same operations for immutable lists |
| [`std.core.Object`](02-core.md#stdcoreobject) | no | field counts, merging and values of Objects |
| [`std.core.RegExp`](02-core.md#stdcoreregexp) | yes | PCRE2 regular expressions: matching, captures, replacing and splitting |
| [`std.io.File`](03-io.md#stdiofile) | yes | reading and writing files; listing and making directories |
| [`std.io.Path`](03-io.md#stdiopath) | yes | directory names, and finding files under a directory |
| [`std.io.Command`](03-io.md#stdiocommand) | yes | running other programs and capturing their output |
| [`std.io.Env`](03-io.md#stdioenv) | yes | reading environment variables |
| [`std.concurrent.Process`](04-concurrent.md#stdconcurrentprocess) | yes | processes, typed inboxes, messages, replies, timers and lifetimes |
| [`std.concurrent.Job`](04-concurrent.md#stdconcurrentjob) | yes | values computed in processes, and parallel loops |
| [`std.concurrent.Buffer`](04-concurrent.md#stdconcurrentbuffer) | yes | shared, mutable, fixed-length memory with atomic access |
| [`std.concurrent.Supervisor`](04-concurrent.md#stdconcurrentsupervisor) | yes | processes that restart their children by policy |
| [`std.net.Tcp`](05-net.md#stdnettcp) | yes | TCP listeners and streams |
| [`std.net.Udp`](05-net.md#stdnetudp) | yes | UDP sockets |
| [`std.net.Tls`](05-net.md#stdnettls) | yes | TLS configurations and secured streams |
| [`std.net.Http`](05-net.md#stdnethttp) | yes | an HTTP/1.1 server and client |
| [`std.sys.Platform`](03-io.md#stdsysplatform) | yes | the operating system, the processor and the temporary directory |
| [`std.sys.Date`](08-dates.md) | yes | the clock, and building, comparing, formatting and parsing dates |
| [`std.crypto.Hash`](09-crypto.md#stdcryptohash) | yes | digests, HMAC, constant-time comparing, and PBKDF2 and HKDF |
| [`std.crypto.Random`](09-crypto.md#stdcryptorandom) | yes | random bytes, numbers and UUIDs fit for keys and tokens |
| [`std.crypto.Cipher`](09-crypto.md#stdcryptocipher) | yes | sealing and opening data with AES-GCM and ChaCha20-Poly1305 |
| [`std.crypto.Pem`](09-crypto.md#stdcryptopem) | yes | the PEM text form of certificates and keys |
| [`std.crypto.X509`](09-crypto.md#stdcryptox509) | yes | reading certificates and checking chains of them |
| [`std.macro`](06-macro.md) | in macros, no | what a macro's body uses: `Fragment`, `emit`, `parse`, `gensym` and `literal` |
| [`std.testing`](07-testing.md) | yes | the test harness: assertions, subtests and options |

A module is imported by name, either whole or by its exports; see [modules](../05-modules.md).
Modules that export types as well as functions, such as `std.concurrent.Process`, are imported
both ways:

```text
import { split, join } from std.core.Bytes;
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;
```

```jello
// expect: 2024 10
import { find, group } from std.core.RegExp;

let m = find("released 2024-10", ~r/(\d+)-(\d+)/);
group(m, 1) + " " + group(m, 2)
```

## Natives

Some exports are implemented in C, in the VM or in the `std.jdll` library it loads, and bound with
`load { ... } from std;` or `from vm;` and a typed `let`. A program uses them through the modules'
typed exports and does not bind them itself.
