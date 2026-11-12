#!/usr/bin/env python3
"""Jello vs. Lua benchmark harness.

Compiles each tests/bench/jello/NAME.jello with jelloc and runs it on jello,
with its JIT and with the interpreter alone, beside tests/bench/lua/NAME.lua,
the same algorithm on the same input, on PUC-Rio Lua and on LuaJIT, with its
JIT and without. Each program checks its own result.

A sample is one process, timed from launch to exit, so start-up, loading and
JIT compilation count, as they do for a user. The `startup` row is a program
that does nothing. Each round runs every side once, in turn, so a slow spell
of the machine falls on all of them; the table reports each side's median.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from typing import Dict, List, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
JELLO_DIR = os.path.join(BENCH_DIR, "jello")
LUA_DIR = os.path.join(BENCH_DIR, "lua")
OUT_DIR = os.path.join(BENCH_DIR, "out")
BUILD_DIR = os.environ.get("JELLO_VM_BUILD_DIR") or os.path.join(ROOT, "build", "vm")


def lua_version(exe: str) -> str:
    p = subprocess.run([exe, "-v"], capture_output=True, text=True, timeout=10)
    return (p.stdout + p.stderr).strip().split("\n")[0]


def find_luas(skip_luajit: bool) -> List[Dict[str, object]]:
    """The Lua sides: PUC-Rio Lua, required, and LuaJIT when it is on PATH."""
    lua = shutil.which("lua")
    version = lua_version(lua) if lua else ""
    if not lua or "LuaJIT" in version:
        raise SystemExit("error: `lua` must be PUC-Rio Lua 5.x (e.g. brew install lua)")
    sides: List[Dict[str, object]] = [{"name": "Lua", "cmd": [lua], "version": version}]
    luajit = None if skip_luajit else shutil.which("luajit")
    if luajit:
        version = lua_version(luajit)
        sides.append({"name": "LuaJIT", "cmd": [luajit], "version": version})
        sides.append({"name": "LuaJIT -joff", "cmd": [luajit, "-joff"], "version": version})
    return sides


def run_once(cmd: List[str], timeout: float) -> float:
    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)
    elapsed = time.perf_counter() - t0
    if p.returncode != 0:
        raise SystemExit(f"error: {' '.join(cmd)} exited {p.returncode}: {p.stdout!r} {p.stderr!r}")
    return elapsed * 1000.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=5, help="rounds; each runs every side once")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--filter", default="", help="only the benchmarks whose name contains this")
    ap.add_argument("--no-interp", action="store_true", help="skip the interpreter-only runs")
    ap.add_argument("--no-luajit", action="store_true", help="skip LuaJIT")
    ap.add_argument("--markdown-out", default="")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    jelloc = os.path.join(BUILD_DIR, "jelloc")
    jello = os.path.join(BUILD_DIR, "jello")
    for tool in (jelloc, jello):
        if not os.path.exists(tool):
            raise SystemExit(f"error: {tool} not found (run ./scripts/build_vm.sh and ./scripts/build_jello.sh)")
    env = dict(os.environ, JELLO_STD_PATH=os.path.join(ROOT, "stdlib"))
    env.pop("JELLO_JIT_DISABLE", None)

    names = sorted(f[:-6] for f in os.listdir(JELLO_DIR) if f.endswith(".jello"))
    names = [n for n in names if args.filter in n]
    if not names:
        raise SystemExit(f"error: --filter {args.filter!r} matched nothing")
    os.makedirs(OUT_DIR, exist_ok=True)

    luas = find_luas(args.no_luajit)
    jellos = [{"name": "Jello JIT", "cmd": [jello, "--require-jit"]}]
    if not args.no_interp:
        jellos.append({"name": "Jello interp", "cmd": [jello, "--no-jit"]})
    print(f"jello: {jello}")
    for side in luas:
        print(f"{side['name']}: {' '.join(side['cmd'])} ({side['version']})")
    print()

    rows: List[Dict[str, object]] = []
    for name in names:
        jlo = os.path.join(OUT_DIR, f"{name}.jlo")
        p = subprocess.run([jelloc, os.path.join(JELLO_DIR, f"{name}.jello"), "--out", jlo],
                           capture_output=True, text=True, env=env)
        if p.returncode != 0:
            raise SystemExit(f"error: jelloc {name}.jello: {p.stdout}{p.stderr}")
        lua_src = os.path.join(LUA_DIR, f"{name}.lua")
        if not os.path.exists(lua_src):
            raise SystemExit(f"error: missing Lua counterpart {lua_src}")
        sides = [(s["name"], s["cmd"] + [jlo]) for s in jellos] + \
                [(s["name"], s["cmd"] + [lua_src]) for s in luas]
        times: Dict[str, List[float]] = {n: [] for n, _ in sides}
        for _ in range(args.samples):
            for side, cmd in sides:
                times[side].append(run_once(cmd, args.timeout))
        medians = {side: statistics.median(ts) for side, ts in times.items()}
        rows.append({"name": name, "ms": medians})
        print(f"  {name}: " + ", ".join(f"{s} {m:.1f}" for s, m in medians.items()), flush=True)

    columns = list(rows[0]["ms"].keys())
    ratios = [s["name"] for s in luas]
    lines = ["| bench | " + " | ".join(f"{c} (ms)" for c in columns) + " | "
             + " | ".join(f"{r} / Jello JIT" for r in ratios) + " |",
             "|---|" + "---:|" * (len(columns) + len(ratios))]
    for r in rows:
        ms = r["ms"]
        lines.append(f"| {r['name']} | " + " | ".join(f"{ms[c]:.1f}" for c in columns) + " | "
                     + " | ".join(f"{ms[c] / ms['Jello JIT']:.2f}x" for c in ratios) + " |")
    lines.append("| **geometric mean** | " + " | ".join("" for _ in columns) + " | " + " | ".join(
        f"**{statistics.geometric_mean([r['ms'][c] / r['ms']['Jello JIT'] for r in rows]):.2f}x**"
        for c in ratios) + " |")
    table = "\n".join(lines) + "\n"
    print()
    print("A ratio above 1.00x means Jello's JIT is faster.")
    print(table, end="")
    if args.markdown_out:
        with open(args.markdown_out, "w", encoding="utf-8") as f:
            f.write(table)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump({"lua": {s["name"]: s["version"] for s in luas}, "benches": rows}, f, indent=2)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
