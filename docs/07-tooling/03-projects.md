# Projects

A project is a directory holding a `jello.json` manifest. It names where the project's sources and
tests are and what packages it depends on, so that `jelloc run`, `jelloc test` and
`jelloc deps.get` can find them without `--path` flags.

```text
hello/
  jello.json
  .formatter          jelloc fmt's settings, if any
  jello.lock          written by deps.get
  src/
    main.jello
    app/Names.jello
  tests/
    app/Names.test.jello
  packages/           written by deps.get
    greet/
  jdll/               native libraries, if any
  build/              written by run and test
```

```text
{
  "name": "hello",
  "version": "0.1.0",
  "dependencies": {
    "greet": "1.2.0"
  }
}
```

A plain `jelloc main.jello --out main.jlo` never reads a manifest; the subcommands do (see
[jelloc](01-jelloc.md#subcommands)), and so does a compile that leaves out its input or `--out`
(see [Defaults](01-jelloc.md#defaults)).

## Making a project

`jlo_tool init [dir] [--name name]` makes a project in `dir`, by default the current directory:
a `jello.json` with the name (by default the directory's), version `0.1.0` and no dependencies, a
`src/Main.jello` that returns `"Hello, world!"`, and a `.gitignore` for `build/` and `packages/`.
It does not overwrite an existing `jello.json`.

```sh
jlo_tool init hello
cd hello
jelloc run
```

## The manifest

`jello.json` holds one JSON object. The compiler reads these keys and ignores any others:

| Key | Value | Default |
|-----|-------|---------|
| `name` | the project's name, which names the module `jelloc` writes to `build/` | the directory's name |
| `entry` | the entry module, which `jelloc` and `jelloc run` compile when given no file | `Main.jello` in `src` |
| `src` | the source directory, a root for imports | `"src"` |
| `test` | an object whose `dirs` is an array of test directories | `{"dirs": ["tests"]}` |
| `dependencies` | an object naming each package the project uses (below) | none |
| `dev_dependencies` | packages only the project's own tests use, named as in `dependencies` | none |
| `package_paths` | an array of directories that also hold packages, as `<dir>/<name>/` | none |
| `jdll` | the directory of the project's native libraries | `"jdll"` |

`deps.get` also needs `name`, and a package's manifest needs `name` and `version` (see
[Dependencies](#dependencies)). Paths are relative to the project's directory.

A manifest that is not valid JSON, is not an object, or gives a key a value of the wrong kind is
an error located in the manifest itself:

```text
jello.json:1:27: "src" must be a string [E0211]
{ "name": "hello", "src": 3 }
                          ^
```

## Which project

- `jelloc run file.jello` uses the nearest directory at or above the file's that holds a
  `jello.json`.
- `jelloc test [dir...]` and `jelloc deps.get [dir]` use the nearest at or above the first
  directory given, or else the current directory.
- `jelloc` with no input or no `--out`, and `jelloc run` with no file, use the nearest at or above
  the current directory.

Outside a project, `run` compiles the file as a plain compile would, and `test` needs its
directories given.

## Source roots in a project

Imports are looked for under these roots, in order (see
[Modules](../05-modules.md#source-roots)):

1. for `test`, the test directories, so that tests can import helpers kept beside them;
2. the entry file's directory, for `run`;
3. each `--path` directory;
4. the project's `src`;
5. each dependency's source directory: its manifest's `src` (by default `src`) within
   `packages/<name>/`, or else within `<dir>/<name>/` for the first `package_paths` directory that
   has it. A package's own dependencies follow, found the same way, so every package of the
   project, direct or not, is a root;
6. for `test`, the packages that only `dev_dependencies` reach, found the same way.

`std.*` modules still come from `--std` or `JELLO_STD_PATH`.

A dependency that is in none of those places is an error at its entry in the manifest:

```text
jello.json:5:14: package greet is in none of packages; run `jelloc deps.get` [E0212]
    "greet": { "path": "../greet" }
             ^
```

## Dependencies

Each key of `dependencies` is a package's name, and its value says where the package comes from:

| Value | Source |
|-------|--------|
| `"1.2.0"` | a released version: the archive of tag `1.2.0` of the package's repository |
| `{"github": "owner/repo", "tag": "v1.2.0"}` | the archive of that tag of a GitHub repository |
| `{"github": "owner/repo", "commit": "3f2a9c1"}` | the archive of that commit |
| `{"path": "../greet"}` | a directory on disk, relative to the project; only in the project's own `jello.json` |

`dev_dependencies` names packages in the same way, for the project's own tests, such as a test
library. They are fetched like the rest, but are source roots only for `jelloc test`, so the
program cannot import them, and only the project's own count: a package's `dev_dependencies` are
never fetched for the projects that use it. A package named in both must come from the same
place, and is then an ordinary dependency.

```text
"dev_dependencies": {
  "degust": {"github": "Lazarus404/degust", "tag": "0.1.0"}
}
```

`jelloc deps.get` fetches every dependency, and theirs in turn, into `packages/<name>/`. A path
dependency is linked there instead of copied, so edits to it are seen at once. The command
reports what it did on standard error (`fetched greet 1.2.0`, `linked greet to ../greet`,
`removed old`, or `dependencies up to date`), and exits 0 on success and 1 on failure.

- A package is a directory with a `jello.json` whose `name` is the dependency's name and which has
  a `version`; a fetched version must be the version asked for. An archive may hold the package at
  its top or in its one directory, as GitHub's do, and may not hold symbolic links.
- `jello.lock`, beside `jello.json`, records each fetched package's version, archive URL and
  SHA-256. A later `deps.get` reuses a package that matches its lock entry, and refetches one
  that does not; a fetched archive whose SHA-256 differs from the one pinned is an error. Commit
  the lock to get the same packages everywhere.
- There is one copy of each package. Two manifests that ask for one package from different
  sources are an error.
- Anything in `packages/` that no manifest asks for any more is removed.
- Only the project's own manifest may name a `path`; a fetched package that names one is an
  error, since its path could reach anywhere on disk.

A released version is fetched from the registry at `JELLO_REGISTRY_URL` when that is set, and
otherwise from the GitHub repository of the package's name under the owner `Lazarus404`, or the
owner `JELLO_DEPS_GITHUB_ORG` names.

`package_paths` names directories that already hold packages, such as a directory of vendored
copies or the other packages of a monorepo. The compiler finds a dependency there when
`packages/` does not have it; `deps.get` does not use them.

## Running a program

```sh
jelloc run [--std dir] [--path dir]... src/Main.jello [args...]
```

`run` compiles the file, or the project's entry module when it is given no file, into
`build/run/<name>.jlo` in the project, and then runs it with `jello`, passing it the arguments
after the file. Its exit status is the program's. The options
before the file are `jelloc`'s; everything after it is the program's, flags included.

When the project or any of its packages has a native library directory (`jdll` in its manifest,
by default `jdll/`), `run` and `test` pass those directories to `jello` with `--jdll-path`.
Outside a project, `run` passes the file's own directory, and `jdll/` in it, so a program finds
the native libraries beside its source as it would beside a module compiled there (see
[Native libraries](06-native-libraries.md)).

## Running tests

```sh
jelloc test [--std dir] [--path dir]... [dir...] [-- args...]
```

`test` finds every `*.test.jello` file under the directories, or else under the project's test
directories that exist, and compiles them into one test binary, `build/test.jlo`, which it runs.
The arguments after `--` go to the test binary, which takes `-v`, `--run` and `--list`, and the
tests' own options after a further `--`:

```sh
jelloc test -- -v --run app/Names
```

Writing tests is described in [Testing](04-testing.md).

## Building an executable

`jelloc` in the project writes the module `build/<name>.jlo`, and `jello --boot` turns it into an
executable (see [jello](02-jello.md#executables)):

```sh
jelloc
jello --boot --bundle -o build/hello build/hello.jlo
```
