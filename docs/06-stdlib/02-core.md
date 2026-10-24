# Core modules

The `std.core` package holds the functions of the built-in value types that the compiler does not
implement itself:

| Module | Import needed | Covers |
| --- | --- | --- |
| `std.core.Array` | no | searching, mapping, folding, inserting and sorting arrays |
| `std.core.Bytes` | no | searching, splitting and trimming text; UTF-8; hexadecimal and Base64; fixed-width numbers |
| `std.core.List` | no | the same operations for immutable lists |
| `std.core.Object` | no | field counts, merging and values of Objects |
| `std.core.RegExp` | yes | regular expressions |

`Array`, `Bytes`, `List` and `Object` are *extension modules*: a call `Array.f(xs, ...)` or
`xs.f(...)` of a name that is not an [intrinsic](01-builtins.md) loads the type's module and calls
its export `f`, with no import. A name the module does not export is an error that lists the
closest names it does export:

```jello
// error: 3:1: unknown method mapp of Array<I32>; closest: map [E0800]
let xs = [1, 2];
xs.mapp(fn(x: I32): I32 { x })
```

A module can also be imported as any other, for instance to call a function by a bare name:
`import { split, join } from std.core.Bytes;`.

## std.core.Array

The functions take the array first. An index out of range traps `BOUNDS`, as indexing does, except
in `insert` and `splice`, which throw.

### Searching

| Function | Description |
| --- | --- |
| `indexOf<T: Eq>(values: Array<T>, value: T): I32` | the index of the first `value`, or -1 |
| `contains<T: Eq>(values: Array<T>, value: T): Bool` | whether `values` holds `value` |
| `first<T>(values: Array<T>, keep: (T) -> Bool): ?T` | the first item `keep` holds for, or `null` |
| `last<T>(values: Array<T>, keep: (T) -> Bool): ?T` | the last item `keep` holds for, or `null` |
| `internIndex<T: Eq>(values: Array<T>, value: T): I32` | the index of `value`, which is appended first when absent |

### Transforming

These return a new array and leave `values` as it was.

| Function | Description |
| --- | --- |
| `map<T, U>(values: Array<T>, f: (T) -> U): Array<U>` | `f` of each item, in order |
| `mapIndexed<T, U>(values: Array<T>, f: (T, I32) -> U): Array<U>` | `f` of each item and its index |
| `filter<T>(values: Array<T>, keep: (T) -> Bool): Array<T>` | the items `keep` holds for |
| `filterIndexed<T>(values: Array<T>, keep: (T, I32) -> Bool): Array<T>` | the same, given each index too |
| `reject<T>(values: Array<T>, drop: (T) -> Bool): Array<T>` | the items `drop` does not hold for |
| `rejectIndexed<T>(values: Array<T>, drop: (T, I32) -> Bool): Array<T>` | the same, given each index too |
| `copy<T>(values: Array<T>): Array<T>` | a new array of the same items |
| `slice<T>(values: Array<T>, start: I32, count: I32): Array<T>` | `count` items from `start` |

### Folding and visiting

| Function | Description |
| --- | --- |
| `reduce<T, U>(values: Array<T>, initial: U, f: (U, T) -> U): U` | `initial`, after `f` folds in each item in order |
| `reduceIndexed<T, U>(values: Array<T>, initial: U, f: (U, T, I32) -> U): U` | the same, given each index too |
| `forEach<T>(values: Array<T>, f: (T) -> Null): Array<T>` | calls `f` on each item; `values` |
| `forEachIndexed<T>(values: Array<T>, f: (T, I32) -> Null): Array<T>` | the same, given each index too |

```jello
// expect: [20, 40] 6 2 null 2,4 a-b-c
let xs = [1, 2, 3, 4];
let evens = xs.filter(fn(x: I32): Bool { x % 2 == 0 });
let doubled = Array.map(evens, fn(x: I32): I32 { x * 10 });
let sum = xs.reduce(0, fn(total: I32, x: I32): I32 { total + x }) - 4;
let big = xs.first(fn(x: I32): Bool { x > 9 });
let shown = Array.new<Bytes>(0);
evens.forEach(fn(x: I32) { shown.push(Std.string(x)); });
Std.string(doubled) + " " + Std.string(sum) + " " + Std.string(xs.indexOf(3)) + " "
  + Std.string(big) + " " + shown.join(",") + " " + ["a", "b", "c"].join("-")
```

### Changing in place

These change `values` itself.

| Function | Description |
| --- | --- |
| `appendFrom<T>(out: Array<T>, values: Array<T>, start: I32): Array<T>` | appends the items of `values` from `start` on to `out`; `out` |
| `reverse<T>(values: Array<T>): Array<T>` | reverses `values`; `values` |
| `insert<T>(values: Array<T>, at: I32, value: T): I32` | inserts `value` at `at`, from 0 to the length; the new length |
| `remove<T>(values: Array<T>, at: I32): T` | removes and returns the item at `at` |
| `shift<T>(values: Array<T>): ?T` | removes and returns the first item, or `null` when empty |
| `unshift<T>(values: Array<T>, value: T): I32` | inserts `value` first; the new length |
| `splice<T>(values: Array<T>, start: I32, count: I32, ...items: T): Array<T>` | replaces `count` items from `start` by `items`; the items removed |
| `sort<T>(values: Array<T>, compare: (T, T) -> I32): Array<T>` | sorts `values` stably; `values` |

`compare(a, b)` is below 0 when `a` goes first, above 0 when `b` does, and 0 when either may; items
that compare 0 keep their order.

```jello
// expect: [1, 9, 2, 3] [9, 2] 1 [0, 3, 6, 7, 8] [("b", 1), ("c", 1), ("a", 2)]
let xs = [1, 2, 3];
xs.insert(1, 9);
let inserted = Std.string(xs);
let removed = xs.splice(1, 2, 7, 8, 6);
let first = xs.shift();
xs.unshift(0);
xs.sort(fn(a: I32, b: I32): I32 { a - b });
let pairs = [("a", 2), ("b", 1), ("c", 1)];
pairs.sort(fn(p: Tuple<Bytes, I32>, q: Tuple<Bytes, I32>): I32 { p.1 - q.1 });
inserted + " " + Std.string(removed) + " " + Std.string(first) + " " + Std.string(xs) + " "
  + Std.string(pairs)
```

## std.core.Bytes

The functions take the Bytes first; they read Bytes, not buffers from `Bytes.new` (copy a buffer
out with `toString()` first). Indexes count bytes, except in the `utf8` functions, which count
code points. An index out of range traps.

### Searching and comparing

| Function | Description |
| --- | --- |
| `isEmpty(text: Bytes): Bool` | whether `text` has no bytes |
| `startsWith(haystack: Bytes, prefix: Bytes): Bool` | whether `haystack` starts with `prefix` |
| `endsWith(haystack: Bytes, suffix: Bytes): Bool` | whether `haystack` ends with `suffix` |
| `contains(haystack: Bytes, needle: Bytes): Bool` | whether `needle` occurs in `haystack` |
| `indexOf(haystack: Bytes, needle: Bytes): I32` | the first index of `needle`, or -1; an empty needle is at 0 |
| `indexOfFrom(haystack: Bytes, needle: Bytes, start: I32): I32` | the first index from `start` on, or -1 |
| `lastIndexOf(haystack: Bytes, needle: Bytes): I32` | the last index of `needle`, or -1 |

### Building and splitting

| Function | Description |
| --- | --- |
| `concat(first: Bytes, second: Bytes): Bytes` | `first + second` |
| `substring(text: Bytes, start: I32, end: I32): Bytes` | the bytes from `start` up to, not including, `end` |
| `trim(text: Bytes): Bytes` | `text` without the ASCII whitespace at either end |
| `split(text: Bytes, separator: Bytes): Array<Bytes>` | the parts between occurrences of `separator`; `""` gives `[""]` |
| `join(parts: Array<Bytes>, separator: Bytes): Bytes` | the parts with `separator` between each two |
| `replace(text: Bytes, pattern: Bytes, replacement: Bytes): Bytes` | `text` with its first `pattern` replaced |
| `replaceAll(text: Bytes, pattern: Bytes, replacement: Bytes): Bytes` | `text` with every `pattern` replaced |
| `copy(text: Bytes): ByteBuffer` | a new buffer holding the bytes of `text` |
| `u8(value: I32): Bytes` | the one byte `value`; outside 0 to 255 traps |

The separator of `split` and the pattern of `replace` and `replaceAll` must not be empty; each
throws when it is. `join(split(t, s), s)` is `t`.

```jello
// expect: ["a", "b", "", "c"] a+b+c [x] true 4 lo HELLO
let line = "  x  ";
let parts = "a,b,,c".split(",");
let buffer = "hello".copy();
let i = 0;
while (i < buffer.len()) { buffer.set(i, buffer.get(i) - 32); i++; }
Std.string(parts) + " " + "a-b-c".replaceAll("-", "+") + " [" + line.trim() + "] "
  + Std.string("hello".endsWith("lo")) + " " + Std.string("abcabc".indexOfFrom("b", 2)) + " "
  + "hello".substring(3, 5) + " " + buffer.toString()
```

### UTF-8

| Function | Description |
| --- | --- |
| `utf8Validate(text: Bytes): Bool` | whether `text` is valid UTF-8 |
| `utf8Length(text: Bytes): I32` | the number of code points |
| `utf8CharAt(text: Bytes, index: I32): I32` | the code point at code point `index` |
| `utf8FromCodepoint(code: I32): Bytes` | the UTF-8 encoding of `code` |

```jello
// expect: 6 5 233 true é
let word = "héllo";
Std.string(word.len()) + " " + Std.string(word.utf8Length()) + " " + Std.string(word.utf8CharAt(1))
  + " " + Std.string(word.utf8Validate()) + " " + Bytes.utf8FromCodepoint(233)
```

### Hexadecimal and Base64

| Function | Description |
| --- | --- |
| `toHex(data: Bytes): Bytes` | `data` in lowercase hexadecimal, two digits a byte |
| `fromHex(text: Bytes): Bytes` | the bytes hexadecimal `text` gives, in either case |
| `toBase64(data: Bytes, url: Bool = false): Bytes` | `data` in Base64 (RFC 4648), padded with `=`; with `url`, in the alphabet for URLs and file names, unpadded |
| `fromBase64(text: Bytes, url: Bool = false): Bytes` | the bytes Base64 `text` gives, in that alphabet; the padding is optional |

`fromHex` and `fromBase64` throw for text with anything else in it, whitespace included.

```jello
// expect: 4a656c6c6f SmVsbG8/ SmVsbG8_ Jello?
let data = "Jello?";
Bytes.slice(data, 0, 5).toHex() + " " + data.toBase64() + " " + data.toBase64(true) + " "
  + Bytes.fromBase64("SmVsbG8_", true)
```

### Fixed-width numbers

Readers take Bytes and the byte index `at`; writers take a buffer from `Bytes.new`, the index and
the value, and return the index after what they wrote. `Le` is least significant byte first, `Be`
most significant first.

| Functions | Value |
| --- | --- |
| `readU16Le`, `readU16Be` (Bytes, I32): I32 | an unsigned 16-bit integer |
| `readU32Le`, `readU32Be`, `readI32Le`, `readI32Be` (Bytes, I32): I32 | a 32-bit integer; a `U32` reads as the `I32` of its bits |
| `readF32Le`, `readF32Be` (Bytes, I32): F32 | a 32-bit float |
| `readI64Le`, `readI64Be` (Bytes, I32): I64 | a 64-bit integer |
| `readF64Le`, `readF64Be` (Bytes, I32): F64 | a 64-bit float |
| `writeU16Le`, `writeU16Be` (ByteBuffer, I32, I32): I32 | the low 16 bits of the value |
| `writeU32Le`, `writeU32Be`, `writeI32Le`, `writeI32Be` (ByteBuffer, I32, I32): I32 | the 32 bits of the value |
| `writeF32Le`, `writeF32Be` (ByteBuffer, I32, F32): I32 | a 32-bit float |
| `writeI64Le`, `writeI64Be` (ByteBuffer, I32, I64): I32 | a 64-bit integer |
| `writeF64Le`, `writeF64Be` (ByteBuffer, I32, F64): I32 | a 64-bit float |

For records laid out in bytes, [byte layouts](../02-types/06-typedefs-and-records.md) and
[bit patterns](../04-expressions/04-bits.md) are usually clearer.

```jello
// expect: 10 258 16909060 513 1.5
let buffer = Bytes.new(10);
let at = Bytes.writeU16Le(buffer, 0, 258);
at = Bytes.writeU32Be(buffer, at, 16909060);
at = Bytes.writeF32Le(buffer, at, 1.5f32);
let data = buffer.toString();
Std.string(at) + " " + Std.string(Bytes.readU16Le(data, 0)) + " " + Std.string(data.readU32Be(2))
  + " " + Std.string(data.readU16Be(0)) + " " + Std.string(data.readF32Le(6))
```

```jello
// expect: 16 -2 0.1 -72057594037927937
let buffer = Bytes.new(16);
let at = Bytes.writeI64Le(buffer, 0, -2i64);
at = Bytes.writeF64Be(buffer, at, 0.1);
let data = buffer.toString();
Std.string(at) + " " + Std.string(data.readI64Le(0)) + " " + Std.string(data.readF64Be(8))
  + " " + Std.string(data.readI64Be(0))
```

## std.core.List

A list is immutable, so the functions that change one return a new list, sharing what they can.
An index out of range throws.

| Function | Description |
| --- | --- |
| `isEmpty<T>(values: List<T>): Bool` | whether `values` has no items |
| `len<T>(values: List<T>): I32` | the number of items |
| `indexOf<T: Eq>(values: List<T>, value: T): I32` | the index of the first `value`, or -1 |
| `contains<T: Eq>(values: List<T>, value: T): Bool` | whether `values` holds `value` |
| `first<T>(values: List<T>, keep: (T) -> Bool): ?T` | the first item `keep` holds for, or `null` |
| `last<T>(values: List<T>, keep: (T) -> Bool): ?T` | the last item `keep` holds for, or `null` |
| `map<T, U>(values: List<T>, f: (T) -> U): List<U>` | `f` of each item, in order |
| `mapIndexed<T, U>(values: List<T>, f: (T, I32) -> U): List<U>` | the same, given each index too |
| `filter<T>(values: List<T>, keep: (T) -> Bool): List<T>` | the items `keep` holds for |
| `filterIndexed<T>(values: List<T>, keep: (T, I32) -> Bool): List<T>` | the same, given each index too |
| `reject<T>(values: List<T>, drop: (T) -> Bool): List<T>` | the items `drop` does not hold for |
| `rejectIndexed<T>(values: List<T>, drop: (T, I32) -> Bool): List<T>` | the same, given each index too |
| `reduce<T, U>(values: List<T>, initial: U, f: (U, T) -> U): U` | `initial`, after `f` folds in each item |
| `reduceIndexed<T, U>(values: List<T>, initial: U, f: (U, T, I32) -> U): U` | the same, given each index too |
| `forEach<T>(values: List<T>, f: (T) -> Null): List<T>` | calls `f` on each item; `values` |
| `forEachIndexed<T>(values: List<T>, f: (T, I32) -> Null): List<T>` | the same, given each index too |
| `reverse<T>(values: List<T>): List<T>` | the items in reverse order |
| `toArray<T>(values: List<T>): Array<T>` | the items in a new array |
| `join(parts: List<Bytes>, separator: Bytes): Bytes` | the parts with `separator` between each two |
| `concat<T>(values: List<T>, more: List<T>): List<T>` | the items of `values`, then those of `more` |
| `append<T>(values: List<T>, value: T): List<T>` | `values` with `value` after its items |
| `slice<T>(values: List<T>, start: I32, count: I32): List<T>` | `count` items from `start` |
| `insert<T>(values: List<T>, at: I32, value: T): List<T>` | `values` with `value` at `at`, from 0 to the length |
| `remove<T>(values: List<T>, at: I32): List<T>` | `values` without the item at `at` |
| `sort<T>(values: List<T>, compare: (T, T) -> I32): List<T>` | the items sorted stably, as `Array.sort` does |

```jello
// expect: [3, 1, 2] [1, 2, 3] [1, 7, 2, 9] [1, 2, 2, 1] 6 3+1+2 List.slice index out of range
let xs = List.cons(3, List.cons(1, List.cons(2, List.nil<I32>())));
let sorted = xs.sort(fn(a: I32, b: I32): I32 { a - b });
let more = xs.remove(0).insert(1, 7).append(9);
let joined = xs.slice(1, 2).concat(xs.reverse().slice(0, 2));
let total = xs.reduce(0, fn(sum: I32, x: I32): I32 { sum + x });
let named = xs.map(fn(x: I32): Bytes { Std.string(x) }).join("+");
let failed = try { xs.slice(2, 5); "" } catch (e) { e as Bytes };
Std.string(xs) + " " + Std.string(sorted) + " " + Std.string(more) + " "
  + Std.string(joined) + " " + Std.string(total) + " " + named + " " + failed
```

## std.core.Object

| Function | Description |
| --- | --- |
| `len(o: Object): I32` | the number of own fields |
| `isEmpty(o: Object): Bool` | whether `o` has no own fields |
| `merge(o: Object, other: Object): Object` | sets every field of `other` on `o`, replacing any `o` has; `o` |
| `values(o: Object): Array<Dynamic>` | the values of the own fields, in the order of `keys` |

The other functions of Objects, such as `get`, `keys` and `copy`, are
[intrinsics](01-builtins.md#objects).

```jello
// expect: 2 false {a: 5, b: 2} 7
let o = { a: 1, b: 2 };
let n = o.len();
o.merge({ a: 5 });
let sum = o.values().reduce(0, fn(total: I32, v: Dynamic): I32 { total + (v as I32) });
Std.string(n) + " " + Std.string(o.isEmpty()) + " " + o.toString() + " " + Std.string(sum)
```

## std.core.RegExp

Regular expressions use PCRE2 syntax. A regexp and a match are native handles held as `Dynamic`;
a failed match is `null`. A literal `~r/pattern/flags` is a regexp compiled once, whose pattern and
flags the compiler checks; see [regular expressions](../04-expressions/05-regular-expressions.md).
This module needs an import:

```text
import std.core.RegExp as RegExp;
import { find, group, replace } from std.core.RegExp;
```

### Regexps

| Function | Description |
| --- | --- |
| `compile(pattern: Bytes, flags: Bytes): Dynamic` | a regexp; flags are any of `i`, `m`, `s`, `x` and `U`; an invalid pattern or flag throws what is wrong with it |
| `source(re: Dynamic): Bytes` | the pattern |
| `flags(re: Dynamic): Bytes` | the flags |
| `escapeLiteral(s: Bytes): Bytes` | a pattern matching `s` itself, under any flags |

### Matching

| Function | Description |
| --- | --- |
| `matches(s: Bytes, re: Dynamic): Bool` | whether `re` matches anywhere in `s` |
| `fullMatch(s: Bytes, re: Dynamic): Bool` | whether `re` matches the whole of `s` |
| `find(s: Bytes, re: Dynamic): Dynamic` | the first match in `s`, or `null` |
| `matchPrefix(s: Bytes, re: Dynamic): Dynamic` | a match at the start of `s`, or `null` |
| `advance(s: Bytes, m: Dynamic): Bytes` | `s` after the prefix that `m`, a match at its start, covers |
| `split(s: Bytes, re: Dynamic): Array<Bytes>` | the parts between matches, as JavaScript's `split` gives them |
| `replace(s: Bytes, re: Dynamic, with: Bytes): Bytes` | `s` with every match replaced |

In a replacement, `$0` to `$99` stand for captures by number, `${name}` for a named capture and
`$$` for `$`. An unset capture is empty; an unknown one traps.

### Reading a match

| Function | Description |
| --- | --- |
| `len(m: Dynamic): I32` | the length in bytes of the whole match |
| `captureCount(m: Dynamic): I32` | the number of groups, counting the whole match as group 0 |
| `group(m: Dynamic, n: I32): Bytes` | group `n`'s text |
| `start(m: Dynamic, n: I32): I32` | the byte index where group `n` starts |
| `named(m: Dynamic, name: Bytes): Bytes` | the group named `name`, `(?<name>...)`; empty when unset; an unknown name traps |

```jello
// expect: 12-34 34 5 3 | 10/2024 | ["a", "b", "c"] | true false
import std.core.RegExp as RegExp;

let m = RegExp.find("x 12-34 y", ~r/(\d+)-(\d+)/);
let first = RegExp.group(m, 0) + " " + RegExp.group(m, 2) + " " + Std.string(RegExp.start(m, 2))
  + " " + Std.string(RegExp.captureCount(m));
let date = RegExp.replace("2024-10", ~r/(?<y>\d+)-(?<m>\d+)/, "${m}/${y}");
let words = RegExp.compile("[a-z]+", "i");
first + " | " + date + " | " + Std.string(RegExp.split("a1b22c", ~r/\d+/)) + " | "
  + Std.string(RegExp.fullMatch("Hello", words)) + " " + Std.string(RegExp.matches("123", words))
```

### Scanning a text

A scanner that steps through a long input should not check it again at every step. `text` copies
Bytes once and checks it is valid UTF-8; `matchAt` and `findAt` then match it from a byte index.
Invalid UTF-8, or an index inside a character, is a runtime failure.

| Function | Description |
| --- | --- |
| `text(s: Bytes): Dynamic` | a text of `s` |
| `matchAt(t: Dynamic, re: Dynamic, at: I32): Dynamic` | a match starting at byte `at`, or `null` |
| `findAt(t: Dynamic, re: Dynamic, at: I32): Dynamic` | the first match from byte `at` on, or `null` |

```jello
// expect: let|x|=|42
import { text, matchAt, len, group } from std.core.RegExp;

let source = "let x = 42";
let t = text(source);
let tokens = Array.new<Bytes>(0);
let at = 0;
while (at < source.len()) {
  let m = matchAt(t, ~r/\s+|[a-z]+|\d+|=/, at);
  if (m == null) { break; }
  let token = group(m, 0);
  if (token.trim() != "") { tokens.push(token); }
  at = at + len(m);
}
tokens.join("|")
```
