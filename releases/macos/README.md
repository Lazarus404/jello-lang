# Jello

A release of Jello for one system and architecture, which `VERSION` names after the version and
the commit it was built from: `macos`, `linux` or `windows` and `arm64` or `x64`, or `macos` and
`universal`, for both Mac architectures. It installs without building anything; `package.sh`, in the
repository's `releases/`, builds one.

| Path | Contents |
|------|----------|
| `bin/jello` | the virtual machine, which runs a compiled `.jlo` module (`jello.exe` on Windows) |
| `bin/jelloc` | the compiler, with `jelloc run`, `jelloc test` and `jelloc deps.get` |
| `bin/jlo_tool` | fetches a project's dependencies, and inspects modules |
| `lib/std.jdll` | the standard library's natives, which `jello` finds in `../lib` |
| `lib/tls.jdll` | TLS, on Mbed TLS, which only a program using `std.net.Tls` loads |
| `lib/libjello_vm.a` | the VM, for a C program that embeds it |
| `include/` | `jello.h`, the embedding API, and `jello/jdll.h`, for native libraries; on Windows `jello.manifest`, which an embedding program carries |
| `std/` | the standard library's sources, which `jelloc` compiles with a program |
| `licenses/` | Jello's licence, BSD 3-Clause, and those of Mbed TLS, in `tls.jdll`, and PCRE2, in `std.jdll`; on Windows also wepoll's, and the MinGW-w64 runtime's and winpthreads', which every binary links |

## Installing on macOS or Linux

```sh
./install.sh     # install this release for the current user
```

`install.sh` copies the release to `~/.jello/sdk/<version>`, points `~/.jello/current` at it, and
writes `~/.config/jello/env.sh`, which puts `~/.jello/current/bin` on `PATH` and sets
`JELLO_ROOT` and `JELLO_STD_PATH`, where `jelloc` finds the standard library. It sources that
file from `~/.zprofile` and `~/.zshrc` on macOS, and `~/.bashrc` and `~/.profile` on Linux, and
from any of those, `~/.bash_profile` among them, that exist, in a block between `# >>> jello >>>` and `# <<< jello <<<`, which replaces an older Jello
installer's. A profile it first changes is kept beside it as `<profile>.jello-backup`. It then
compiles and runs a program with the installed tools.

| Option | Meaning |
|--------|---------|
| `--prefix dir` | install under `dir` instead of `~/.jello` |
| `--no-profile` | write `env.sh`, but change no shell profile |
| `--uninstall` | remove this version; when none is left in use, remove `env.sh` and the profile blocks too |

Open a new terminal afterwards, or run `. ~/.config/jello/env.sh`.

## Installing on Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1    # install this release for the current user
```

`install.ps1` copies the release to `%LOCALAPPDATA%\jello\sdk\<version>`, points the directory
junction `%LOCALAPPDATA%\jello\current` at it, and sets the user's environment: its `bin` first on
`PATH`, and `JELLO_ROOT` and `JELLO_STD_PATH`. It then compiles and runs a program with the
installed tools.

| Option | Meaning |
|--------|---------|
| `-Prefix dir` | install under `dir` instead of `%LOCALAPPDATA%\jello` |
| `-NoPath` | set `JELLO_ROOT` and `JELLO_STD_PATH`, but leave `PATH` alone |
| `-Uninstall` | remove this version; when none is left in use, remove what it set in the environment too |

Open a new terminal afterwards.

## Using it

```sh
jelloc hello.jello --out hello.jlo && jello hello.jlo
jelloc run hello.jello
```

## Embedding the VM

```sh
cc -I"$JELLO_ROOT/include" main.c "$JELLO_ROOT/lib/libjello_vm.a" -lpthread -lm  # macOS
cc -I"$JELLO_ROOT/include" main.c "$JELLO_ROOT/lib/libjello_vm.a" -lpthread -ldl -lm  # Linux
```

On Windows, built with llvm-mingw, the program also carries `include/jello.manifest`, which gives
it UTF-8 paths and arguments, compiled from a resource file such as `main.rc` holding
`1 24 "jello.manifest"`:

```sh
windres -I"$JELLO_ROOT/include" -i main.rc -o main-rc.o
clang -I"$JELLO_ROOT/include" main.c main-rc.o "$JELLO_ROOT/lib/libjello_vm.a" \
  -static -lpthread -lws2_32 -lbcrypt
```

The program finds the native libraries as `jello` does: beside it, in `JELLO_JDLL_PATH`, or in
`../lib` from its directory.
