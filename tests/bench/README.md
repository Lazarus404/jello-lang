# Jello vs. Lua benchmarks

Times Jello programs against the same programs in Lua. The numbers are
recorded for comparison; no test asserts them.

## Sides

- **Jello JIT**: the program compiled by `jelloc` and run by `jello`, which
  compiles it to native code.
- **Jello interp**: the same `.jlo` run with `jello --no-jit`.
- **Lua**: PUC-Rio Lua 5.x, a plain interpreter. Being faster than it, on the
  geometric mean of the ratios, is the bar for Jello's JIT.
- **LuaJIT** and **LuaJIT -joff**: LuaJIT with its tracing JIT, and its
  interpreter alone. Both run when `luajit` is on PATH.

## Layout

- `tests/bench/jello/NAME.jello`: the Jello side.
- `tests/bench/lua/NAME.lua`: the Lua side; the same algorithm on the same
  input, written in Lua 5.1 so LuaJIT runs it too.
- `tests/bench/bench.py`: the harness. It compiles each Jello program into
  `tests/bench/out/` (not tracked), runs every side and prints a markdown
  table.

Each program checks its own result and fails if it is wrong; `bench.py` only
times them. A pair must stay the same algorithm: neither side is tuned for its
language beyond what a programmer would write.

## Benchmarks

| Name | Shape |
|---|---|
| `binary_trees` | allocating and walking complete binary trees of records |
| `closures` | higher-order calls through closures over an array |
| `fannkuch` | array permutation and reversal |
| `fib` | double self-recursion, `fib(37)` |
| `mandelbrot` | float iteration in a called function |
| `methods` | method calls and field updates on prototype instances |
| `nbody` | float fields of records in nested loops |
| `pi` | a tight float loop at the top level, no calls |
| `sieve` | byte-buffer reads and writes |
| `spectral_norm` | float arrays and a small function called in an inner loop |
| `startup` | nothing; the cost of starting a program |
| `strings` | string concatenation and equality |
| `tak` | three-argument recursion |

## Running it

```bash
./scripts/build_vm.sh && ./scripts/build_jello.sh
python3 tests/bench/bench.py
```

A sample is one process, timed from launch to exit, so start-up, loading and
JIT compilation count. Each round runs every side once, in turn, and the table
reports each side's median over `--samples` rounds (default 5). `--filter
<substring>` runs a subset, `--no-interp` and `--no-luajit` skip those sides,
and `--markdown-out` and `--json-out` write the table and the raw numbers.
`JELLO_VM_BUILD_DIR` picks the `jello` and `jelloc` (default `vm/build`).

## Results

Measured 2026-10-20 on an Apple M5 (arm64) at 2398136, against Lua 5.5.1 and
LuaJIT 2.1, with 5 samples. Times are milliseconds per process. A ratio above
1.00x means Jello's JIT is faster.

| bench | Jello JIT (ms) | Jello interp (ms) | Lua (ms) | LuaJIT (ms) | LuaJIT -joff (ms) | Lua / Jello JIT | LuaJIT / Jello JIT | LuaJIT -joff / Jello JIT |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| binary_trees | 453.0 | 1238.8 | 1322.3 | 508.0 | 719.4 | 2.92x | 1.12x | 1.59x |
| closures | 305.8 | 1275.6 | 449.7 | 111.7 | 329.9 | 1.47x | 0.37x | 1.08x |
| fannkuch | 461.0 | 3349.2 | 1454.6 | 220.9 | 1357.2 | 3.16x | 0.48x | 2.94x |
| fib | 348.7 | 1131.2 | 597.6 | 92.9 | 410.0 | 1.71x | 0.27x | 1.18x |
| mandelbrot | 159.7 | 1081.1 | 450.3 | 187.6 | 633.7 | 2.82x | 1.17x | 3.97x |
| methods | 240.8 | 1034.8 | 399.3 | 37.5 | 346.6 | 1.66x | 0.16x | 1.44x |
| nbody | 117.5 | 908.0 | 548.4 | 33.6 | 335.5 | 4.67x | 0.29x | 2.86x |
| pi | 322.3 | 2000.3 | 496.1 | 124.1 | 589.4 | 1.54x | 0.39x | 1.83x |
| sieve | 217.7 | 753.8 | 377.2 | 132.3 | 502.1 | 1.73x | 0.61x | 2.31x |
| spectral_norm | 94.8 | 751.1 | 460.0 | 23.5 | 582.0 | 4.85x | 0.25x | 6.14x |
| startup | 4.6 | 4.4 | 4.6 | 4.8 | 4.9 | 1.00x | 1.05x | 1.07x |
| strings | 149.8 | 633.0 | 296.7 | 35.6 | 196.8 | 1.98x | 0.24x | 1.31x |
| tak | 205.5 | 787.6 | 421.3 | 89.5 | 260.4 | 2.05x | 0.44x | 1.27x |
| **geometric mean** |  |  |  |  |  | **2.19x** | **0.43x** | **1.91x** |

# Concurrency benchmarks

`tests/bench/concurrency/` times processes against Go's goroutines and
Elixir's processes on the BEAM, each side on one scheduler and on one a CPU
(`JELLO_SCHEDULERS`, `GOMAXPROCS`, `+S`).

- `jello/NAME.jello`, `go/NAME.go` and `elixir/NAME.exs`: the same program in
  each language. The jobs rows have no Elixir side. Elixir's `http_hello`
  installs Bandit and Mint with `Mix.install` from the local Hex cache.
- `concurrency.py`: the harness. It compiles each side into
  `tests/bench/out/concurrency/` (not tracked), runs every side once a round
  and prints a markdown table of medians.

Each program checks its own result and prints `ms=N`, the milliseconds its
workload took on a monotonic clock, and the table reports that rather than the
process's time: the BEAM spends hundreds of milliseconds starting and compiling
the script before the workload begins.

| Name | Shape |
|---|---|
| `call` | 200,000 request/reply round trips between two processes |
| `echo_tcp` | 50 connections, each making 2,000 round trips of 64 bytes through a TCP echo server over loopback |
| `echo_udp` | 16 sockets, each making 5,000 round trips of a 64-byte datagram through a UDP echo server |
| `fanout` | one sender, 100 messages to each of 10,000 receivers |
| `http_hello` | 50 keep-alive clients, each making 400 GETs of a fixed body from an HTTP server: `std.net.Http`, Go's `net/http` and Bandit, each language's own client |
| `idle` | resident memory per process while 100,000 wait in receive |
| `jobs_mandelbrot` | a 1,600 by 1,600 Mandelbrot set, 200 iterations a point, by `Job.parallelFor` into a `Buffer` |
| `jobs_nbody` | 800 bodies for 40 steps by `Job.parallelFor` over `Buffer`s of F64 |
| `ring` | 1,000 processes in a ring passing a token for 1,000 laps |
| `spawn` | spawning 1,000,000 processes, then joining them all |

The `idle` row is the peak resident size of a run with 100,000 processes, less
that of a run with none, over 100,000; the peak also holds what each language
keeps to reach a process (a handle, a channel or a pid) and what spawning left
for its collector. N for the second column is `os.cpu_count()`, which counts
efficiency cores as well as performance ones, so a jobs row's speedup is
bounded by less than N on such machines. The `spawn` row joins in Jello, waits on a `WaitGroup` in Go
and counts a message from each process in Elixir, each language's way to wait
for them.

```bash
python3 tests/bench/concurrency/concurrency.py
```

`--samples`, `--filter` and `--markdown-out` work as they do for `bench.py`.

## Concurrency results

Measured 2026-10-21 on an Apple M5 (arm64, 10 cores) on the tree that adds
`std.net`'s sockets and HTTP, against Go 1.26.3 and Elixir 1.20.3 on OTP 27,
with 3 rounds. Times are each workload's median milliseconds; `idle` is
resident memory per waiting process.

| bench | Jello N=1 | Jello N=10 | Go N=1 | Go N=10 | BEAM N=1 | BEAM N=10 |
|---|---:|---:|---:|---:|---:|---:|
| call | 96 | 97 | 30 | 41 | 315 | 77 |
| echo_tcp | 712 | 617 | 655 | 606 | 1636 | 1028 |
| echo_udp | 1542 | 1342 | 1518 | 1126 | 2107 | 1340 |
| fanout | 406 | 442 | 31 | 100 | 809 | 525 |
| http_hello | 293 | 133 | 283 | 164 | 573 | 229 |
| idle | 19.88 KiB | 19.88 KiB | 2.82 KiB | 2.82 KiB | 2.84 KiB | 2.97 KiB |
| jobs_mandelbrot | 1585 | 203 | 375 | 55 |  |  |
| jobs_nbody | 1791 | 381 | 27 | 7 |  |  |
| ring | 216 | 206 | 75 | 121 | 730 | 181 |
| spawn | 802 | 2872 | 321 | 218 | 6743 | 9169 |

Jello beats the BEAM on `echo_tcp`, `http_hello`, `fanout`, `spawn` and
`call` at one scheduler, matches it on `echo_udp` at ten, and serves
`http_hello` faster than Go's `net/http`; `echo_tcp` is within 2% of Go at ten
schedulers and `echo_udp` within 1.2x. It misses these targets:

- `idle`: 19.9 KiB per process, against a target of 4 KiB; a process holds one
  16 KiB page of stack, and Go and the BEAM keep under 3 KiB.
- `call` and `ring` at ten schedulers: 97 ms and 206 ms, against the BEAM's
  77 ms and 181 ms.
- `jobs_nbody` speeds up 4.7 times on ten cores, against a target of 7; the
  Mandelbrot set speeds up 7.8 times.
- `spawn` slows down as schedulers are added, behind locks every spawn takes.

