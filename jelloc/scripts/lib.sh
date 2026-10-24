# SPDX-License-Identifier: BSD-3-Clause

# Sourced by test_jello.sh and the scripts that check the compiler: the
# compiler under test (JELLOC_JLO, JELLOC_VM, JELLOC_JDLL; env.sh) and what
# they check it with.

. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

# MacOS ships bash 3.2, where a failing `[[ ... ]]` does not trigger `set -e`.
# Every check therefore fails explicitly (`fail`, `expect`, `|| exit`) etc.

fail() { printf '%s: %s\n' "${0##*/}" "$*" >&2; exit 1; }
expect() { [[ "$1" == "$2" ]] || fail "$3: expected '$2', got '$1'"; }

# Stops the shell's background jobs, and the VMs they started, since killing
# only a job would leave its VM writing its output. Children go first, since
# a killed process's children are reparented out of reach.

kill_tree() {
  local child
  for child in $(pgrep -P "$1"); do kill_tree "$child"; done
  kill "$1" 2>/dev/null || true
}
kill_jobs() { local job; for job in $(jobs -p); do kill_tree "$job"; done; }

# The arm64 JIT must compile what the tests run (--require-jit); the x64 JIT
# declines more and is not held to it, and a VM built with JIT=0 has none.
# The VM's architecture, not the shell's, under Rosetta an x86_64 VM runs
# from an arm64 shell, and a universal VM runs the host's slice. A VM that
# cannot answer --jit-available predates it.

REQUIRE_JIT=""
vm_arch="$(file -b "$VM" 2>/dev/null || true)"
[[ "$vm_arch" != *universal* ]] || vm_arch="$(uname -m)"
case "$vm_arch" in
  *arm64* | *aarch64*) vm_arch=arm64 ;;
  *x86_64* | *x86-64*) vm_arch=x64 ;;
  *) fail "cannot tell which architecture $VM is for (is file(1) installed?)" ;;
esac
if [[ "$vm_arch" == arm64 ]]; then
  jit_status=0
  "$VM" --jit-available 2>/dev/null || jit_status=$?
  case "$jit_status" in
    0) REQUIRE_JIT="--require-jit" ;;
    1) ;;
    *) fail "$VM does not answer --jit-available; rebuild it (./scripts/build_vm.sh)" ;;
  esac
fi

# mode_flag MODE: the VM flag for running a program in MODE, where jit
# (functions compile as they are called, the production path), eager
# (every function first, failing if one declines - arm64 only) or
# no-jit (the interpreter).

mode_flag() { case "$1" in eager) echo "$REQUIRE_JIT" ;; no-jit) echo --no-jit ;; esac; }

# repo_vm ARGS...: the repository VM, validating what it loads, with
# MallocScribble (macOS), which fills new allocations with junk so that
# reading memory the VM never initialized fails instead of passing by luck.

REPO_VM=(env -u JELLO_BC_SKIP_VALIDATE -u JELLO_JIT_DISABLE MallocScribble=1 JELLO_JDLL_PATH="$VM_BUILD" "$VM")
repo_vm() { "${REPO_VM[@]}" "$@"; }

# repo_vm_within SECS ARGS...: repo_vm, killed after SECS seconds. Perl execs
# the VM itself so that the alarm reaches it, through a shell it would kill only
# the shell, and the VM would keep the output open, so a hang would never end.

repo_vm_within() { perl -e 'alarm shift; exec @ARGV' "$1" "${REPO_VM[@]}" "${@:2}"; }
repo_jlo_tool() { env -u JELLO_BC_SKIP_VALIDATE JELLO_JDLL_PATH="$VM_BUILD" "$VM_BUILD/jlo_tool" "$@"; }

# decodes IR JLO DIS WHAT: the VM loads JLO (validating it) and disassembles it
# to DIS, which must decode back to IR, the program the compiler allocated.
# scripts/roundtrip.py compares the two instruction by instruction and block by
# block.

decodes() {
  local problems
  repo_jlo_tool disasm "$2" -o "$3" 2>"$3.err" || fail "$4: the VM rejects the emitted bytes: $(cat "$3.err")"
  problems="$(python3 "$COMPILER_DIR/scripts/roundtrip.py" "$1" "$3")" ||
    fail "$4: the emitted bytes do not decode to the allocated program: $problems"
}
