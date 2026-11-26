# Not Yet Implemented

This list records language forms that the compiler or documentation explicitly describes as not
supported yet.

## Expressions and calls

- Implement `await` expressions. The parser accepts them, but semantic checking reports `E0900` (`jelloc/tests/e2e/programs/diagnostics/unsupported_await.jello`).
- Support spread arguments in calls such as `Math.max(...values)`. They currently report `a spread here is not supported yet` (`jelloc/tests/e2e/programs/spread/builtin.jello`).
- Allow calling arbitrary function-valued expressions directly. They currently must first be bound to a name (`jelloc/src/frontend/semantics/Aggregates.jello:1631`).
- Support type application outside a call. Semantic checking currently reports `E0900` (`jelloc/src/frontend/semantics/TypeCheck.jello:3809`).
- Support `with` where it appears in an expression context. Semantic checking currently reports `E0900` (`jelloc/src/frontend/semantics/TypeCheck.jello:3811`).

## Assignment and aggregates

- Support assignment through a member followed by an index, for example `o.items[1] = value` (`jelloc/itests/syntax/index_assign_member.jello`).
- Support spread fields in `bytes-layout` values passed to `.pack` (`jelloc/itests/typedef/bytes_layout_pack_spread.jello`).
- Add the remaining compound assignment operators. Only `+=`, `-=`, `*=`, and `/=` are currently accepted; `%=` is explicitly rejected (`jelloc/itests/syntax/unsupported_compound.jello`).

## Patterns and declarations

- Support regexp patterns in `with` clauses (`jelloc/itests/with/regexp_in_clause.jello`).
- Support exported generic prototypes. They currently desugar to an unsupported exported templated binding (`jelloc/itests/prototype/export_generic.jello`).

## Native functions

- Allow a name introduced by `load` to be called directly. It currently requires an intermediate typed function binding (`docs/05-modules.md:296`).
- Allow typed native bindings to be assigned, passed, or stored as function values. They can currently only be called; wrapping one in a Jello function is the documented workaround (`docs/05-modules.md:305`).
