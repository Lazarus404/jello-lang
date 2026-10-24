#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Makes stage 1 the seed, boot/jelloc.jlo: the committed compiler that builds
# the compiler (./scripts/build_jello.sh). It runs ./scripts/bootstrap.sh first
# and promotes only at its fixed point, so the seed is a compiler that compiles
# its own sources to itself. Promoting accepts the code the new compiler emits,
# so the fixed point does not hold it to the old seed's modules (PROMOTING). The
# compiler's sources can use only what the seed compiles: a language change lands
# in the compiler, is promoted, and only then appears in its sources. Commit the
# seed with the change that needs it.

set -euo pipefail
[[ $# -eq 0 ]] || { echo "usage: $0 (takes no arguments)" >&2; exit 2; }

. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
PROMOTING=1 "$(dirname "$COMPILER_DIR")/scripts/bootstrap.sh"
cp "$COMPILER_DIR/build/jelloc1.jlo" "$SEED"
echo "== the seed is stage 1 =="
