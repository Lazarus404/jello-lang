#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# The committed seed, stage 0 (jelloc/boot/jelloc.jlo),
# builds the compiler, jelloc/build/jelloc.jlo, which builds stage 1
# (./scripts/build_jello.sh); stage 1 builds stage 2 and stage 2 stage 3. Stages 1 to
# 3 must be one program, byte for byte, and stage 0's build and stage 1 must
# agree on every swept source (jelloc/scripts/fixed-point.sh).

set -euo pipefail

[[ $# -eq 0 ]] || { echo "usage: $0 (takes no arguments)" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build_jello.sh"
"$ROOT/jelloc/scripts/fixed-point.sh"
