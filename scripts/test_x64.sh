#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Builds the VM for x86_64 and runs test_vm.sh and test_jello.sh against it,
# so the x64 JIT is gated like the arm64 one. On Apple Silicon the binaries
# run under Rosetta 2 (directly: `arch -x86_64` breaks, since xcrun is
# arm64-only). Rosetta is not x64 hardware: alignment and memory-ordering
# faults may differ, and its timings mean nothing.

set -euo pipefail
[[ $# -eq 0 ]] || { echo "usage: $0 (takes no arguments)" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "$(uname -m)" != x86_64 ]]; then
  if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
    echo "test_x64.sh: needs an x86_64 host or Rosetta 2" >&2
    exit 1
  fi
  export CC="clang -arch x86_64"
fi
BUILDS="${JELLO_VM_BUILD_DIR:-${TMPDIR:-/tmp}/jello-vm-x64}"

# Builds the VM in JELLO_VM_BUILD_DIR, which must then be x86_64: a VM of
# another architecture would pass the gates without running the x64 JIT.

build_x64() {
  "$ROOT/scripts/build_vm.sh"
  case "$(file -b "$JELLO_VM_BUILD_DIR/jello")" in *x86_64* | *x86-64*) return ;; esac
  echo "test_x64.sh: $JELLO_VM_BUILD_DIR/jello is not x86_64; remove the directory and rerun" >&2
  exit 1
}

# The VM as shipped through both gates; test_jello.sh builds jelloc on it.

export JELLO_VM_BUILD_DIR="$BUILDS"
build_x64
"$ROOT/scripts/test_vm.sh"
"$ROOT/scripts/test_jello.sh"

# A checking build, in its own directory, in which JIT code finds every caller-saved register overwritten
# after each C call (emit_call_fn), so a value kept in one across a call
# fails every run, not only when the callee happens to use that register.
# The VM gate, and test_jello.sh under stage 1, the largest JIT workload.

export JELLO_VM_BUILD_DIR="$BUILDS-checking" CPPFLAGS="${CPPFLAGS:-} -DJELLO_JIT_SCRIBBLE_CALLS"
build_x64
"$ROOT/scripts/test_vm.sh"
JELLOC_JLO="$ROOT/jelloc/build/jelloc1.jlo" "$ROOT/scripts/test_jello.sh"
