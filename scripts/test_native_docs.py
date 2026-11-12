#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Runs the native-library examples of the language reference, which Docs.test passes over.

A block fenced as ```c native=NAME is C of the library NAME; a page's blocks of one library are
joined in order, so a page can build one up piece by piece, and the library is built as
NAME.jdll. A block fenced as ```jello native=NAME is a program using it, compiled and run with
`jelloc run`, the library in jdll/ beside it; one fenced as ```jello native=NAME file=PATH is a
module of the program that follows it, written at PATH beside it. A program's leading comment
lines say what it does:

    // expect: LINE    a line of its standard output, in order; it exits 0
    // stderr: TEXT    text its standard error holds; it exits non-zero

The VM runs one scheduler, so that what a page says of blocking calls holds even then.

    scripts/test_native_docs.py [page.md...]

With no pages, every page under docs/. CC is the C compiler (cc by default); JELLO_VM_DIR and
JELLO_VM_BUILD_DIR are the VM's checkout and build, as build_vm.sh has them. A fence that starts
as a native one but does not parse, such as ```c native=my-lib, fails rather than going untested.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VM_SRC = Path(os.environ.get("JELLO_VM_DIR", ROOT.parent / "jello-vm")).resolve() / "vm"
FENCE = re.compile(r"^```(c|jello) native=([A-Za-z_][A-Za-z0-9_]*)(?: file=(\S+))?$")


def fail(message):
    sys.exit(f"test_native_docs.py: {message}")


def blocks(page):
    """Each native block of `page`: (language, library, file or None, first line, text)."""
    lines = page.read_text().split("\n")
    found = []
    i = 0
    while i < len(lines):
        match = FENCE.match(lines[i])
        if not match and re.match(r"^```(c|jello) native", lines[i]):
            fail(f"{page}:{i + 1}: a native fence is ```c native=NAME or ```jello native=NAME,"
                 " with an optional file=PATH, NAME a C identifier")
        if match:
            start = i + 1
            body = []
            i += 1
            while i < len(lines) and lines[i] != "```":
                body.append(lines[i])
                i += 1
            if i == len(lines):
                fail(f"{page}:{start}: a native block is not closed")
            language, library, file = match.groups()
            if file and (language != "jello" or file.startswith("/") or ".." in file):
                fail(f"{page}:{start}: file= names a Jello module inside its example")
            found.append((language, library, file, start, "\n".join(body) + "\n"))
        i += 1
    return found


def link_flags(cc):
    """The flags that make a library for `cc`'s target, and the libraries it links after."""
    machine = subprocess.run([*cc, "-dumpmachine"], capture_output=True, text=True).stdout
    if "-apple-" in machine:
        return ["-bundle"], []
    if "-linux" in machine:
        return ["-fPIC", "-shared"], ["-lm"]
    if "mingw" in machine or "windows" in machine:
        return ["-shared"], []
    fail(f"no shared-library flags for {machine.strip()}")


def build(cc, library, source, out):
    """Builds `library` from `source` into directory `out`, warnings as errors."""
    c_file = out / f"{library}.c"
    c_file.write_text(source)
    flags, libraries = link_flags(cc)
    command = [*cc, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
               f"-I{VM_SRC / 'src/include'}", *flags, str(c_file), *libraries,
               "-o", str(out / f"{library}.jdll")]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"library {library} does not build:\n{result.stderr}")


def check(where, jelloc, directory, program):
    """Runs `program`, main.jello in `directory`, and holds it to its directives."""
    directives = []
    for line in program.split("\n"):
        if not line.startswith("//"):
            break
        match = re.match(r"^// (expect|stderr): ?(.*)$", line)
        if match:
            directives.append(match.groups())
    if not directives:
        fail(f"{where}: an example says neither what it prints (// expect:) nor its error"
             " (// stderr:)")
    env = {k: v for k, v in os.environ.items() if k not in ("JELLO_JDLL_PATH", "JELLO_JIT_DISABLE")}
    env["JELLO_SCHEDULERS"] = "1"
    result = subprocess.run([jelloc, "run", "--std", str(ROOT / "stdlib"), "main.jello"],
                            cwd=directory, capture_output=True, text=True, env=env, timeout=120)
    expected = [text for kind, text in directives if kind == "expect"]
    errors = [text for kind, text in directives if kind == "stderr"]
    if errors:
        if result.returncode == 0:
            fail(f"{where}: exits 0, but should fail with {errors[0]!r}")
        for text in errors:
            if text not in result.stderr:
                fail(f"{where}: standard error is {result.stderr!r}, without {text!r}")
    elif result.returncode != 0:
        fail(f"{where}: exits {result.returncode}: {result.stderr}")
    if result.stdout.split("\n")[:-1] != expected:
        fail(f"{where}: printed {result.stdout!r}, not {expected!r}")


def main():
    pages = [Path(p).resolve() for p in sys.argv[1:]] or sorted((ROOT / "docs").rglob("*.md"))
    cc = shlex.split(os.environ.get("CC", "cc"))
    jelloc = str(Path(os.environ.get("JELLO_VM_BUILD_DIR", ROOT / "build/vm")).resolve() / "jelloc")
    examples = 0
    with tempfile.TemporaryDirectory() as tmp:
        for page in pages:
            found = blocks(page)
            if not found:
                continue
            name = page.relative_to(ROOT).as_posix().replace("/", "_")
            sources = {}
            for language, library, _, _, text in found:
                if language == "c":
                    sources[library] = sources.get(library, "") + text
            built = Path(tmp) / name / "libraries"
            built.mkdir(parents=True)
            for library, source in sources.items():
                build(cc, library, source, built)
            modules = []
            for language, library, file, start, text in found:
                where = f"{page}:{start}"
                if language == "c":
                    continue
                if library not in sources:
                    fail(f"{where}: the page has no C for library {library}")
                if file:
                    modules.append((file, text))
                    continue
                directory = Path(tmp) / name / f"line{start}"
                (directory / "jdll").mkdir(parents=True)
                (directory / "main.jello").write_text(text)
                for file_name, source in modules:
                    (directory / file_name).parent.mkdir(parents=True, exist_ok=True)
                    (directory / file_name).write_text(source)
                modules = []
                for library_file in built.glob("*.jdll"):
                    (directory / "jdll" / library_file.name).write_bytes(library_file.read_bytes())
                check(where, jelloc, directory, text)
                examples += 1
            if modules:
                fail(f"{page}: a module block is followed by no example")
    print(f"== {examples} native-library examples ==")


if __name__ == "__main__":
    main()
