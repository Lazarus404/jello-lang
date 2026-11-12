# Platforms

Jello runs on these systems, with the JIT on each of them. A program and its `.jlo` modules are
the same on all of them: a module compiled on one runs on any other.

| System | Architectures | Needs |
|--------|---------------|-------|
| macOS | arm64 (Apple silicon) and x64 (Intel), one release each and a universal one of both | macOS 11 or later |
| Linux | arm64 and x64, one release each | glibc 2.34 or later: Ubuntu 22.04, Debian 12, RHEL 9 and later |
| Windows | arm64 and x64, one release each | Windows 10 version 1903 or later, or Windows 11 |

A release is an archive, `jello-<version>-<system>-<architecture>.tar.gz`, or `.zip` for
Windows, from the repository's GitHub releases, with `SHA256SUMS` beside it. Unpacked, it installs
for the current user with its `install.sh`, or on Windows its `install.ps1`, which puts the tools
on the user's `PATH` (see its `README.md`).

## Differences

The systems differ only where the system itself does.

- **Paths.** Paths use `/`, on Windows too, where the natives also take `\` and give back `/`; a
  Windows path starts with a drive (`C:/`) or a share (`//server/share/`). Paths, arguments and
  the environment are UTF-8 everywhere. `std.sys.Platform` names the system (`os()`, `"macos"`,
  `"linux"` or `"windows"`), the architecture (`arch()`, `"arm64"` or `"x64"`) and the directory
  for temporary files (`tempDir()`: `TMPDIR`, then on Windows `TEMP` or `TMP`, else `/tmp`). The
  names `/dev/stdout`, `/dev/stderr` and `/dev/null` mean the same on every system.
- **Lists of directories.** `JELLO_JDLL_PATH` and `SSL_CERT_DIR` separate directories with `:`,
  or `;` on Windows, as `PATH` does.
- **Programs.** `std.io.Command` finds a program named without a directory on `PATH`; on Windows
  it also tries the extensions `PATHEXT` lists, never looks in the current directory first, and
  runs a `.bat` or `.cmd` file through `cmd.exe`, refusing an argument holding `"`, `%` or a line
  break, which `cmd.exe` would read as its own syntax. A program's executable is `name.exe` on
  Windows, and `jello --boot` writes one.
- **Exit statuses.** A program ended by a signal exits with 128 plus the signal's number, as a
  shell reports it. On Windows a program that crashes reports as POSIX would: an access violation
  139 (`SIGSEGV`), Ctrl-C 130 (`SIGINT`) and any other crash 134 (`SIGABRT`); other statuses are
  taken modulo 256.
- **Processors.** `cpuCount()` and the default number of schedulers count the processors the
  program may use, which on Linux is its affinity mask: a container given a set of processors
  uses that many.
- **Certificates.** A TLS client trusts the system's certificates: those of `SSL_CERT_FILE` and
  `SSL_CERT_DIR` when either is set, else the system's bundle file, or on Windows the certificates
  in its trusted root store, which holds only those the system has fetched so far (see
  [std.net](../06-stdlib/05-net.md)).
- **Mathematics.** `Math` uses the system's C library, so a function such as `Math.sin` or
  `Math.exp` can differ in its last digit between systems. What IEEE 754 fixes, such as
  arithmetic, `Math.sqrt` and rounding, is the same everywhere, as is the cube root of an exact
  cube.
- **Sockets.** On Windows the poller watches sockets only, so the `proc_poll_*` natives a
  `.jdll` uses take sockets there, not other handles.
- **Dependencies.** On Windows `jlo_tool get-deps` links a path dependency into `packages/` as a
  directory junction, which needs no privilege, where other systems use a symbolic link.
- **Native libraries.** A `.jdll` is built for one system: on macOS a bundle (`-bundle`), which
  may be universal, on Linux a shared object for one architecture (`-shared -fPIC`), and on
  Windows a DLL built with llvm-mingw (`-shared`). See [jello](02-jello.md#native-libraries).
- **Embedding.** A program embedding the VM on Windows needs the manifest Jello's own programs
  carry, `include/jello.manifest` in a release, so that the system gives it UTF-8 paths and
  arguments.
- **Debugging.** On Windows a debugger, or a crash dump, cannot follow the stack back through a
  function the JIT compiled, as the system has no description of its frames; run the program with
  `jello --no-jit` to see the whole stack.
