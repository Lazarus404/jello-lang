# SPDX-License-Identifier: BSD-3-Clause

# Sourced by the scripts that run the new compiler. The compiler under test
# is JELLOC_JLO, run as new_jelloc by the command JELLOC_VM (word-split, like
# CC) with its natives from JELLOC_JDLL; by default the old compiler build of
# it, build/jelloc.jlo (./scripts/build_jello.sh), on the repository VM, VM_BUILD
# (JELLO_VM_BUILD_DIR, as build_vm.sh). The old compiler is SEED, the committed
# compiler that builds the compiler (scripts/promote-seed.sh).

# The compiler finds the standard library only where a script says.

unset JELLO_STD_PATH

COMPILER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SEED="$COMPILER_DIR/boot/jelloc.jlo"

abspath() { if [[ "$1" == /* ]]; then echo "$1"; else echo "$PWD/$1"; fi; }

VM_BUILD="$(abspath "${JELLO_VM_BUILD_DIR:-$(dirname "$COMPILER_DIR")/vm/build}")"
VM="$VM_BUILD/jello"

JELLOC_JLO="$(abspath "${JELLOC_JLO:-$COMPILER_DIR/build/jelloc.jlo}")"
: "${JELLOC_VM:=$VM}" "${JELLOC_JDLL:=$VM_BUILD}"

export JELLOC_JLO JELLOC_VM JELLOC_JDLL

# The compiler under test as one command, for a tool that takes it as words
# (degust.Programs' --compiler).

jelloc_command() { echo "env JELLO_JDLL_PATH=$JELLOC_JDLL $JELLOC_VM $JELLOC_JLO"; }

new_jelloc() {
  env -u JELLO_BC_SKIP_VALIDATE -u JELLO_JIT_DISABLE JELLO_JDLL_PATH="$JELLOC_JDLL" $JELLOC_VM "$JELLOC_JLO" "$@"
}

# each_source OUT BODY PATH...; the bash script BODY for every .jello source
# under the PATHs - one per core at a time - with new_jelloc, the source as $1,
# and as $o where its output goes: OUT/, its path less .jello (the directory
# is made). Fails if a BODY does, or a PATH holds no source, which would
# check nothing.

each_source() {
  local out="$1" body="$2" path; shift 2
  for path in "$@"; do
    [[ -n "$(find "$path" -type f -name '*.jello' -print -quit)" ]] || { echo "each_source: no sources under $path" >&2; return 1; }
  done
  export -f new_jelloc
  find "$@" -type f -name '*.jello' -print0 | sort -z |
    EACH_OUT="$out" xargs -0 -P "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -n 1 bash -c \
      'o="$EACH_OUT/${1%.jello}"; mkdir -p "${o%/*}"; '"$body" _
}
