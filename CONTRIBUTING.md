# Contributing to the Project

Thank you for your interest in contributing! There are likely many bugs in this codebase that need fixing, and many features that need adding, so contributions from interested devs is most welcome. However, to keep the codebase robust, maintainable, and clean, please follow the guidelines outlined below.

I'm a stickler for orderliness, so please do your best :-)

---

## 1. Code Style and Formatting

All code must be properly formatted before submitting a pull request. We enforce strict code style standards to ensure consistency across the virtual machine and compiler subsystems.

* **Code Tidying:** Use **ClangFormat** for all C code formatting in the VM repo (see rules there). A `.clang-format` configuration file is provided in the root directory. Ensure your changes adhere to this configuration before committing.
* **Jello Formatting:** Format Jello code with `jelloc fmt`, run from the repository root; the `.formatter` there names what it formats and how. `jelloc fmt --check` lists any file that still needs it.
* **Comments:** File headers and function descriptions are written in **Doxygen format** (eg. `/** ... */` with tags like `@param` and `@returns`). A comment that fits on one line is a `//` comment, and a comment of more lines is a `/* ... */` or `/** ... */` block, as `jelloc fmt` writes them.
* **Licensing Headers:** All new modules, header files, and source files must include the standard SPDX license identifier as the very first line:
```c
// SPDX-License-Identifier: BSD-3-Clause
```
Or second line if it's a Bash script, etc.

* **English & Documentation:** Whether writing documentation, commit messages, code comments, or pull request descriptions, ensure your English is clear and polished. I'm not great at understanding broken English, and my country's education system let me down by not making sure I learned any other languages, so blame my Government (or parents).

---

## 2. Compiler & VM Specific Guidelines

Because this project is a self-hosted compiler and virtual machine (VM repo), code changes can have cascading effects on code generation, parsing, and execution integrity.

* **Bootstrap Verification:** Since the compiler is self-hosted, any modification to the compiler front-end, intermediate representation (IR), or code generator must successfully pass the bootstrap cycle. Your changes must compile cleanly using the existing compiler binary and be able to re-compile the source tree without regression. See the `scripts` folder in the VM repo.
* **Memory Safety & Standards:**
* Write standard-compliant C (targeting C99/C11). Avoid compiler-specific extensions unless strictly necessary and properly guarded with preprocessor macros.
* Manage memory rigorously. Ensure all allocated buffers are properly freed, and run tests under Valgrind or AddressSanitizer (ASan) to check for memory leaks or buffer overflows before opening a PR.


* **Versions:** The release's version is written in one place, the `version` of `jelloc/jello.json`. The VM's build takes it from there for `jello --version` and `std.sys.Platform`'s `version()`, which `jelloc --version` prints, and `releases/package.sh` names the release after it.
* **Test Suite:** Always add or update unit/integration tests for any bug fix or feature addition. Run the full test suite locally to verify that the VM bytecode execution and compiler outputs match expected results.

---

## 3. Pull Request Workflow

1. **Fork and Clone:** Fork the repository on GitHub and clone your fork locally.
2. **Create a Branch:** Create a descriptive feature branch (eg. `feature/parser-optimizations` or `fix/vm-stack-overflow`).
3. **Commit:** Make atomic, well-documented commits. Ensure commit messages are written in clear English and describe *why* the change was made.
4. **Check Formatting & Tests:**
* Run ClangFormat on all modified C files, and `jelloc fmt` on Jello files.
* Verify Doxygen comments are complete.
* Run the test suite and verify bootstrap integrity.
5. **Open a Pull Request:** Submit your PR against the `master` branch. Address any review feedback promptly.

Don't go by my past examples; this is a moving forward declaration :-)
