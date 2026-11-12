# SPDX-License-Identifier: BSD-3-Clause

# Sourced by the scripts that build the VM, after they set ROOT. The VM is a jello-vm checkout,
# JELLO_VM_DIR, by default beside this repository; VM_SRC is its CMake project, and VM_BUILD
# (JELLO_VM_BUILD_DIR, by default build/vm) where it builds.

JELLO_VM_DIR="${JELLO_VM_DIR:-$ROOT/../jello-vm}"
if [[ ! -f "$JELLO_VM_DIR/vm/CMakeLists.txt" ]]; then
  echo "$(basename "$0"): no jello-vm checkout at $JELLO_VM_DIR (set JELLO_VM_DIR)" >&2
  exit 1
fi
JELLO_VM_DIR="$(cd "$JELLO_VM_DIR" && pwd)"
VM_SRC="$JELLO_VM_DIR/vm"
VM_BUILD="${JELLO_VM_BUILD_DIR:-$ROOT/build/vm}"
[[ "$VM_BUILD" == /* ]] || VM_BUILD="$PWD/$VM_BUILD"

# The programs and libraries a VM build makes; jelloc also needs a compiler image.

VM_TARGETS=(jello jlo_tool jello_vm jello_std_jdll jello_tls_jdll)

# vm_build DIR [CMAKE_ARG...] [-- TARGET...]: configures DIR for the compiler CC (word-split, as
# "clang -arch x86_64" or "x86_64-w64-mingw32-clang") and builds the TARGETs, by default
# VM_TARGETS. It optimises as -O2 with asserts kept, and adds CFLAGS and CPPFLAGS. CMake takes the
# architecture from its own settings, never from CC's flags: -arch becomes
# CMAKE_OSX_ARCHITECTURES, and an llvm-mingw clang its toolchain file.

vm_build() {
  local dir="$1" cc_words word next_is_arch= arch= flags="-O2 ${CFLAGS:-} ${CPPFLAGS:-}"
  local -a args=() targets=()
  shift
  while [[ $# -gt 0 && "$1" != -- ]]; do args+=("$1"); shift; done
  [[ $# -gt 0 ]] && shift
  targets=("$@")
  [[ ${#targets[@]} -gt 0 ]] || targets=("${VM_TARGETS[@]}")
  read -r -a cc_words <<< "${CC:-cc}"
  for word in "${cc_words[@]:1}"; do
    if [[ -n "$next_is_arch" ]]; then arch="$word"; next_is_arch=
    elif [[ "$word" == -arch ]]; then next_is_arch=1
    else flags="$flags $word"
    fi
  done
  case "${cc_words[0]}" in
    *-w64-mingw32-clang)
      local mingw_arch
      mingw_arch="$(basename "${cc_words[0]}")"
      args+=("-DCMAKE_TOOLCHAIN_FILE=$VM_SRC/cmake/Toolchain-mingw.cmake" "-DJELLO_MINGW_ARCH=${mingw_arch%%-*}")
      ;;
    *) args+=("-DCMAKE_C_COMPILER=${cc_words[0]}") ;;
  esac
  [[ -z "$arch" ]] || args+=("-DCMAKE_OSX_ARCHITECTURES=$arch")
  cmake -S "$VM_SRC" -B "$dir" -Wno-deprecated -DCMAKE_BUILD_TYPE= "-DCMAKE_C_FLAGS=$flags" \
    "${args[@]}" > /dev/null
  cmake --build "$dir" --parallel --target "${targets[@]}"
}
