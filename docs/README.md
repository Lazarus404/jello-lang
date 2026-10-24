# The Jello language

Jello is a statically typed language that compiles to bytecode for its own virtual machine. This
reference describes the language as the compiler implements it. Each page states the rules with
examples, and every example marked `jello` is a program that the compiler's test suite runs, so
what an example says it prints, or the error it says it gives, is what happens.

A program is a source file: the compiler, [jelloc](07-tooling/01-jelloc.md), compiles it with the
modules it imports into a `.jlo` file, and [jello](07-tooling/02-jello.md) runs that. A
program's value is its last expression; when that is `Bytes`, the VM prints it.

```jello
// expect: hello, world
let name = "world";
"hello, " + name
```

## Contents

1. [Lexical structure](01-lexical.md): characters, comments, names, keywords, literals, operators
   and statement separators.
2. Types
   - [Scalars](02-types/01-scalars.md): integers, floats, `Bool`, `Bytes` and templates, atoms
     and `null`.
   - [Nullability and Dynamic](02-types/02-nullability-and-dynamic.md): `?T` and values whose
     type is known only at run time.
   - [Collections](02-types/03-collections.md): tuples, arrays, lists and objects.
   - [Functions](02-types/04-functions.md): function types, parameters and closures.
   - [Enums](02-types/05-enums.md): variants with fields.
   - [Typedefs and records](02-types/06-typedefs-and-records.md): aliases, records and byte
     layouts.
   - [Generics](02-types/07-generics.md): type parameters and their instances.
3. Statements
   - [Bindings](03-statements/01-bindings.md): `let`, `const`, destructuring and assignment.
   - [Control flow](03-statements/02-control-flow.md): blocks, `if`, loops and `return`.
   - [Patterns](03-statements/03-patterns.md): `match`, `with`, `let ... else` and guards.
   - [Exceptions](03-statements/04-exceptions.md): `throw` and `try`.
   - [Macros](03-statements/05-macros.md): code that runs while the program compiles.
4. Expressions
   - [Operators](04-expressions/01-operators.md), with the precedence table.
   - [Calls and members](04-expressions/02-calls-and-members.md): methods on built-in types,
     spread arguments, pipelines and casts.
   - [Objects and prototypes](04-expressions/03-objects-and-prototypes.md).
   - [Bits](04-expressions/04-bits.md): packing and unpacking binary data.
   - [Regular expressions](04-expressions/05-regular-expressions.md).
5. [Modules](05-modules.md): packages, imports and exports.
6. [The standard library](06-stdlib/README.md)
   - [Built-in namespaces and value methods](06-stdlib/01-builtins.md)
   - [Core modules](06-stdlib/02-core.md)
   - [Files, paths, commands and the environment](06-stdlib/03-io.md)
   - [Processes, jobs and supervisors](06-stdlib/04-concurrent.md)
   - [Networking](06-stdlib/05-net.md)
   - [std.macro](06-stdlib/06-macro.md)
   - [std.testing](06-stdlib/07-testing.md)
   - [Dates and times](06-stdlib/08-dates.md)
   - [Cryptography](06-stdlib/09-crypto.md)
7. Tooling
   - [jelloc](07-tooling/01-jelloc.md), the compiler.
   - [jello](07-tooling/02-jello.md), the virtual machine.
   - [Projects](07-tooling/03-projects.md): `jello.json`, dependencies, `jelloc run` and
     `jelloc test`.
   - [Testing](07-tooling/04-testing.md): writing and running tests.
   - [Platforms](07-tooling/05-platforms.md): the systems Jello runs on, and how they differ.
   - [Native libraries](07-tooling/06-native-libraries.md): writing your own `.jdll` in C, from
     a first library to handles and blocking calls.
   - [Limits](07-tooling/07-limits.md): how deep, how big and how many, beside Lua's.
   - [Formatting](07-tooling/08-formatting.md): `jelloc fmt` and the `.formatter` file.
8. Errors
   - [Compile errors](08-errors/01-compile-errors.md): every error code and what it means.
   - [Runtime traps](08-errors/02-runtime-traps.md): the faults that stop a program.
