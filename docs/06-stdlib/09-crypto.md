# Cryptography

The `std.crypto` package has digests and message authentication codes, keys derived from passwords
and secrets, random values fit for keys and tokens, authenticated encryption, and the PEM and
X.509 forms of certificates. Keys, digests and sealed data are raw `Bytes`;
[`toHex` and `toBase64`](02-core.md#hexadecimal-and-base64) give them as text. Each module is
imported by name, and `Pem` and `X509` also by their types when a program names them:

```text
import std.crypto.X509 as X509;
import type { Certificate } from std.crypto.X509;
```

`Hash`, `Cipher` and `X509` run on Mbed TLS 3.5.1, in the native library `tls.jdll` that
[`std.net.Tls`](05-net.md#stdnettls) uses, so a program importing them needs it; `Random` and `Pem`
do not.

## std.crypto.Hash

Algorithms are named `"md5"`, `"sha1"`, `"sha224"`, `"sha256"`, `"sha384"` and `"sha512"`; any
other name throws. MD5 and SHA-1 are broken for signatures and certificates, so are only for
checksums and older protocols.

| Function | Gives |
| --- | --- |
| `digest(algorithm: Bytes, data: Bytes): Bytes` | the digest of `data` |
| `md5(data)`, `sha1(data)`, `sha256(data)`, `sha384(data)`, `sha512(data)` | the digest by that algorithm |
| `size(algorithm: Bytes): I32` | the bytes in its digests, as 32 for `"sha256"` |
| `hmac(algorithm: Bytes, key: Bytes, data: Bytes): Bytes` | the HMAC (RFC 2104) of `data` under `key` |
| `equal(a: Bytes, b: Bytes): Bool` | whether `a` and `b` are equal, in a time that depends only on their lengths |
| `pbkdf2(algorithm, password, salt, iterations: I32, length: I32): Bytes` | `length` bytes of key from a password, by PBKDF2 (RFC 8018) |
| `hkdf(algorithm, key, salt, info, length: I32): Bytes` | `length` bytes of key from a random secret, by HKDF (RFC 5869) |

Compare a code received with the one expected by `equal`, not `==`: `==` stops at the first byte
that differs, so how long it takes tells an attacker how much of a forged code is right.

```jello
// expect: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad true
import std.crypto.Hash as Hash;

let key = "a shared secret";
let code = Hash.hmac("sha256", key, "pay 10 to bob");
let received = Hash.hmac("sha256", key, "pay 10 to bob");
Hash.sha256("abc").toHex() + " " + Std.string(Hash.equal(code, received))
```

PBKDF2 makes a key from a password slowly, so that guessing passwords is slow too: give it a
random salt of 16 bytes or more, kept with what the key protects, and as many iterations as can be
afforded, such as 600000 for SHA-256. HKDF is quick, for a secret that is already random, such as
one two parties agreed; its `info` names what each key is for, so that one secret gives several
keys. It gives at most 255 digests' worth.

## std.crypto.Random

Random values from the system's entropy, which nobody can guess or repeat. `Math.random` is
faster, for simulations and games, but can be predicted.

| Function | Gives |
| --- | --- |
| `bytes(count: I32): Bytes` | `count` random bytes |
| `int(low: I32, high: I32): I32` | a whole number from `low` to `high`, both included, each as likely |
| `uuid(): Bytes` | a random UUID (RFC 9562, version 4), as `"7d444840-9dc0-4c8d-9c4e-5d7f0b6e1a2f"` |

```jello
// expect: 32 43 true 36
import std.crypto.Random as Random;

let token = Random.bytes(32).toBase64(true);
let die = Random.int(1, 6);
Std.string(Random.bytes(32).len()) + " " + Std.string(token.len()) + " "
  + Std.string((die >= 1) && (die <= 6)) + " " + Std.string(Random.uuid().len())
```

## std.crypto.Cipher

Authenticated encryption: `seal` hides data and adds a 16-byte tag, and `open` gives it back only
when neither it nor its *associated data*, such as a header sent in the clear, has changed;
otherwise it throws. Ciphers are named `"aes-128-gcm"`, `"aes-256-gcm"` and
`"chacha20-poly1305"`. A key is 16 bytes for AES-128 and 32 for the others, and a nonce is 12.

| Function | Gives |
| --- | --- |
| `seal(algorithm, key, nonce, data, associated = ""): Bytes` | `data` encrypted, then the tag; 16 bytes longer |
| `open(algorithm, key, nonce, sealed, associated = ""): Bytes` | the data `sealed` holds |
| `keySize(algorithm: Bytes): I32` | the bytes in its keys |
| `NONCE_SIZE`, `TAG_SIZE` | 12 and 16 |

A nonce must never be used twice with one key, as that gives away both messages: take a fresh one
from `Random.bytes(12)` for each message and send it with the sealed data.

```jello
// expect: Saturday 24 February 2024, 09:30 48 Cipher.open: the data is not as it was sealed
import std.crypto.Cipher as Cipher;
import std.crypto.Random as Random;

let key = Random.bytes(32);
let nonce = Random.bytes(12);
let sealed = Cipher.seal("aes-256-gcm", key, nonce, "Saturday 24 February 2024, 09:30", "to: bob");
let opened = Cipher.open("aes-256-gcm", key, nonce, sealed, "to: bob");
let changed = try { Cipher.open("aes-256-gcm", key, nonce, sealed, "to: eve") } catch (e) { Std.string(e) };
opened + " " + Std.string(sealed.len()) + " " + changed
```

## std.crypto.Pem

PEM (RFC 7468) is the text form of certificates and keys: Base64 between lines such as
`-----BEGIN CERTIFICATE-----` and `-----END CERTIFICATE-----`, whose label says what it holds.

| Function | Gives |
| --- | --- |
| `decode(text: Bytes): Array<Block>` | the blocks in `text`, each with its `label` and `data`; text around them is left out |
| `encode(label: Bytes, data: Bytes): Bytes` | `data` as a block, in lines of 64 |

`decode` throws for a block without its end line, or not in Base64.

```jello
// expect: 1 NOTE Saturday 24 February 2024
import std.crypto.Pem as Pem;

let text = "Kept for the record:\n" + Pem.encode("NOTE", "Saturday 24 February 2024");
let blocks = Pem.decode(text);
Std.string(blocks.len()) + " " + blocks[0].label + " " + blocks[0].data
```

## std.crypto.X509

X.509 certificates bind a name to a public key under the signature of an issuer. `parse` reads
them from PEM, one or more, or DER, one, into `Certificate`s; it throws when there is none or one
cannot be read. Names read as `"CN=example.com, O=Example"`.

| Field or method | Gives |
| --- | --- |
| `subject`, `issuer: Bytes` | whom it is for, and who signed it |
| `serial: Bytes` | its serial number, in hexadecimal |
| `notBefore`, `notAfter: Date` | when it starts and stops being valid, in UTC |
| `names: Array<Bytes>` | the DNS names and IP addresses it is for (its subject alternative names) |
| `isCa: Bool` | whether it may sign other certificates |
| `key: Bytes` | its key's kind and size, as `"RSA 2048"` or `"EC 256"` |
| `der: Bytes` | it in DER, its binary form |
| `isValidAt(date: Date): Bool` | whether `date` is from `notBefore` to `notAfter` |
| `fingerprint(algorithm = "sha256"): Bytes` | its digest, which identifies it |
| `pem(): Bytes` | it in PEM |

`verify(chain, trusted, host = "", ?at)` checks `chain`, a certificate then those that signed it
in turn: that it leads to a certificate in `trusted`, or, when `trusted` is empty, to one the
system trusts; that each on the way is valid at the date `at`, or now; and, unless `host` is
empty, that the first is for `host`, a DNS name or IP address. It returns what is wrong, so an empty array means
the chain can be trusted.

```jello
// expect: CN=Jello Test CA, O=Jello true 2024-03-01T00:00:00Z EC 256 | ok | the certificate is not for example.com
import std.crypto.X509 as X509;
import std.sys.Date as Date;

let pem = "-----BEGIN CERTIFICATE-----\n"
  + "MIIBgjCCASigAwIBAgICEAEwCgYIKoZIzj0EAwIwKDEWMBQGA1UEAwwNSmVsbG8g\n"
  + "VGVzdCBDQTEOMAwGA1UECgwFSmVsbG8wHhcNMjQwMzAxMDAwMDAwWhcNMzQwMzAx\n"
  + "MDAwMDAwWjAoMRYwFAYDVQQDDA1KZWxsbyBUZXN0IENBMQ4wDAYDVQQKDAVKZWxs\n"
  + "bzBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABF1dPebQl2gC0ngqHji4e8F4SeDd\n"
  + "r2VC4sjcq3o4VSNIFRuVgh1NRtxG8NbdGO1A17WcNAxQ2SmN4+tqhkNU9++jQjBA\n"
  + "MB0GA1UdDgQWBBSB0UDtCcjQqFtnclbt33SGDe9lsjAPBgNVHRMBAf8EBTADAQH/\n"
  + "MA4GA1UdDwEB/wQEAwIBBjAKBggqhkjOPQQDAgNIADBFAiEAm7oE0H1OxuPrhQa9\n"
  + "wP2/PHPWR5cjoNry8neP4uTaV/wCIFkqpUzfwZLGSvxb7/aa57M2dF6w90eHKrE8\n"
  + "mvtowwVG\n"
  + "-----END CERTIFICATE-----\n";
let ca = X509.parse(pem)[0];
let march = Date.utc(2024, 3, 15);
let problems = X509.verify([ca], [ca], "", march);
ca.subject + " " + Std.string(ca.isCa) + " " + ca.notBefore.iso() + " " + ca.key + " | "
  + (problems.len() == 0 ? "ok" : Bytes.join(problems, "; ")) + " | "
  + Bytes.join(X509.verify([ca], [ca], "example.com", march), "; ")
```

The problems `verify` reports are:

- `a certificate is not valid yet` and `a certificate has expired`, for any on the way to the
  trusted one, that one included;
- `the certificate is not for <host>`;
- `the chain does not lead to a trusted certificate`, which a bad signature gives too;
- that a certificate is missing or revoked, or has a key usage that does not allow it, a hash or
  key of a kind that is not accepted, or a key that is too short (RSA keys must have 2048 bits or
  more, and signatures use SHA-256 or longer).

[`std.net.Tls`](05-net.md#stdnettls) checks a server's certificates this way as it connects, at
the time it connects.
