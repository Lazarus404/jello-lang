#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

# Builds the VM and the compiler (./scripts/build_vm.sh, ./scripts/build_jello.sh) for the
# system the compiler targets and lays out a release in OUT, by default releases/<os> (macos,
# linux or windows). A macOS release is universal: its binaries hold an arm64 and an x86_64
# build. A Windows release (CC an llvm-mingw clang) builds on Windows or, from another system,
# whose compiler (HOST_CC, by default cc) builds the VM that compiles the compiler module; it
# interprets, without the JIT, and must import only the system's own DLLs.
#   bin/       jello, the VM; jelloc, the compiler as a native program; and
#              jlo_tool, which fetches dependencies and inspects modules
#   lib/       std.jdll and tls.jdll, the VM's native libraries, and
#              libjello_vm.a, for programs that embed the VM
#   include/   jello.h and jello/jdll.h, and on Windows jello.manifest, which an embedding
#              program carries as Jello's do
#   std/       the standard library's sources
#   licenses/  Jello's licence and those of the vendored libraries, and on Windows of the
#              MinGW-w64 runtime and winpthreads, which its binaries link
#   VERSION    the compiler's version, the commit it was built from, the OS and the architecture
#   install.sh, or install.ps1 on Windows, and README.md, which install and describe it

set -euo pipefail
[[ $# -le 1 && "${1:-}" != -* ]] || { echo "usage: $0 [out-dir]" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$ROOT/vm/build"

# The system and architecture the compiler builds for, as the Makefile takes them.

target="$(${CC:-cc} -dumpmachine)"
case "$target" in
  *-apple-*) os=macos ;;
  *-linux*) os=linux ;;
  *-windows-*) os=windows ;;
  *) echo "package.sh: cannot package for $target" >&2; exit 1 ;;
esac
case "$target" in
  arm64* | aarch64*) arch=arm64 ;;
  x86_64* | amd64*) arch=x64 ;;
  *) echo "package.sh: cannot package for $target" >&2; exit 1 ;;
esac
OUT="${1:-$HERE/$os}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# Elsewhere than on Windows, a Windows release's compiler module is built by this system's VM.

build_cc="${CC:-cc}"
if [[ "$os" == windows && "$(uname -s)" != MINGW* && "$(uname -s)" != MSYS* ]]; then
  build_cc="${HOST_CC:-cc}"
fi
CC="$build_cc" "$ROOT/scripts/build_vm.sh" > /dev/null
CC="$build_cc" "$ROOT/scripts/build_jello.sh" > /dev/null
exe=
installer=install.sh
if [[ "$os" == macos ]]; then
  # Each architecture built afresh in its own directory, embedding the same compiler module,
  # for macOS 11, the first on Apple silicon, rather than the building Mac's version; then
  # joined by lipo into one binary of each.
  arch=universal
  UNIVERSAL="$BUILD/universal"
  export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
  rm -rf "$UNIVERSAL"
  for slice in arm64 x86_64; do
    make -s -C "$ROOT/vm" BUILD_DIR="$UNIVERSAL/$slice" CC="${CC:-cc} -arch $slice" all jelloc \
      > /dev/null
  done
  for file in jello jelloc jlo_tool std.jdll tls.jdll libjello_vm.a; do
    lipo -create "$UNIVERSAL/arm64/$file" "$UNIVERSAL/x86_64/$file" -output "$UNIVERSAL/$file"
  done
  BUILD="$UNIVERSAL"
elif [[ "$os" == windows ]]; then
  exe=.exe
  installer=install.ps1
  WINDOWS="$BUILD/windows-$arch"
  rm -rf "$WINDOWS"
  make -s -C "$ROOT/vm" BUILD_DIR="$WINDOWS" CC="$CC" AR="${AR:-llvm-ar}" all jelloc > /dev/null
  BUILD="$WINDOWS"
  # Windows' own DLLs and its C runtime's, which every Windows 10 has; nothing to ship beside.
  for file in jello.exe jelloc.exe jlo_tool.exe std.jdll tls.jdll; do
    if llvm-objdump -p "$BUILD/$file" | sed -n 's/^ *DLL Name: //p' |
      grep -viE '^(api-ms-win-crt-[a-z]+-l1-1-0|(kernel|ws2_|advapi|crypt)32|bcrypt)\.dll$'; then
      echo "package.sh: $file imports the DLLs above, which Windows does not have" >&2
      exit 1
    fi
  done
fi

rm -rf "$OUT/bin" "$OUT/lib" "$OUT/include" "$OUT/std" "$OUT/licenses" "$OUT/install.sh" \
  "$OUT/install.ps1"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/include/jello" "$OUT/std" "$OUT/licenses"
install -m 755 "$BUILD/jello$exe" "$BUILD/jelloc$exe" "$BUILD/jlo_tool$exe" "$OUT/bin/"
install -m 755 "$BUILD/std.jdll" "$BUILD/tls.jdll" "$OUT/lib/"
install -m 644 "$BUILD/libjello_vm.a" "$OUT/lib/"
install -m 644 "$ROOT/vm/src/include/jello.h" "$OUT/include/"
install -m 644 "$ROOT/vm/src/include/jello/jdll.h" "$OUT/include/jello/"
if [[ "$os" == windows ]]; then
  install -m 644 "$ROOT/vm/tools/jello.manifest" "$OUT/include/"
fi
cp -R "$ROOT/stdlib/." "$OUT/std/"
find "$OUT/std" -name .DS_Store -delete
install -m 644 "$ROOT/LICENSE" "$OUT/licenses/jello.txt"
install -m 644 "$ROOT/vm/third_party/mbedtls-3.5.1/LICENSE" "$OUT/licenses/mbedtls-3.5.1.txt"
install -m 644 "$ROOT/vm/third_party/pcre2-10.30/LICENCE" "$OUT/licenses/pcre2-10.30.txt"
if [[ "$os" == windows ]]; then
  install -m 644 "$ROOT/vm/third_party/wepoll-1.5.8/LICENSE" "$OUT/licenses/wepoll-1.5.8.txt"
  # The binaries link the MinGW-w64 runtime and winpthreads statically, whose notices the
  # toolchain keeps in share/mingw32 beside the target's lib.
  mingw="$(dirname "$($CC -print-file-name=libwinpthread.a)")/../share/mingw32"
  install -m 644 "$mingw/COPYING.MinGW-w64-runtime.txt" "$OUT/licenses/mingw-w64-runtime.txt"
  install -m 644 "$mingw/COPYING.winpthreads.txt" "$OUT/licenses/winpthreads.txt"
fi
install -m 755 "$HERE/$installer" "$OUT/"
install -m 644 "$HERE/README.md" "$OUT/"

version="$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' "$ROOT/jelloc/jello.json")"
[[ -n "$version" ]] || { echo "package.sh: no version in jelloc/jello.json" >&2; exit 1; }
commit="$(git -C "$ROOT" rev-parse --short HEAD)"
git -C "$ROOT" diff --quiet HEAD -- jelloc/src jelloc/boot stdlib vm/src vm/tls vm/tools ':!*.DS_Store' ||
  commit="$commit+dirty"
printf '%s %s %s %s\n' "$version" "$commit" "$os" "$arch" > "$OUT/VERSION"

# A release must not depend on where it was built: no binary names a path under the build
# directory (a format such as "%s/src" is not one, though a short ROOT can look like it).

if strings "$OUT"/bin/* "$OUT"/lib/*.jdll | grep -F "$ROOT/" | grep -vF "%s$ROOT/" > /dev/null; then
  echo "package.sh: a binary names the build directory $ROOT" >&2
  exit 1
fi
printf 'Packaged Jello %s (%s, %s %s) in %s\n' "$version" "$commit" "$os" "$arch" "$OUT"
