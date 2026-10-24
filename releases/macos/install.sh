#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Installs the Jello release beside this script (package.sh lays it out) for
# the current user, on the macOS or Linux machine it was built for.
#
#   <prefix>/sdk/<version>/   bin, lib, include, std and licenses
#   <prefix>/current          the version in use
#   ~/.config/jello/env.sh    puts <prefix>/current/bin on PATH and sets
#                             JELLO_ROOT and JELLO_STD_PATH
#
# Each shell profile that zsh, bash or sh reads at start-up sources env.sh, from a
# block between "# >>> jello >>>" and "# <<< jello <<<", which the installer
# rewrites and the uninstaller removes. Older Jello installers' blocks, and
# the jello-vm block and its vm-env.sh, are replaced.
set -euo pipefail

usage() {
  cat <<'EOF'
usage: install.sh [--prefix dir] [--no-profile]
       install.sh --uninstall [--prefix dir]

  --prefix dir   install under dir (default ~/.jello)
  --no-profile   write env.sh, but change no shell profile
  --uninstall    remove this version, and, when no version is left in use,
                 env.sh and the profile blocks
EOF
}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Run from releases/ itself, it installs the package package.sh made there for this OS.
if [[ ! -f "$HERE/VERSION" ]]; then
  case "$(uname -s)" in
    Darwin) PACKAGED="$HERE/macos" ;;
    Linux) PACKAGED="$HERE/linux" ;;
    *) PACKAGED="" ;;
  esac
  if [[ -n "$PACKAGED" && -f "$PACKAGED/VERSION" && -f "$PACKAGED/install.sh" ]]; then
    exec bash "$PACKAGED/install.sh" "$@"
  fi
fi
PREFIX="$HOME/.jello"
PROFILE=1
UNINSTALL=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; PREFIX="$2"; shift 2 ;;
    --no-profile) PROFILE=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
  esac
done
case "$(uname -s)" in
  Darwin) OS=macos ;;
  Linux) OS=linux ;;
  *) OS="$(uname -s)" ;;
esac
case "$(uname -m)" in
  arm64 | aarch64) MACHINE=arm64 ;;
  x86_64 | amd64) MACHINE=x64 ;;
  *) MACHINE="$(uname -m)" ;;
esac
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}/jello"
ENV_FILE="$CONFIG/env.sh"
BEGIN="# >>> jello >>>"
END="# <<< jello <<<"
PROFILES=("$HOME/.zprofile" "$HOME/.zshrc" "$HOME/.bash_profile" "$HOME/.bashrc" "$HOME/.profile")

[[ -f "$HERE/VERSION" ]] || { echo "install.sh: no VERSION beside it (run package.sh)" >&2; exit 1; }
read -r VERSION COMMIT RELEASE_OS ARCH < "$HERE/VERSION"
TARGET="$PREFIX/sdk/$VERSION"

# Removes the jello and jello-vm blocks from PROFILE, keeping a copy of it
# beside it the first time.
strip_blocks() {
  local profile="$1"
  [[ -f "$profile" ]] || return 0
  grep -Eq '^# >>> jello(-vm)? >>>$' "$profile" || return 0
  [[ -f "$profile.jello-backup" ]] || cp -p "$profile" "$profile.jello-backup"
  # A block's blank line before it goes with it, so installing again adds none.
  awk '
    /^# >>> jello(-vm)? >>>$/ { skip = 1; held = ""; next }
    skip && /^# <<< jello(-vm)? <<<$/ { skip = 0; next }
    skip { next }
    /^$/ { held = held "\n"; next }
    { printf "%s", held; held = ""; print }
    END { printf "%s", held }
  ' "$profile" > "$profile.jello-tmp"
  cat "$profile.jello-tmp" > "$profile"
  rm -f "$profile.jello-tmp"
}

if [[ "$UNINSTALL" -eq 1 ]]; then
  rm -rf "$TARGET"
  if [[ -L "$PREFIX/current" && "$(readlink "$PREFIX/current")" == "$TARGET" ]]; then
    rm -f "$PREFIX/current"
  fi
  rmdir "$PREFIX/sdk" "$PREFIX" 2>/dev/null || true
  # env.sh and the profile blocks go only when env.sh is this prefix's, not another install's.
  if [[ ! -e "$PREFIX/current" ]] \
      && { [[ ! -f "$ENV_FILE" ]] || grep -qxF "export JELLO_ROOT=\"$PREFIX/current\"" "$ENV_FILE"; }; then
    rm -f "$ENV_FILE" "$CONFIG/vm-env.sh"
    rmdir "$CONFIG" 2>/dev/null || true
    for profile in "${PROFILES[@]}"; do strip_blocks "$profile"; done
  fi
  echo "Uninstalled Jello $VERSION from $PREFIX."
  exit 0
fi

for item in bin/jello bin/jelloc bin/jlo_tool lib/std.jdll lib/tls.jdll lib/libjello_vm.a \
    include/jello.h include/jello/jdll.h std/core/Array.jello; do
  [[ -f "$HERE/$item" ]] || { echo "install.sh: the release has no $item (run package.sh)" >&2; exit 1; }
done
# A universal macOS release runs on either architecture.
[[ "$RELEASE_OS $ARCH" == "$OS $MACHINE" || "$RELEASE_OS $ARCH" == "$OS universal" ]] ||
  { echo "install.sh: this release is for $RELEASE_OS $ARCH, not $OS $MACHINE" >&2; exit 1; }

echo "Installing Jello $VERSION ($COMMIT) in $TARGET"
mkdir -p "$PREFIX/sdk"
rm -rf "$TARGET.new"
mkdir "$TARGET.new"
cp -R "$HERE/bin" "$HERE/lib" "$HERE/include" "$HERE/std" "$HERE/licenses" "$HERE/VERSION" \
  "$TARGET.new/"
[[ -f "$HERE/README.md" ]] && cp "$HERE/README.md" "$TARGET.new/"
# On macOS a release downloaded by a browser is quarantined, and Gatekeeper
# would stop its unnotarised binaries; they are signed ad hoc, which is re-done
# if the copy broke the signature.
if [[ "$OS" == macos ]]; then
  xattr -dr com.apple.quarantine "$TARGET.new" 2>/dev/null || true
  for binary in "$TARGET.new"/bin/* "$TARGET.new"/lib/*.jdll; do
    codesign --verify "$binary" 2>/dev/null || codesign --force --sign - "$binary"
  done
fi
rm -rf "$TARGET"
mv "$TARGET.new" "$TARGET"
ln -sfn "$TARGET" "$PREFIX/current"

mkdir -p "$CONFIG"
rm -f "$CONFIG/vm-env.sh"
cat > "$ENV_FILE" <<EOF
# Jello, written by install.sh: the tools on PATH, and where jelloc finds the
# standard library.
export JELLO_ROOT="$PREFIX/current"
case ":\${PATH}:" in
  *":\${JELLO_ROOT}/bin:"*) ;;
  *) export PATH="\${JELLO_ROOT}/bin:\${PATH}" ;;
esac
export JELLO_STD_PATH="\${JELLO_ROOT}/std"
EOF
chmod 644 "$ENV_FILE"

if [[ "$PROFILE" -eq 1 ]]; then
  # zsh, macOS's shell, reads .zprofile for a login shell and .zshrc for an
  # interactive one; on Linux, bash reads .bashrc, and a login shell .profile.
  # Other profiles get the block only when they exist.
  if [[ "$OS" == macos ]]; then
    touch "$HOME/.zprofile" "$HOME/.zshrc"
  else
    touch "$HOME/.bashrc"
    [[ -f "$HOME/.bash_profile" || -f "$HOME/.bash_login" ]] || touch "$HOME/.profile"
  fi
  for profile in "${PROFILES[@]}"; do
    [[ -f "$profile" ]] || continue
    strip_blocks "$profile"
    [[ -s "$profile" ]] && echo >> "$profile"
    printf '%s\n[ -f "%s" ] && . "%s"\n%s\n' "$BEGIN" "$ENV_FILE" "$ENV_FILE" "$END" >> "$profile"
  done
fi

# The installed tools, from a shell with nothing but env.sh.
CHECK="$(mktemp -d)"
trap 'rm -rf "$CHECK"' EXIT
printf 'let who = "Jello";\n"hello from " + who\n' > "$CHECK/hello.jello"
result="$(cd "$CHECK" && env -i HOME="$HOME" PATH=/usr/bin:/bin TMPDIR="$CHECK" \
  bash -c '. "$1" && jelloc hello.jello --out hello.jlo && jello hello.jlo && jelloc run hello.jello' \
  check "$ENV_FILE")" || { echo "install.sh: the installed tools do not run" >&2; exit 1; }
[[ "$result" == $'hello from Jello\nhello from Jello' ]] \
  || { echo "install.sh: the installed tools printed: $result" >&2; exit 1; }

echo "Installed Jello $VERSION: jello, jelloc and jlo_tool in $PREFIX/current/bin."
if [[ "$PROFILE" -eq 1 ]]; then
  echo "Open a new terminal, or run: . $ENV_FILE"
else
  echo "Source $ENV_FILE from your shell profile to use them."
fi
