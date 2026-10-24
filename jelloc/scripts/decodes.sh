#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# scripts/decodes.sh DUMP MODULE. The e2e tier's check after each compile
# (degust.Programs' --check; scripts/test.sh). The VM loads MODULE, 
# validating it, and its bytes decode back to the program the compiler 
# allocated and printed, DUMP (decodes, lib.sh).

set -euo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
dis="$(mktemp)"
trap 'rm -f "$dis" "$dis.err"' EXIT
decodes "$1" "$2" "$dis" "$2"
