#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Builds the compiler from the committed seed, stage 0
# (jelloc/boot/jelloc.jlo), on the repository VM (./scripts/build_vm.sh):
# stage 0 builds jelloc/build/jelloc.jlo, which builds stage 1,
# build/jelloc1.jlo, and jelloc, the compiler as a native program, embeds
# stage 1 (vm/build/jelloc). scripts/fixed-point.sh checks the result.

set -euo pipefail
[[ $# -eq 0 ]] || { echo "usage: $0 (takes no arguments)" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$ROOT/jelloc"
. "$PROJECT/scripts/env.sh"
[[ -x "$VM" && -f "$VM_BUILD/std.jdll" ]] || { echo "build_jello.sh: missing $VM or its std.jdll (run ./scripts/build_vm.sh)" >&2; exit 1; }

JELLOC_JLO="$SEED" OUT="$PROJECT/build/jelloc.jlo" "$PROJECT/scripts/self-compile.sh" > /dev/null

# A current seed builds itself, so stage 1 is the seed too. Its file is left
# alone when it already is, so jelloc is not rebuilt.

if cmp -s "$SEED" "$PROJECT/build/jelloc.jlo"; then
  cmp -s "$SEED" "$PROJECT/build/jelloc1.jlo" || cp "$SEED" "$PROJECT/build/jelloc1.jlo"
else
  JELLOC_JLO="$PROJECT/build/jelloc.jlo" OUT="$PROJECT/build/jelloc1.jlo" "$PROJECT/scripts/self-compile.sh" > /dev/null
fi
make -s -C "$ROOT/vm" BUILD_DIR="$VM_BUILD" jelloc

printf 'Built:\n  %s\n  %s\n  %s\n' "$PROJECT/build/jelloc.jlo" "$PROJECT/build/jelloc1.jlo" "$VM_BUILD/jelloc"
