# Bits

A `bits[...]` expression packs integers, floats and bytes into a new `Bytes` value, segment by
segment, in a fixed binary layout. The same notation, used as a pattern, takes `Bytes` apart again.
For building bytes one at a time, a [byte buffer](#byte-buffers) from `Bytes.new` can be written in
place.

```jello
// expect: 6 [1, 0, 2, 0, 0, 0]
let header = bits[u16(1), u32.le(2)];
Std.string(header.len()) + " " + Std.string([header[0], header[1], header[2], header[3], header[4], header[5]])
```

## Segments

A `bits[...]` expression holds one or more segments, separated by commas, with an optional trailing
comma. Each segment is a type followed by its value in parentheses, `type(value)`.

| Segment type | Width | Value |
| --- | --- | --- |
| `1` to `8` | that many bits | an integer |
| `16`, `32` | 16 or 32 bits | an integer |
| `u8` | 8 bits | an integer |
| `u16` | 16 bits | an integer |
| `u32`, `i32` | 32 bits | an integer |
| `f32` | 32 bits | an `F32`, in IEEE 754 single precision |
| `rest` | the length of the value | `Bytes` |

The named types can also be written in capitals, as `U16` or `F32`. Any other width, such as `12`,
is an error.

An integer segment's value is an `I32`, or an integer of a narrower type, which widens to `I32`; an
unsuffixed literal is typed to fit. A literal that does not fit an unsigned segment of fewer than
32 bits is an error. Any other value keeps only its low bits: `16(70000)` is not allowed, but a
variable holding 70000 packs as 4464. A 32-bit segment holds all 32 bits of an `I32`, so `i32(-2)`
and `32(-2)` pack the same bytes.

```jello
// error: 2:9: 256 does not fit in 8 bits [E0007]
let b = bits[u8(256)];
b
```

```jello
// error: 3:16: this segment holds I32, not I64 [E0400]
let big: I64 = 1;
let b = bits[8(big)];
b
```

### Byte order

A segment of 16 or 32 bits takes a byte order suffix, `.le` for little-endian, least significant
byte first, or `.be` for big-endian, most significant byte first. Without one, it is little-endian.
A segment of 8 bits or fewer and a `rest` segment have no byte order, and a suffix on them is an
error.

```jello
// expect: [2, 1, 2, 1, 1, 2]
let b = bits[16(258), u16.le(258), U16.be(258)];
Std.string([b[0], b[1], b[2], b[3], b[4], b[5]])
```

```jello
// error: 2:15: ParseError: endian suffix needs a 16- or 32-bit segment, not 8 bits or fewer, or rest [E0104]
let b = bits[4.be(1), 4(0)];
```

### Sub-byte segments

Segments of 1 to 7 bits pack into a byte from its most significant bit down. A segment of 8 or more
bits, and a `rest` segment, must start on a byte boundary, and the fixed segments together must
fill whole bytes.

```jello
// expect: 69
// 0100 0101: the high nibble is 4 and the low one 5.
let b = bits[4(4), 4(5)];
Std.string(b[0])
```

```jello
// error: 2:9: the segments of a bits value must fill whole bytes [E0104]
let b = bits[4(1)];
b
```

```jello
// error: 2:9: a segment of 8 or more bits, or a rest, must start on a byte boundary [E0104]
let b = bits[3(1), 8(1), 5(0)];
b
```

### Floats

An `f32` segment packs an `F32` as its four IEEE 754 bytes. An unsuffixed float literal is typed as
an `F32` there.

```jello
// expect: [0, 0, 192, 63] [63, 192, 0, 0]
let le = bits[f32(1.5)];
let be = bits[f32.be(1.5)];
Std.string([le[0], le[1], le[2], le[3]]) + " " + Std.string([be[0], be[1], be[2], be[3]])
```

### rest

A `rest` segment copies a whole `Bytes` value in. There is at most one, and it must be the last
segment.

```jello
// expect: 4 99 100
let body = "cd";
let packet = bits[u16.le(2), rest(body)];
Std.string(packet.len()) + " " + Std.string(packet[2]) + " " + Std.string(packet[3])
```

```jello
// error: 2:9: a rest segment must be the last [E0104]
let b = bits[8(1), rest("ab"), 8(2)];
b
```

## Bit patterns

A `bits[...]` pattern matches `Bytes` of the right length and binds or tests each segment. A pattern
without a `rest` segment matches only `Bytes` exactly as long as its fixed segments; with one, it
matches any value at least that long, and the `rest` binds what follows, which may be empty. The
length is tested before any segment is read, so a value that is too short or too long falls to the
next arm.

An integer segment of fewer than 32 bits reads an unsigned value, from 0 up; a 32-bit segment reads
its bits as an `I32`.

```jello
// expect: 258 5 2 cd
let packet = bits[16.be(258), 8(5), rest("cd")];
match (packet) {
  bits[16.be(kind), 8(length), rest(body)] => Bytes.join([Std.string(kind), Std.string(length),
    Std.string(body.len()), body], " "),
  _ => "short",
}
```

A segment in a pattern can hold any pattern: a literal to test, a name to bind, or `_`. Because a
bits pattern can fail, a plain `let` cannot use one; use `if (let ...)`, `let ... else` or `match`.
Patterns are described in full in [Patterns](../03-statements/03-patterns.md).

```jello
// expect: 122 -1
let lowest = fn(raw: Bytes): I32 {
  let bits[u8(only)] = raw else { return -1; };
  only
};
Std.string(lowest("z")) + " " + Std.string(lowest("zz"))
```

A typedef can name a binary layout once, for both packing and matching; see
[Typedefs and records](../02-types/06-typedefs-and-records.md).

## Byte buffers

`Bytes` values are immutable. To build bytes in place, `Bytes.new(length)` makes a buffer of that
many zero bytes, of type `ByteBuffer`. A buffer is indexed like an array of bytes: `buffer[i]` reads
a byte as an `I32`, and `buffer[i] = byte`, `buffer.set_u8(i, byte)` and the compound assignments
write one. An index outside the buffer, or a byte outside 0 to 255, traps with `BOUNDS`.

Bytes leave a buffer only by copying: `buffer.toString()` copies all of it into a new `Bytes`, and
`Bytes.slice(buffer, start, length)` copies part. Later writes to the buffer do not change the copy.

```jello
// expect: ABC AB
let buffer = Bytes.new(3);
buffer[0] = 65;
buffer.set_u8(1, 66);
buffer[2] = 66;
buffer[2]++;
let all = buffer.toString();
let part = Bytes.slice(buffer, 0, 2);
buffer[0] = 90;
all + " " + part
```

A buffer is not `Bytes`. It cannot be passed where `Bytes` is expected, concatenated with `+`,
converted with `as`, printed by `Std.string`, compared or thrown; copy its bytes out first. Its size
is fixed: to grow it, make a larger buffer and copy the bytes across.

```jello
// error: 3:19: a buffer from Bytes.new is not Bytes; copy a buffer's bytes out with buffer.toString() [E0411]
let buffer = Bytes.new(2);
let name: Bytes = buffer;
name
```

```jello
// trap: BOUNDS 3:1
let buffer = Bytes.new(2);
buffer[0] = 256;
buffer.len()
```
