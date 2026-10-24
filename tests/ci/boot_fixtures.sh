#!/usr/bin/env bash
# Where jello finds a program's .jdll, and boot images: a program run as an
# executable of its own, away from the VM's build, with --bundle carrying its
# libraries.
set -euo pipefail

[[ $# -eq 3 ]] || { echo "usage: boot_fixtures.sh <jlo_tool> <jello> <dir of test_native.jdll>" >&2; exit 2; }
JLO_TOOL="$1" JELLOVM="$2" NATIVE="$3/test_native.jdll"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "${TMP}"' EXIT
unset JELLO_JDLL_PATH

failures=0
fail() {
  echo "boot_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

# check NAME EXPECTED_STATUS EXPECTED_OUTPUT COMMAND...; runs in $TMP/cwd.
check() {
  local name="$1" want_status="$2" want="$3" out status
  shift 3
  out="$(cd "${TMP}/cwd" && "$@" 2>&1)" && status=0 || status=$?
  if [[ "${status}" != "${want_status}" || "${out}" != "${want}" ]]; then
    fail "${name}: expected status ${want_status} [${want}], got ${status} [${out}]"
  else
    echo "boot_fixtures: ${name}"
  fi
}

# A VM away from its build, with no library beside it, and a program that uses test_native.
mkdir -p "${TMP}/bin" "${TMP}/lib" "${TMP}/prog/jdll" "${TMP}/cwd" "${TMP}/path" "${TMP}/away"
cp "${JELLOVM}" "${TMP}/bin/jello"
VM="${TMP}/bin/jello"
"${JLO_TOOL}" asm "${ROOT}/tests/bytecode/jdll/bytes_twice.jlo.txt" -o "${TMP}/prog/twice.jlo"
PROG="${TMP}/prog/twice.jlo"
TRIED="${TMP}/prog/jdll, ${TMP}/prog, ., ${TMP}/bin, ${TMP}/lib"

check "a missing library names every directory searched" 1 \
  "jello: ${PROG}: cannot find test_native.jdll in ${TRIED} (offset 112)" "${VM}" "${PROG}"
# Each place, alone, holds it.
for place in prog/jdll prog cwd path lib; do
  cp "${NATIVE}" "${TMP}/${place}/"
  check "found in ${place}" 0 42001 env JELLO_JDLL_PATH="${TMP}/path" "${VM}" --jdll-path "${TMP}/away" "${PROG}"
  rm "${TMP}/${place}/test_native.jdll"
done
# A file that will not open, as one built for another machine, is passed over.
echo "not a library" > "${TMP}/prog/jdll/test_native.jdll"
cp "${NATIVE}" "${TMP}/prog/"
check "a library that will not open is passed over" 0 42001 "${VM}" "${PROG}"
rm "${TMP}/prog/test_native.jdll"
out="$(cd "${TMP}/cwd" && "${VM}" "${PROG}" 2>&1)" && status=0 || status=$?
if [[ "${status}" != 1 || "${out}" != "jello: ${PROG}: cannot open ${TMP}/prog/jdll/test_native.jdll: "* ]]; then
  fail "a library that will not open: expected its dlopen error, got ${status} [${out}]"
fi
rm "${TMP}/prog/jdll/test_native.jdll"
cp "${NATIVE}" "${TMP}/away/"
check "found in a --jdll-path directory" 0 42001 "${VM}" --jdll-path "${TMP}/none:${TMP}/away" "${PROG}"
check "the --jdll-path directories come after the program's" 1 \
  "jello: ${PROG}: cannot find test_native.jdll in ${TMP}/prog/jdll, ${TMP}/prog, ${TMP}/none, ., ${TMP}/bin, ${TMP}/lib (offset 112)" \
  "${VM}" --jdll-path "${TMP}/none" "${PROG}"
rm "${TMP}/away/test_native.jdll"

# A boot image needs its libraries where it runs; --bundle puts them in jdll/ beside it.
check "--boot loads the program first" 1 \
  "jello: ${PROG}: cannot find test_native.jdll in ${TRIED} (offset 112)" "${VM}" --boot "${PROG}"
cp "${NATIVE}" "${TMP}/path/"
check "--boot writes the program's executable" 0 "" "${VM}" --boot --jdll-path "${TMP}/path" "${PROG}"
mv "${TMP}/prog/twice" "${TMP}/away/twice"
check "a boot image without its library" 1 \
  "twice: twice: cannot find test_native.jdll in ${TMP}/away/jdll, ${TMP}/away, ., ${TMP}/lib (offset 112)" \
  "${TMP}/away/twice"
check "--bundle copies the libraries" 0 "" \
  "${VM}" --boot --bundle --jdll-path "${TMP}/path" -o "${TMP}/away/twice" "${PROG}"
rm "${TMP}/path/test_native.jdll"
check "a bundled boot image runs anywhere, every argument its program's" 0 42001 \
  "${TMP}/away/twice" --boot --no-jit
cmp -s "${NATIVE}" "${TMP}/away/jdll/test_native.jdll" || fail "--bundle: jdll/test_native.jdll differs"

# tls.jdll, which a program using TLS imports, is bundled as any library is.
"${JLO_TOOL}" asm "${ROOT}/tests/bytecode/boot/tls_version.jlo.txt" -o "${TMP}/prog/tls.jlo"
cp "$(dirname "${JELLOVM}")/tls.jdll" "${TMP}/path/"
check "--bundle copies tls.jdll" 0 "" \
  "${VM}" --boot --bundle --jdll-path "${TMP}/path" -o "${TMP}/away/tls" "${TMP}/prog/tls.jlo"
rm "${TMP}/path/tls.jdll"
check "a bundled program using TLS runs anywhere" 0 "Mbed TLS 3.5.1" "${TMP}/away/tls"

USAGE="usage: jello [--no-jit | --require-jit] [--jit-report] [--schedulers <n>] [--jdll-path <dirs>]
             <program.jlo> [args...]
       jello --boot [--bundle] [--jdll-path <dirs>] [-o <executable>] <program.jlo>
       jello --jit-available
       jello --version"
check "--boot without a program" 2 \
  "${USAGE}" "${VM}" --boot
check "--boot takes no program arguments" 2 \
  "${USAGE}" "${VM}" --boot "${PROG}" extra
check "--boot needs a .jlo name without -o" 2 "jello: --boot without -o needs a program named *.jlo" \
  "${VM}" --boot "${TMP}/away/twice"

if [[ ${failures} -gt 0 ]]; then
  echo "boot_fixtures: ${failures} failure(s)" >&2
  exit 1
fi
echo "boot_fixtures: passed"
