# Compile errors

Each error the compiler reports ends with its code, as in `unknown name: x [E0300]`.
A code is never renumbered or reused. `jelloc --explain E0300` prints what this page says
of it. This page is generated from the compiler's own explanations.

## E0001: the source is not valid UTF-8

A source file must be UTF-8. The error points at the first byte that starts no
valid UTF-8 sequence; save the file as UTF-8.

## E0002: a character that starts no token

The character can begin no token, such as a control character outside a literal
or a comment. Remove it, or write it inside a bytes literal.

## E0003: a malformed number literal

A number literal has no digits after its radix prefix, as `0x` has none, or runs
into a letter, digit or dot that cannot follow it, as in `12abc`. Complete the
literal, or separate it from what follows.

## E0004: an invalid escape

A backslash in a bytes literal or a template starts an escape. The escapes are
\0, \a, \b, \f, \n, \r, \t, \v, a quote, backslash or backtick after a
backslash, \xHH for any byte, and \uHHHH or \u{H...} (one to six hex digits) for
a Unicode scalar value, which is no surrogate and at most 10FFFF. Write a
literal backslash as \\.

## E0005: an unterminated literal or comment

A bytes literal, regexp literal, template or block comment runs to the end of
its line, or of the file, without its closing delimiter. A bytes literal ends on
the line it starts; a template and a block comment may span lines. Close it.

## E0006: a backslash ends a template's line

A backslash cannot be the last character of a line in a template, where it would
escape the newline. Write \n for a newline, or move the backslash.

## E0007: a literal does not fit its type

A number literal's value is not a value of the type it has: it is out of the
type's range, or, for an integer given a float type, not exactly representable.
A bits segment's literal must fit the segment's width. Use a wider type, or a
smaller value; `let big: I64 = 3000000000;`.

## E0008: `.code` needs exactly one character

`"A".code` is the code point of a literal of one Unicode character. The literal
has none, or more than one.

## E0100: a syntax error

The parser found a token where the grammar allows none of that kind; the message
says what it expected. The fault is often just before the place shown, such as a
missing `;`, `)` or `}`.

## E0101: a trailing comma

No longer reported. Lists of arguments, parameters, array items and function
type parameters once could not end with a comma; now they can, so `f(1, 2,)` is
`f(1, 2)`.

## E0102: parameters in the wrong order

Required parameters come first, then those with defaults, then optional `?x`
parameters, and a varargs parameter, `xs: T...`, is last; it takes no `?` and no
default. A function type's `T...` is likewise its last parameter.

## E0103: an invalid assignment target

Only a variable, an array or buffer element, or a record's or an Object's field
can be assigned, and only those of them that hold integers incremented. A
tuple's fields cannot be assigned; build a new tuple instead.

## E0104: an invalid bits segment

In a bytes layout or a bits value, segments must fill whole bytes: a segment of
8 bits or more, or a rest, starts on a byte boundary, there is at most one rest,
and it is last. Widths are 1 to 8, 16 or 32 bits; `le` or `be` goes on wider
segments, not on `u8` or a rest.

## E0105: a `let ... else` misused

Only a `let` whose pattern can fail takes an `else`, as in `let [first, ..._] =
xs else { return 0; };`, and the `else` must leave by `return`, `throw`, `break`
or `continue`, since the names would otherwise be unbound. A `let` of one name
cannot fail, so it takes none.

## E0106: a pipeline topic out of place

`%` names the value of a pipeline, `value |> body`, so it is only in a
pipeline's body.

## E0107: a declaration that cannot be exported

`export` goes before a top-level `let`, `const`, enum, typedef, prototype or
bytes layout. Remove it from this declaration.

## E0108: a chain needs parentheses

`x is A is B` and `A -> B -> C` do not chain. Parenthesise to say which you
mean; a function type of two parameters is `(A, B) -> C`.

## E0109: a `let` test out of place

`let pattern = value` is a test only as the condition of an `if` or a `while`,
alone or joined with `&&`: `if (let [first, ..._] = xs) { ... }`. Elsewhere
`let` is a statement, which ends with `;`, so a block cannot end with one as its
value.

## E0110: a declaration out of place

Imports, enums, typedefs and prototypes are declared at the top level of a
module, where the whole module sees them, never inside a function. Move this one
out of the function.

## E0200: a module not found

An import names a module that is in no source root. The module a.b.C is the file
a/b/C.jello under one of the roots; the message lists the paths tried. Add the
root with --path <dir>, or in a project, check its jello.json `src` and
dependencies.

## E0201: the standard library was not found

The module is in the standard library, but the compiler was not told where that
is. Pass --std <dir> or set JELLO_STD_PATH.

## E0202: a module's package does not match its path

A module's `package` declaration must name the directories its import path
gives; a.b.C declares `package a.b;`. Change the declaration or the import.

## E0203: an import cycle

Modules import each other in a cycle, so none can be initialised first. Move
what they share into a module both import.

## E0204: an imported module ends with an expression

Only the program's entry module has a result, its last expression. A module that
is imported must end with a statement; end the expression with `;`.

## E0205: a duplicate import name

Two imports bring in the same name. Rename one with `as`; `import { f as g }
from a.B;`.

## E0206: the wrong import form

Values are imported with `import { x } from m;` or `import m as M;`, types with
`import type { T } from m;`, and natives with `load { f } from library;`, where
the library is one name. The import used names the wrong kind.

## E0207: a module imported under two names

One file can be imported by one module path only, since a path names the
module's package. Import it the same way everywhere.

## E0208: an unknown export

The module exports no value or type of that name. Check its spelling, and that
the declaration is marked `export`.

## E0209: a test file misnamed or misplaced

A test file's name ends in .test.jello, and it is under a --path root, which
gives its module path.

## E0210: a test with the wrong signature

In a test file, a test is a top-level `let testName = fn(t: Test) { ... };`,
taking std.testing's Test, and the hooks `beforeAll` and `afterAll` take no
parameter: `let beforeAll = fn() { ... };`. Both return Null.

## E0211: an invalid jello.json

A project's jello.json is a JSON object. `src` is a string, `test` an object
whose `dirs` is an array of strings, `package_paths` an array of strings, and
`dependencies` an object. Other keys are ignored.

## E0212: a dependency not found

A dependency in jello.json is in neither packages/<name> nor a directory of
`package_paths`. Run `jelloc deps.get` to fetch it.

## E0213: an invalid native declaration

A native is bound with a function type whose parameters and result are Bool,
I32, I64, F32, F64, Bytes, Dynamic or an abstract type, null or not, or whose
result is Void, before it is used: `let f: (Bytes) -> I32 = native_name;`. Every
binding of one native gives it the same type.

## E0214: std.core exports an intrinsic

A std.core module cannot export a name the compiler provides itself.

## E0215: an invalid .formatter

A .formatter file is a JSON object. `inputs`, `exclude` and `extensions` are
arrays of strings, `indent` and `line_width` whole numbers, `strip_comments`,
`keep_doc_comments`, `rewrite_comments` and `blank_before_one_line_blocks` true
or false, and `overrides` an array of objects, each with `paths` or
`extensions`, or both, arrays of strings, and any of those settings. Other keys
are ignored.

## E0300: an unknown name

No binding of that name is in scope. Check the spelling, declare it before its
use in a block, or import it.

## E0301: not a value

The name is a type, or `_`, which binds nothing; neither is a value. Construct a
value of the type instead, as with an enum variant, `Shape.Circle(1.0)`. A
built-in function, such as `Array.contains`, is only called; to pass one as a
value, wrap the call in a function, `fn(a: Array<I32>, x: I32): Bool {
Array.contains(a, x) }`.

## E0302: a duplicate declaration

A name is declared twice where it must be unique: two locals of one scope, two
top-level names of a module, two parameters, type parameters, fields or
variants, or a method that shares its name with a field. Rename one.

## E0303: an assignment to something immutable

Only a local or global declared with `let`, not `const`, can be assigned.
Functions, natives, namespaces, assertions and imported bindings cannot be.

## E0304: a use before initialisation

A global is read while the module initialises, before its `let` has run, or a
function uses a binding whose type is not known yet. Move the declaration
earlier, or annotate the binding's type.

## E0305: a type parameter hides a type

A type parameter has the name of a type in scope, so the type could not be named
inside. Rename the parameter.

## E0306: `this` outside a method

`this` is the receiver of a prototype's method. A plain `fn` inside a method
does not see it; write `method fn` for one that does.

## E0307: `break`, `continue` or `return` out of place

`break` and `continue` are inside a loop of their own function, and cannot leave
a list comprehension for a loop around it; `return` is inside a function, not at
a module's top level.

## E0308: a generic local function captures a local

A generic function declared inside another cannot capture the outer function's
locals. Pass the value as an argument.

## E0400: mismatched types

A value's type is not the one its place requires: a parameter, an annotation, a
field, an element, a condition (which is Bool), or a function's result. Convert
the value, as `n as I64`, or change the annotation. Values convert implicitly
only where nothing is lost: a number to a type that holds all its values, a T to
a Null<T>.

## E0401: an unknown type

No type of that name is in scope, or it cannot be used as written: Void is no
value type, and a tuple type is `Tuple<A, B>`. Import the type with `import
type`.

## E0402: the wrong number of type arguments

A generic type or function takes as many type arguments as it has type
parameters; anything else takes none. An enum's are written on the enum, not the
variant: `Box<I32>.Empty`.

## E0403: a type cannot be inferred

Nothing fixes a type here, such as a bare `null`, an empty array, or a parameter
no call constrains. Annotate it, `let xs: Array<I32> = [];`, or give the value
its type with `as`, `[] as Array<I32>`.

## E0404: an implicit Dynamic conversion

A value becomes Dynamic only with `as Dynamic`, and a Dynamic one becomes a T
only with `as T`, which checks it at run time.

## E0405: a value that cannot become Dynamic

A Dynamic holds a value it can describe exactly. A list cannot become Dynamic,
since its empty value, nil, reads as null; nor a buffer from Bytes.new, nor a
U64, or U32 inside another type, whose large values would read as negative.

## E0406: an invalid `as` or `is`

`as` converts between number types and to and from Dynamic; `x is T` tests a
Dynamic value, and the pattern `is T` a Dynamic or nullable one. Only Bool, I32,
I64, F16, F32, F64, Atom, Bytes, enums, Object and abstract types are checked
kinds, which a Dynamic can be extracted as or tested for; for an abstract type,
that it holds a native handle.

## E0407: an invalid Dynamic comparison

A Dynamic compares with null, or with a value of a kind `is` can test. Two
Dynamics do not compare; extract one first with `as T`.

## E0408: a misuse of null

Only a Null<T>, written ?T, holds null, and only it compares with null, by ==
and !=. A list is never null: its empty value is nil, which List.is_nil tests. A
parameter with a default is not nullable; an optional `?x` parameter is.

## E0409: a value that cannot be thrown

A thrown value is caught as Dynamic, so it must be a value that can become one:
not a list or a buffer from Bytes.new.

## E0410: a block used as a value has none

A block, match arm, `try`, `with` body or `if` used as a value must end with an
expression, without `;`, and an `if` needs an `else`.

## E0411: a buffer where Bytes are needed

A buffer from Bytes.new is mutable, and not Bytes. Copy its bytes out with
`buffer.toString()`.

## E0412: Bytes are immutable

A Bytes value cannot change. Build bytes in a buffer from Bytes.new, then copy
them out with `buffer.toString()`.

## E0413: a pipeline body that is not a call

A pipeline body that does not use `%` must be a call, which takes the pipeline's
value as its first argument; `5 |> double()`. Use `%` to put the value anywhere
else; `5 |> % * 2`.

## E0500: an operator not defined for a type

The operator does not apply to its operands' types. Arithmetic takes two numbers
of one type, `&&`, `||` and `!` take Bool, and ++ and -- an integer. Convert one
operand with `as`, so that both have the same type.

## E0501: a missing or unknown bound

A generic function uses == on a type parameter, prints it, or uses it as an
integer, so the parameter needs that bound, Eq, Show or Integer: `let same<T:
Eq> = fn(a: T, b: T) ...`. A call's type argument must satisfy the bounds; only
functions' type parameters take bounds.

## E0600: the wrong number of arguments

A call passes more arguments than the function has parameters, or fewer than it
requires. Parameters with defaults and optional ones may be left out.

## E0601: a call of something that is not a function

The value called is not a function. Check what the name is bound to.

## E0602: a name that is not a value

A generic function, a native and an assertion can only be called, and a module
namespace only named through, as `M.x`. A generic prototype is a value with its
type arguments, `P<I32>`.

## E0603: an invalid parameter default

A default is a constant of the parameter's type, which is not nullable; a
parameter that may be left out to null is an optional one, `?x: T`.

## E0604: an invalid spread

A spread, `...xs`, passes an array's or a tuple's items as arguments or items.
Some calls, such as Function.bind, Function.call and an Object's functions, take
none.

## E0605: type parameters where none are taken

Only functions, enums, typedefs and prototypes take type parameters; a generic
function is `let id<T> = fn(x: T): T { x };`. A `let` of any other value, a
bytes layout and an abstract type take none.

## E0700: a match that is not exhaustive

A match must cover every value of its subject. The message gives a value no arm
matches; add an arm for it, or a `_` arm.

## E0701: unreachable code

A match arm can match nothing the arms before it do not, or a statement follows
one that always leaves, such as `return`. Remove it, or reorder the arms.

## E0702: a pattern that can fail

The pattern of a plain `let`, or of a `let` clause in a `with`, must match every
value. Use `if (let p = value)` or `let p = value else { ... };` for a pattern
that can fail, and in a `with`, a clause `p <- value`.

## E0703: a pattern that cannot match its value

The pattern is of a different type from the value it matches, such as a tuple
pattern of the wrong size, or a record pattern with a rest.

## E0704: a pattern binds a name badly

A pattern binds each name once, and every alternative of an `|` pattern binds
the same names with the same types. A spread is used once, and in a list pattern
only at its end; a field is matched once.

## E0705: an invalid pin

`^x` matches the value of x, a constant in scope. It cannot pin a name the
pattern itself binds, nor one that can be assigned.

## E0706: a `with` that cannot give its value

A `with` clause that fails gives the failing value, unless an `else` handles it,
so that value must have the `with`'s type; add an `else` otherwise. An `else`
needs at least one arm, and a `with` of only `let` clauses cannot fail, so takes
none.

## E0800: no such field, member or function

The type has no field or method of that name, or the namespace no such function.
Check the spelling; the message lists close names when there are any.

## E0801: an invalid record literal

A record literal names each field of its type once: `{ x: 1, y: 2 }`. A spread
of a record of the same type may come first, `{ ...p, x: 3 }`, to fill the rest.

## E0802: an unknown enum variant

The enum has no variant of that name.

## E0803: the wrong payload count

An enum variant is constructed and matched with as many values as it declares,
and one without values is written without parentheses.

## E0804: an invalid typedef

A typedef cannot refer to itself. A typedef linked to a prototype of the same
name has the same type parameters and is exported as it is, and its fields have
types an Object can hold as themselves.

## E0805: a misuse of a prototype

`new P(args)` makes an Object that delegates to the prototype P, passing the
arguments to its init method; `super.m(...)` calls the method m of the Object a
prototype delegates to. An Object's __proto__ is an Object, and no prototype
delegates to itself.

## E0806: a misuse of an Object

An Object's keys are Atoms or bytes literals, and its fields are Dynamic, so a
field is assigned a new value rather than updated in place, as with +=. An
Object literal spreads only another Object.

## E0807: an invalid index or tuple field

Arrays, buffers and Objects are indexed with []; a tuple's fields are .0, .1 and
so on, up to its size. A method cannot be updated in place.

## E0808: a compile-time literal required

Atom.intern takes a bytes literal, which the compiler interns.

## E0809: an invalid regexp

A regexp literal or pattern does not compile, or has a flag that is not one of
the known flags. The message gives the regexp engine's reason.

## E0900: not supported yet

The form is part of the language, but this compiler does not support it yet.

## E0901: a compiler limit

The program passes a limit of the compiler or VM: 256 registers in one function,
try regions nested 16 deep, natives of more than 8 arguments, generic instances
nested 64 deep, or macro expansions nested 32 deep or making more than 4096
nodes in one module. Split the function, or simplify the nesting.

## E1000: an invalid macro

A macro is defined at the top level of a module, once per name and with no
value's name, and its body runs when the program compiles, so it can use only
its parameters, its own locals, the builtins and `Macro`: it cannot import, load
a native library or define a macro. `import macro { name } from module;` names a
macro the module defines with `export macro`.

## E1001: a quote or unquote out of place

`quote { ... }` makes syntax in a macro body, and `unquote(value)` splices into
a quote the syntax, or the literal, that `value` holds; neither means anything
elsewhere, and a quote cannot hold another. In a type, `unquote(name)` names a
variable of the macro body. In a match-arm list, it may splice an
`Array<Fragment>` of `quote.arm` fragments.

## E1002: a macro call that cannot expand

A macro call passes one argument per parameter. A call that is a statement
expands to the statements its macro emits; a call anywhere else must expand to
one expression. A quote passed as an argument holds no `unquote`.

## E1003: a macro that fails

The macro's body threw, trapped or ran too long while the program compiled, or
emitted syntax that does not fit where it goes, such as a statement where an
expression is needed, or text that `Macro.parse` cannot parse. The message says
which.
