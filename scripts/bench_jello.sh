#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Claude generated - don't blame me

# Benchmarks for the new compiler:
#   run      the workload programs, as stage 1 builds them, on this repository's
#            VM (JIT, and the interpreter alone). corpus parses the whole
#            language corpus.
#   compile  the compiler compiling each workload: each side's sources, as
#            stage 1 builds them, on this tree's VM (JIT), so the two differ
#            only in their sources.
#   self     stage 1 compiling itself, on the JIT and on the
#            interpreter alone. A baseline revision from before the seed, which
#            may not compile itself, reports these rows without comparing them.
#            The jelloc1 rows compile each side's own sources, so they also
#            measure how those sources grew; the frozen rows compile the
#            baseline's sources on both sides, which measures the compiler alone.
#
# The working tree is compared with a baseline revision, named in
# jelloc/tests/bench/baseline-revision: its VM and std.jdll, built from
# its sources (the committed builds lag them), its compiler's sources and its
# workload sources. Each row runs the
# two in alternation, RUNS rounds; a short row repeats its pairs for 200 ms a
# round, and a row whose first run takes over 2 s runs 3 rounds. Its change
# is the median of the per-pair ratios, and a row more than 10% slower fails.
# `--save [REV]` makes REV (default HEAD) the baseline; BENCH_REV=REV compares
# with REV for one run without saving it.
#
# Why pairs: times saved on another day cannot be compared. An identical
# 5 ms program's fastest of 21 runs moves 24% between batches, and whole
# batches run slow together. Run against itself, this script reads within
# 1.1% on every row (two runs, 2026-10-08); with the working tree's JIT rows
# run without the JIT, it fails reparse (+104%) and corpus (+82%) only.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNS="${RUNS:-21}"
BASELINE="$ROOT/jelloc/tests/bench/baseline-revision"
[[ $# -eq 0 || ( "$1" == --save && $# -le 2 ) ]] || { echo "usage: $0 [--save [REV]]" >&2; exit 2; }
if [[ "${1:-}" == --save ]]; then
  rev="$(git -C "$ROOT" rev-parse --verify "${2:-HEAD}^{commit}")"
  echo "$rev" > "$BASELINE"
  echo "Baseline is now $rev"
  exit 0
fi
if [[ -n "${BENCH_REV:-}" ]]; then
  REV="$(git -C "$ROOT" rev-parse --verify "$BENCH_REV^{commit}")" ||
    { echo "bench_jello.sh: BENCH_REV $BENCH_REV is not a commit" >&2; exit 2; }
else
  [[ -f "$BASELINE" ]] || { echo "bench_jello.sh: no baseline; run bench_jello.sh --save" >&2; exit 1; }
  REV="$(cat "$BASELINE")"
fi

# Every run validates what it loads and may use the JIT, as shipped, and finds
# the standard library only where a row says.
unset JELLO_BC_SKIP_VALIDATE JELLO_JIT_DISABLE JELLO_STD_PATH
WORKLOADS="lexer parser reparse ast corpus"

# Stage 1 from the working tree's sources, not whatever build is on disk.
"$ROOT/scripts/build_jello.sh" > /dev/null
cd "$ROOT/jelloc"
B="$(mktemp -d)"
trap 'rm -rf "$B"' EXIT

# Each side's VM and std.jdll together, and the workload sources and stdlib it
# compiles (workload/frontend and workload/support link into src). This tree's files run
# from copies too, so neither side is a fresh file the other is not. This
# tree's stage 1 builds the baseline's compiler, base/jelloc.jlo. A baseline
# with a seed can compile itself, so that build compiles the baseline's
# sources again, to its stage 1, for the self rows.
mkdir -p "$B/base/vm" "$B/new/vm"
git -C "$ROOT" archive "$REV" vm/Makefile vm/src vm/std vm/tools vm/third_party | tar -x -C "$B/base"

# The VM is jello, or jellovm in older revisions.
base_vm=jello
git -C "$ROOT" cat-file -e "$REV:vm/tools/jello.c" 2>/dev/null || base_vm=jellovm
make -s -C "$B/base/vm" BUILD_DIR="$B/base/vm/build" "$B/base/vm/build/$base_vm" "$B/base/vm/build/std.jdll" > /dev/null ||
  { echo "bench_jello.sh: cannot build the baseline's VM" >&2; exit 1; }
cp "$B/base/vm/build/$base_vm" "$B/base/vm/jello"
for f in jello std.jdll; do cp "$ROOT/vm/build/$f" "$B/new/vm/$f"; done
cp "$B/base/vm/build/std.jdll" "$B/base/vm/std.jdll"
cp build/jelloc1.jlo "$B/new/jelloc1.jlo"

# The compiler is in jelloc/; the
# workload in its tests/workload, or itests/emit/workload in older ones.
base_dir=jelloc
git -C "$ROOT" cat-file -e "$REV:$base_dir/src" 2>/dev/null || base_dir=jelloc
workload=$base_dir/tests/workload
git -C "$ROOT" cat-file -e "$REV:$workload" 2>/dev/null || workload=$base_dir/itests/emit/workload
git -C "$ROOT" archive "$REV" "$workload" "$base_dir/src" stdlib | tar -x -C "$B/base"
BASE_WORKLOAD="$B/base/$workload"
BASE_MAIN="$B/base/$base_dir/src/Main.jello"

# compile ARGS...: a compile, ARGS, on this tree's VM, without the empty line
# it prints.
compile() { JELLO_JDLL_PATH="$B/new/vm" "$B/new/vm/jello" "$@" > /dev/null; }
compile "$B/new/jelloc1.jlo" "$BASE_MAIN" --std "$B/base/stdlib" --out "$B/base/jelloc.jlo" ||
  { echo "bench_jello.sh: stage 1 cannot build the baseline's compiler" >&2; exit 1; }
BASE_SELF=""
if git -C "$ROOT" cat-file -e "$REV:$base_dir/boot/jelloc.jlo" 2>/dev/null; then
  BASE_SELF=1
  compile "$B/base/jelloc.jlo" "$BASE_MAIN" --std "$B/base/stdlib" --out "$B/base/jelloc1.jlo" ||
    { echo "bench_jello.sh: the baseline's compiler cannot compile itself" >&2; exit 1; }
fi

# Rows, one per line as "name<TAB>new command<TAB>baseline command", the
# baseline empty for a row only reported. side_cmd SIDE VM-FLAG ARGS...: ARGS
# on SIDE's VM.
side_cmd() { printf 'JELLO_JDLL_PATH=%q %q %s' "$B/$1/vm" "$B/$1/vm/jello" "$2"; printf ' %q' "${@:3}"; }
row() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$cmds"; }
cmds="$B/commands"
: > "$cmds"

# The workloads build at once, since nothing is timed yet.
pids=()
for w in $WORKLOADS; do
  compile "$B/new/jelloc1.jlo" "tests/workload/$w.jello" --std "$ROOT/stdlib" --out "$B/$w-new.jlo" & pids+=($!)
  compile "$B/base/jelloc.jlo" "$BASE_WORKLOAD/$w.jello" --std "$B/base/stdlib" --out "$B/$w-base.jlo" & pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid" || { echo "bench_jello.sh: a workload does not compile" >&2; exit 1; }; done
for w in $WORKLOADS; do
  for mode in jit no-jit; do
    flag=""; [[ "$mode" == no-jit ]] && flag="--no-jit"
    row "run $w $mode" "$(side_cmd new "$flag" "$B/$w-new.jlo")" "$(side_cmd base "$flag" "$B/$w-base.jlo")"
  done
  row "compile $w" "$(side_cmd new "" "$B/new/jelloc1.jlo" "tests/workload/$w.jello" --std "$ROOT/stdlib" --out "$B/$w-again.jlo")" \
    "$(side_cmd new "" "$B/base/jelloc.jlo" "$BASE_WORKLOAD/$w.jello" --std "$B/base/stdlib" --out "$B/$w-base-again.jlo")"
done
for mode in jit no-jit; do
  flag=""; [[ "$mode" == no-jit ]] && flag="--no-jit"
  base=""
  [[ -z "$BASE_SELF" ]] || base="$(side_cmd base "$flag" "$B/base/jelloc1.jlo" "$BASE_MAIN" --std "$B/base/stdlib" --out "$B/self-base.jlo")"
  row "self jelloc1 $mode" "$(side_cmd new "$flag" "$B/new/jelloc1.jlo" "$ROOT/jelloc/src/Main.jello" --std "$ROOT/stdlib" --out "$B/self-new.jlo")" "$base"
  row "self frozen $mode" "$(side_cmd new "$flag" "$B/new/jelloc1.jlo" "$BASE_MAIN" --std "$B/base/stdlib" --out "$B/frozen-new.jlo")" "$base"
done

# Programs run from a directory mirroring jelloc/, where the workloads
# find their inputs. It also has the lexer and parser at their old paths, which
# an older baseline's reparse workload reads.
run="$B/run"; mkdir -p "$run/src/frontend"
for d in itests tests; do ln -s "$PWD/$d" "$run/$d"; done
ln -s "$PWD/src/frontend/syntax" "$run/src/frontend/syntax"
for f in Parser Lexer; do ln -s "$PWD/src/frontend/syntax/$f.jello" "$run/src/frontend/$f.jello"; done
cd "$run"

python3 - "$cmds" "$RUNS" "$REV" <<'EOF'
import os, shlex, statistics, subprocess, sys, time
cmds_path, runs, rev = sys.argv[1], int(sys.argv[2]), sys.argv[3]
rows = [line.rstrip("\n").split("\t") for line in open(cmds_path)]
# Run without a shell, which would add its own start-up to every time; a
# leading NAME=value word sets the environment.
def prepare(cmd):
    words, env = shlex.split(cmd), dict(os.environ)
    while "=" in words[0]:
        key, value = words.pop(0).split("=", 1)
        env[key] = value
    return words, env
def timed(prepared):
    words, env = prepared
    start = time.perf_counter()
    subprocess.run(words, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return (time.perf_counter() - start) * 1000
new = {name: [] for name, _, _ in rows}
base = {name: [] for name, _, _ in rows}
ratios = {name: [] for name, _, _ in rows}
prepared = {name: (prepare(n), prepare(b) if b else None) for name, n, b in rows}
# A row shorter than this repeats its pairs for this long each round, after
# a pair it discards (the first runs after a multi-second compile are slow).
# A row whose first run is longer than LONG_MS runs LONG_ROUNDS rounds.
SHORT_MS, LONG_MS, LONG_ROUNDS = 200, 2000, 3
for i in range(runs):
    for name, _, _ in rows:
        if i >= LONG_ROUNDS and new[name][0] > LONG_MS:
            continue
        n, b = prepared[name]
        if b is None:
            new[name].append(timed(n))
            continue
        short = bool(new[name]) and statistics.median(new[name]) < SHORT_MS / 4
        if short:
            timed(n); timed(b)
        spent, k = 0.0, 0
        while k == 0 or (short and spent < SHORT_MS):
            # Alternate which goes first, so neither always follows the other.
            if (i + k) % 2 == 0:
                tn = timed(n); tb = timed(b)
            else:
                tb = timed(b); tn = timed(n)
            new[name].append(tn); base[name].append(tb); ratios[name].append(tn / tb)
            spent += tn + tb; k += 1
print("baseline %s, %d rounds (short rows: pairs for %d ms a round); times in ms" % (rev[:10], runs, SHORT_MS))
print("%-24s %9s %9s %9s %9s" % ("", "median", "fastest", "baseline", "change"))
failed = []
for name, _, _ in rows:
    line = "%-24s %9.2f %9.2f" % (name, statistics.median(new[name]), min(new[name]))
    if ratios[name]:
        change = (statistics.median(ratios[name]) - 1) * 100
        line += " %9.2f %+8.1f%%" % (statistics.median(base[name]), change)
        if change > 10:
            failed.append(name)
    print(line)
if failed:
    print("more than 10% slower than the baseline:", ", ".join(failed))
sys.exit(1 if failed else 0)
EOF
