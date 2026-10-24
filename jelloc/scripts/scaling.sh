#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# How the compiler's time grows with the size of one function.
# Generates a function of N groups, N = 400, 800, 1600 and 3200 (1x to 8x), in
# three shapes:
#
#   branchy   each group declares two locals and runs a while loop with an
#             if-else in it: about six blocks a group;
#   straight  each group declares a local and assigns another: one block;
#   calls     the straight shape, computing each value with a call.
# and times the compiler on it to three points, the median of RUNS runs each
# (default 3), less the same for an empty function:
#   check     --hir: parsing and type checking;
#   lower     --ir less --hir: lowering, and verifying the IR;
#   allocate  --jlo less --ir: legalization and register allocation, and
#             verifying each.
#
# Each phase's growth from one size to the next is printed beside its time,
# unless the smaller time is under 50 ms, which is noise: 2.0 is linear, 4.0
# quadratic. The compiler is new_jelloc (env.sh), with JELLOC_JLO
# defaulting to stage 1, build/jelloc1.jlo, on the JIT.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${JELLOC_JLO:=$ROOT/build/jelloc1.jlo}"
. "$ROOT/scripts/env.sh"
[[ -f "$JELLOC_JLO" ]] || { echo "scaling.sh: missing $JELLOC_JLO (run ./scripts/build_jello.sh)" >&2; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

export -f new_jelloc
export JELLOC_VM JELLOC_JDLL
python3 - "$TMP" "$ROOT/../stdlib" "${RUNS:-3}" <<'EOF'
import os, statistics, subprocess, sys, time
tmp, std, runs = sys.argv[1], sys.argv[2], int(sys.argv[3])
SIZES = [400, 800, 1600, 3200]
SHAPES = {
    "branchy": lambda k: f"  let a{k} = n + {k};\n  let i{k} = 0;\n  while (i{k} < a{k}) {{\n"
                         f"    if ((i{k} & 1) == 0) {{ s = s + i{k}; }} else {{ s = s - {k}; }}\n    i{k}++;\n  }}\n",
    "straight": lambda k: f"  let a{k} = n + {k};\n  s = s + a{k} * 3;\n",
    "calls": lambda k: f"  let a{k} = add(n, {k});\n  s = add(s, a{k});\n",
}
# The shell new_jelloc starts in takes the same time at every size, so the
# empty function's time takes it out.
compiler = ["bash", "-c", 'new_jelloc "$@"', "new_jelloc"]

def source(shape, n):
    path = os.path.join(tmp, f"{shape}{n}.jello" if n else "empty.jello")
    with open(path, "w") as f:
        f.write("let add = fn(a: I32, b: I32): I32 { a + b };\nlet work = fn(n: I32): I32 {\n  let s = 0;\n")
        f.write("".join(SHAPES[shape](k) for k in range(n)))
        f.write("  s\n};\nwork(3)\n")
    return path

def seconds(path, mode):
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        if subprocess.run(compiler + [path, "--std", std, mode], stdout=subprocess.DEVNULL).returncode != 0:
            sys.exit("scaling.sh: %s %s failed" % (os.path.basename(path), mode))
        times.append(time.perf_counter() - start)
    return statistics.median(times)

def phases(path):
    hir, ir, jlo = (seconds(path, mode) for mode in ("--hir", "--ir", "--jlo"))
    return [hir, ir - hir, jlo - ir]

print("%-9s %6s  %-14s %-14s %-14s" % ("", "groups", "check", "lower", "allocate"))
empty = phases(source("straight", 0))
for shape in SHAPES:
    previous = None
    for n in SIZES:
        times = [max(t - e, 0.0) for t, e in zip(phases(source(shape, n)), empty)]
        cells = []
        for i, t in enumerate(times):
            growth = "" if previous is None or previous[i] < 0.05 else "x%.1f" % (t / previous[i])
            cells.append("%6.2fs %-6s" % (t, growth))
        print("%-9s %6d  %s" % (shape, n, " ".join(cells)))
        previous = times
EOF
