#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail
[[ $# -eq 0 ]] || { echo "usage: $0 (takes no arguments)" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VM="$ROOT/vm"
TESTS="$ROOT/tests"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A VM of its own (a temporary build) unless JELLO_VM_BUILD_DIR names one.

BUILD="${JELLO_VM_BUILD_DIR:-$TMP/build}"

# CC (word-split, e.g. "clang -arch x86_64") builds the VM and every test binary.

CC="${CC:-cc}"
export CC

section() {
  printf '\n== %s ==\n' "$1"
}

run_script() {
  local script="$1"
  shift
  section "$script"
  bash "$TESTS/ci/$script" "$@"
}

section "Build VM"
make -C "$VM" BUILD_DIR="$BUILD"

JLO="$BUILD/jlo_tool"
LIB="$BUILD/libjello_vm.a"
if [[ ! -x "$JLO" || ! -f "$LIB" ]]; then
  echo "test_vm.sh: build did not produce jlo_tool and libjello_vm.a" >&2
  exit 1
fi

section "JIT=0 build"

# Only the tools: std.jdll does not depend on the JIT. jlo_tool is built to check that it links.

NOJIT="$TMP/nojit"
make -C "$VM" JIT=0 BUILD_DIR="$NOJIT" -j4 "$NOJIT/jello" "$NOJIT/jlo_tool"
"$JLO" asm "$TESTS/bytecode/bench/fib.jlo.txt" -o "$TMP/fib.jlo"

# expect_nojit STATUS OUTPUT ARGS...: the JIT=0 jello run with ARGS gives STATUS and OUTPUT.

expect_nojit() {
  local want_status="$1" want_out="$2" out status
  shift 2
  out="$("$NOJIT/jello" "$@" 2>&1)" && status=0 || status=$?
  if [[ "$status" != "$want_status" || "$out" != "$want_out" ]]; then
    echo "test_vm.sh: the JIT=0 jello $* gave status $status [$out], not $want_status [$want_out]" >&2
    exit 1
  fi
}
expect_nojit 0 46368 "$TMP/fib.jlo"
expect_nojit 1 "jello: built without a JIT" --require-jit "$TMP/fib.jlo"
expect_nojit 1 "jello: built without a JIT" --jit-report "$TMP/fib.jlo"
echo "JIT=0 jello runs fib interpreted and refuses --require-jit and --jit-report"

section "Build test JDLL"

# A library reaches the VM through the table it is handed, so it links with nothing undefined.
# The target is the compiler's, as in the Makefile. Tests build with -D_GNU_SOURCE, as the VM
# does on Linux: glibc hides POSIX under -std=c11, and macOS ignores it.

case "$($CC -dumpmachine)" in
  *-apple-*) JDLL_LINK=(-bundle) ;;
  *-linux*) JDLL_LINK=(-fPIC -shared) ;;
  *) echo "test_vm.sh: unsupported target $($CC -dumpmachine)" >&2; exit 1 ;;
esac
for jdll in test_native test_blocking test_old_abi; do
  $CC -std=c11 -D_GNU_SOURCE -I"$VM/src/include" "${JDLL_LINK[@]}" \
    "$TESTS/jdll/$jdll.c" -o "$TMP/$jdll.jdll"
done

# Only this build's libraries: an installed std.jdll with another ABI would
# shadow the one just built.

export JELLO_JDLL_PATH="$TMP:$BUILD"
if [[ ! -f "$BUILD/std.jdll" ]]; then
  echo "test_vm.sh: build did not produce std.jdll" >&2
  exit 1
fi

# The std regexp engine must be the vendored PCRE2 10.30, not an installed one.

if ! grep -aq "10.30 2017-08-14" "$BUILD/std.jdll"; then
  echo "test_vm.sh: std.jdll is not built from PCRE2 10.30 (2017-08-14)" >&2
  exit 1
fi

# TLS must be the vendored Mbed TLS 3.5.1, in tls.jdll alone, so that a
# program without TLS does not load it.

if ! grep -aq "third_party/mbedtls-3.5.1/library/ssl_tls.c" "$BUILD/tls.jdll"; then
  echo "test_vm.sh: tls.jdll is not built with Mbed TLS 3.5.1" >&2
  exit 1
fi
if grep -aq "mbedtls" "$BUILD/jello"; then
  echo "test_vm.sh: jello links Mbed TLS, which belongs in tls.jdll" >&2
  exit 1
fi

run_script bytecode_fixtures.sh "$JLO"
run_script interp_fixtures.sh "$JLO"
run_script gc_fixtures.sh "$JLO"
run_script dyn_fixtures.sh "$JLO"
run_script jdll_fixtures.sh "$JLO"
run_script process_fixtures.sh "$JLO" "$BUILD/jello"
run_script boot_fixtures.sh "$JLO" "$BUILD/jello" "$TMP"
run_script deps_fixtures.sh "$JLO"
run_script packet_fixtures.sh "$JLO"
run_script jir_fixtures.sh "$JLO"
run_script fuel_fixtures.sh "$JLO"
run_script name_atom_roundtrip.sh "$JLO"
run_script stress_fixtures.sh "$JLO"

# Any arguments after the test's name are extra compiler flags.

build_c_test() {
  local name="$1"
  shift
  $CC -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Werror "$@" \
    -DJELLO_JDLL_HOST -I"$VM/src/include" "$TESTS/csrc/$name.c" \
    "$LIB" -lpthread -ldl -lm -o "$TMP/$name"
}

run_c_test() {
  local name="$1"
  section "$name"
  build_c_test "$@"
  "$TMP/$name"
}

run_c_test trap_pc_tests
run_c_test vm_safety_tests
run_c_test enum_tests
run_c_test thread_tests
run_c_test proc_switch_tests -O2
run_c_test proc_vm_tests
run_c_test proc_native_tests
run_c_test blocking_tests
run_c_test poll_tests
run_c_test run_for_tests
run_c_test jir_tests
run_c_test jit_frame_abi_tests
run_c_test jit_regs_tests
run_c_test jit_numeric_tests
run_c_test jit_cfg_tests
run_c_test jit_calls_tests
run_c_test jit_mixed_tests
run_c_test jit_reload_tests
run_c_test jit_arm64_tests
run_c_test jit_golden_tests
run_c_test thread_stress_tests
run_c_test capstone_stress_test

section "JIT conformance"
build_c_test jit_numeric_run
bash "$TESTS/ci/jit_numeric_conform.sh" "$JLO" "$TMP/jit_numeric_run" "$BUILD/jello"

printf '\nAll retained VM tests passed.\n'
