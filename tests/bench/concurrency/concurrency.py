#!/usr/bin/env python3
"""Jello vs. Go and Elixir (BEAM) concurrency benchmark harness.

Runs each tests/bench/concurrency/jello/NAME.jello, compiled by jelloc and run
by jello with its JIT, beside go/NAME.go and elixir/NAME.exs, the same
program in Go and in Elixir, on one scheduler and on one a CPU. Each program
checks its own result and prints `ms=N`, the milliseconds its workload took
on a monotonic clock; that time, not the process's, is what the table
reports, since Elixir spends hundreds of milliseconds booting the BEAM and
compiling the script before its workload starts. A side with no counterpart
file, or whose language is not installed, is left out of that row.

The `idle` row instead reports the resident memory each process costs while
all of them wait in receive: the peak resident size of a run with IDLE_N
processes, less that of a run with none, over IDLE_N. The peak also holds the
little each language keeps per process to reach it, such as a handle or a
pid, and what spawning left for the collector.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(BENCH_DIR)))
OUT_DIR = os.path.join(os.path.dirname(BENCH_DIR), "out", "concurrency")
BUILD_DIR = os.environ.get("JELLO_VM_BUILD_DIR") or os.path.join(ROOT, "vm", "build")
IDLE_N = 100000


def measure(cmd: list[str], env: dict[str, str], timeout: float) -> tuple[int, int]:
    """Runs CMD; the milliseconds its workload took, as it printed them, and its peak resident
    bytes."""
    with tempfile.TemporaryFile() as err:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err, env=env)
        timed_out = threading.Event()
        killer = threading.Timer(timeout, lambda: (timed_out.set(), p.kill()))
        killer.start()
        with p.stdout:
            out = p.stdout.read().decode()
        # wait4, unlike Popen.wait, gives the child's own resource usage.
        _, status, usage = os.wait4(p.pid, 0)
        p.returncode = status
        killer.cancel()
        if status != 0:
            err.seek(0)
            why = f"timed out after {timeout:.0f} s" if timed_out.is_set() else f"failed ({status})"
            raise SystemExit(f"error: {' '.join(cmd)} {why}: {out!r} {err.read()!r}")
    m = re.search(r"ms=(\d+)", out)
    if not m:
        raise SystemExit(f"error: {' '.join(cmd)} printed no ms=: {out!r}")
    # ru_maxrss is in bytes on macOS and in KiB on Linux.
    return int(m.group(1)), usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024


def build(cmd: list[str], env: dict[str, str]) -> None:
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if p.returncode != 0:
        raise SystemExit(f"error: {' '.join(cmd)}: {p.stdout}{p.stderr}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=3, help="rounds; each runs every side once")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--filter", default="", help="only the benchmarks whose name contains this")
    ap.add_argument("--markdown-out", default="")
    args = ap.parse_args()

    cores = os.cpu_count() or 1
    jelloc, jello = os.path.join(BUILD_DIR, "jelloc"), os.path.join(BUILD_DIR, "jello")
    for tool in (jelloc, jello):
        if not os.path.exists(tool):
            raise SystemExit(f"error: {tool} not found (run ./scripts/build_vm.sh and ./scripts/build_jello.sh)")
    base = dict(os.environ, JELLO_STD_PATH=os.path.join(ROOT, "stdlib"))
    for var in ("JELLO_JIT_DISABLE", "JELLO_SCHEDULERS", "GOMAXPROCS"):
        base.pop(var, None)

    names = sorted(f[:-6] for f in os.listdir(os.path.join(BENCH_DIR, "jello")) if f.endswith(".jello"))
    names = [n for n in names if args.filter in n]
    if not names:
        raise SystemExit(f"error: --filter {args.filter!r} matched nothing")
    os.makedirs(OUT_DIR, exist_ok=True)
    have = {tool: shutil.which(tool) is not None for tool in ("go", "elixir")}
    for tool, found in have.items():
        if not found:
            print(f"warning: no `{tool}` on PATH; its sides are left out")

    counts = sorted({1, cores})
    columns = [f"{lang} N={n}" for lang in ("Jello", "Go", "BEAM") for n in counts]
    print(f"cores: {cores}; times are each workload's median milliseconds over {args.samples} rounds")
    rows: list[tuple[str, dict[str, str]]] = []
    for name in names:
        # Each side: its column, its command and the environment giving it n schedulers.
        sides: list[tuple[str, list[str], dict[str, str]]] = []
        jlo = os.path.join(OUT_DIR, f"{name}.jlo")
        build([jelloc, os.path.join(BENCH_DIR, "jello", f"{name}.jello"), "--out", jlo], base)
        sides += [(f"Jello N={n}", [jello, "--require-jit", jlo], dict(base, JELLO_SCHEDULERS=str(n)))
                  for n in counts]
        go_src = os.path.join(BENCH_DIR, "go", f"{name}.go")
        if os.path.exists(go_src) and have["go"]:
            exe = os.path.join(OUT_DIR, f"{name}_go")
            build(["go", "build", "-o", exe, go_src], base)
            sides += [(f"Go N={n}", [exe], dict(base, GOMAXPROCS=str(n))) for n in counts]
        exs = os.path.join(BENCH_DIR, "elixir", f"{name}.exs")
        if os.path.exists(exs) and have["elixir"]:
            # A script's Mix.install takes its packages from the local Hex cache, never the network.
            sides += [(f"BEAM N={n}", ["elixir", "--erl", f"+S {n}:{n} +P 2000000", exs],
                       dict(base, HEX_OFFLINE="1")) for n in counts]

        cells: dict[str, str] = {}
        if name == "idle":
            for side, cmd, env in sides:
                full = [measure(cmd, dict(env, IDLE_N=str(IDLE_N)), args.timeout)[1]
                        for _ in range(args.samples)]
                empty = measure(cmd, dict(env, IDLE_N="0"), args.timeout)[1]
                cells[side] = f"{(statistics.median(full) - empty) / IDLE_N / 1024:.2f} KiB"
        else:
            times: dict[str, list[int]] = {side: [] for side, _, _ in sides}
            for _ in range(args.samples):
                for side, cmd, env in sides:
                    times[side].append(measure(cmd, env, args.timeout)[0])
            cells = {side: f"{statistics.median(ts):.0f}" for side, ts in times.items()}
        rows.append((name, cells))
        print(f"  {name}: " + ", ".join(f"{s} {c}" for s, c in cells.items()), flush=True)

    lines = ["| bench | " + " | ".join(columns) + " |", "|---|" + "---:|" * len(columns)]
    lines += [f"| {name} | " + " | ".join(cells.get(c, "") for c in columns) + " |" for name, cells in rows]
    table = "\n".join(lines) + "\n"
    print()
    print(table, end="")
    if args.markdown_out:
        with open(args.markdown_out, "w", encoding="utf-8") as f:
            f.write(table)
    return 0


if __name__ == "__main__":
    sys.exit(main())
