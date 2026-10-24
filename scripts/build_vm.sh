#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${JELLO_VM_BUILD_DIR:-$ROOT/vm/build}"

make -C "$ROOT/vm" BUILD_DIR="$BUILD" "$@"

printf 'Built:\n  %s\n  %s\n  %s\n  %s\n  %s\n' \
  "$BUILD/jello" \
  "$BUILD/jlo_tool" \
  "$BUILD/libjello_vm.a" \
  "$BUILD/std.jdll" \
  "$BUILD/tls.jdll"
