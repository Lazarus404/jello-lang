#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Builds the VM (scripts/vm.sh says where from and to); any arguments are extra CMake arguments.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/vm.sh"

vm_build "$VM_BUILD" "$@"

printf 'Built:\n  %s\n  %s\n  %s\n  %s\n  %s\n' \
  "$VM_BUILD/jello" \
  "$VM_BUILD/jlo_tool" \
  "$VM_BUILD/libjello_vm.a" \
  "$VM_BUILD/std.jdll" \
  "$VM_BUILD/tls.jdll"
