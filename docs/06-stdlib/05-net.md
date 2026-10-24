# Networking

The `std.net` package has TCP and UDP sockets, TLS, and an HTTP/1.1 server and client, all built on
[processes](04-concurrent.md). A call that has to wait, such as an accept or a read, parks only
the process that makes it. Each module exports functions and types, so a program imports it both
ways:

```text
import std.net.Tcp as Tcp;
import type { Listener, Stream } from std.net.Tcp;
```

## Sockets and processes

- **A socket belongs to the process that made it**, and only that process may use it; it closes
  when that process finishes.
- **Sending moves it.** A socket sent in a message, inside an enum, or captured by a new process,
  moves to the receiver: the sender's handle then reads as closed, and the receiver owns it once
  it receives the message, or the new process from its start. A message never received closes it,
  and one that cannot be sent leaves it with the sender.
- **A socket in a global stays with its maker.** In the copy of the globals a new process starts
  with, it reads as closed, so hold sockets in locals and pass them on.
- **Failures throw Bytes** saying what went wrong, such as `Connection refused`. A call with a time
  limit returns `null` once the limit passes. Using a socket that is closed, or that belongs to
  another process, throws `socket closed, or another process's`.

## std.net.Tcp

| Function | Description |
| --- | --- |
| `listen(host: Bytes, port: I32): Listener` | a listener on `host`, `""` for every address, and `port`, 0 for any free one |
| `connect(host: Bytes, port: I32): Stream` | a stream connected to `host` and `port` |
| `connectWithin(host: Bytes, port: I32, ms: I32): ?Stream` | the same, or `null` once `ms` pass without connecting |

| `Listener` method | Description |
| --- | --- |
| `accept(): Stream` | the next connection, waiting as long as it takes |
| `acceptWithin(ms: I32): ?Stream` | the same, or `null` once `ms` pass with none |
| `port(): I32` | the port it listens on, the one chosen when it was asked for 0 |
| `close(): Bool` | stops listening; whether it was open |

| `Stream` method | Description |
| --- | --- |
| `read(max: I32): Bytes` | up to `max` bytes, as soon as there are any; empty once the other end has closed |
| `readWithin(max: I32, ms: I32): ?Bytes` | the same, or `null` once `ms` pass with nothing to read |
| `write(data: Bytes)` | writes all of `data`, waiting while the other end is slow to take it |
| `close(): Bool` | closes it; whether it was open |
| `peerHost(): Bytes`, `peerPort(): I32` | the numeric address and port of the other end |
| `localPort(): I32` | the port of its own end |

A read gives what has arrived, which may be less than the other end wrote at once, so a reader
loops until it has what it needs.

```jello
// expect: hello world|true|Connection refused
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;
import std.net.Tcp as Tcp;
import type { Listener, Stream } from std.net.Tcp;

// Echoes what S reads until its other end closes.
let echo = fn(s: Stream) {
  let data = s.read(1024);
  while (data.len() > 0) { s.write(data); data = s.read(1024); }
  s.close();
};

let run = fn(): Bytes {
  let listener = Tcp.listen("127.0.0.1", 0);
  let port = listener.port();
  Process.spawn<I32>(fn(_: Inbox<I32>) {
    while (true) {
      let s = listener.accept();
      Process.spawn<I32>(fn(_: Inbox<I32>) { echo(s); });
    }
  });

  let client = Tcp.connect("127.0.0.1", port);
  client.write("hello ");
  client.write("world");
  let got = "";
  while (got.len() < 11) { got = got + client.read(64); }
  let quiet = client.readWithin(64, 20) == null;
  client.close();

  let closed = Tcp.listen("127.0.0.1", 0);
  let closedPort = closed.port();
  closed.close();
  let refused = try { Tcp.connect("127.0.0.1", closedPort); "" } catch (e) { e as Bytes };
  got + "|" + Std.string(quiet) + "|" + refused
};
run()
```

## std.net.Udp

| Function | Description |
| --- | --- |
| `bind(host: Bytes, port: I32): Socket` | a socket bound to `host`, `""` for every address, and `port`, 0 for any free one |

| `Socket` method | Description |
| --- | --- |
| `sendTo(data: Bytes, host: Bytes, port: I32)` | sends `data` as one datagram |
| `receive(max: I32): Datagram` | the next datagram, up to `max` bytes of it, waiting as long as it takes |
| `receiveWithin(max: I32, ms: I32): ?Datagram` | the same, or `null` once `ms` pass with none |
| `port(): I32` | the port it is bound to, the one chosen when it was asked for 0 |
| `close(): Bool` | closes it; whether it was open |

A `Datagram` has the fields `data: Bytes`, and `host: Bytes` and `port: I32`, the numeric address
of its sender.

```jello
// expect: ping 127.0.0.1 true|pong ping|true
import std.net.Udp as Udp;
import type { Socket, Datagram } from std.net.Udp;

let run = fn(): Bytes {
  let a = Udp.bind("127.0.0.1", 0);
  let b = Udp.bind("127.0.0.1", 0);
  a.sendTo("ping", "127.0.0.1", b.port());
  let d = b.receive(64);
  let fromA = d.port == a.port();
  b.sendTo("pong " + d.data, d.host, d.port);
  let answer = a.receive(64).data;
  let none = a.receiveWithin(64, 20) == null;
  a.close();
  b.close();
  d.data + " " + d.host + " " + Std.string(fromA) + "|" + answer + "|"
    + Std.string(none)
};
run()
```

## std.net.Tls

TLS 1.2 on TCP streams, with the vendored Mbed TLS 3.5.1, in the native library `tls.jdll`: a
program importing this module needs it, and no other loads it. A configuration is one side's: a
server's certificate chain and private key, or the certificates a client trusts. Securing a stream
runs the handshake on it, and from then on its `read`, `write` and `close` carry encrypted data;
closing it tells the other end first. A client checks the server's certificate against those it
trusts and against the name it connected to. A configuration sent to another process is shared;
a secured stream moves as any stream does.

| Function | Description |
| --- | --- |
| `library(): Bytes` | the TLS library's name and version, `"Mbed TLS 3.5.1"` |
| `server(certificates: Bytes, key: Bytes): Config` | a server's configuration: a PEM certificate chain, the server's own first, and that certificate's PEM private key |
| `client(trusted: Bytes): Config` | a client's configuration, trusting the PEM certificates `trusted`, or the system's when it is `""` |
| `accept(listener: Listener, config: Config): Stream` | the listener's next connection, secured as `config`'s server |
| `connect(host: Bytes, port: I32, config: Config): Stream` | a stream connected to `host` and `port`, secured as `config`'s client of `host` |
| `secureServer(stream: Stream, config: Config): Stream` | secures a connected stream as `config`'s server, and returns it |
| `secureClient(stream: Stream, config: Config, host: Bytes): Stream` | secures a connected stream as `config`'s client of the server `host`, the name its certificate must have, which may not be empty, and returns it |

PEM that cannot be read, or a key that is not the certificate's, throws what is wrong, as Bytes.
So does a handshake that fails, such as for a certificate the client does not trust, and the
stream is then closed. The system's certificates are those in the file `SSL_CERT_FILE` names and
the `:`-separated directories `SSL_CERT_DIR` names, as OpenSSL reads them. When neither is set,
they are the first of `/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`,
`/etc/pki/tls/certs/ca-bundle.crt` and `/etc/ssl/ca-bundle.pem` there is.

```text
let server = Tls.server(File.readText("cert.pem"), File.readText("key.pem"));
let listener = Tcp.listen("", 8443);
let stream = Tls.accept(listener, server);

let api = Tls.connect("example.com", 443, Tls.client(""));
```

```jello
// expect: Mbed TLS 3.5.1: X509 - The CRT/CRL/CSR format is invalid, e.g. different type expected
import std.net.Tls as Tls;
import type { Config } from std.net.Tls;

let _system = Tls.client("");
let bad = "";
try { Tls.client("no certificate here"); } catch (e) { bad = e as Bytes; }
Tls.library() + ": " + bad
```

## std.net.Http

An HTTP/1.1 server, which runs a process for each connection and keeps it open for request after
request, and a client. It speaks plain HTTP only. Bodies are read whole, sized by `Content-Length`
or sent chunked, up to 16 MiB; the server answers a larger one with 413 and always answers with a
`Content-Length`. Header names compare without regard to ASCII case.

```text
import std.net.Http as Http;
import type { Request, Response, Header, Client } from std.net.Http;
```

### Serving

| Function | Description |
| --- | --- |
| `serve(listener: Listener, handle: (Request) -> Response)` | answers every connection `listener` accepts, each in a process of its own, with `handle` |

`serve` runs until accepting fails, as it does once the listener closes, and then throws why. A
request that `handle` throws for gets a 500. The server answers 400 to what is not HTTP/1.x and
closes the connection after a request that asks it to, or an HTTP/1.0 request without
`keep-alive`.

| Type | Fields and methods |
| --- | --- |
| `Request` | `verb: Bytes`, such as `"GET"`; `target: Bytes`, such as `"/index.html?q=1"`; `version: Bytes`; `headers: Array<Header>`; `body: Bytes`; `header(name: Bytes): ?Bytes`, the first header called `name`; `path(): Bytes`, the target up to any query |
| `Response` | `new Response(status: I32, body: Bytes)`; `status: I32`; `headers: Array<Header>`; `body: Bytes`; `withHeader(name: Bytes, value: Bytes): Response`, which adds a header and returns the response; `header(name: Bytes): ?Bytes` |
| `Header` | `new Header(name: Bytes, value: Bytes)`; `name: Bytes`, as it was sent; `value: Bytes` |

### Requesting

| Function or method | Description |
| --- | --- |
| `connect(host: Bytes, port: I32): Client` | a client connected to `host` and `port` |
| `Client.get(target: Bytes): Response` | sends a `GET` of `target` and reads the response |
| `Client.send(verb: Bytes, target: Bytes, headers: Array<Header>, body: Bytes): Response` | sends any request and reads the response |
| `Client.close()` | closes the connection |

A client sends its requests one after another on one connection, adding a `Host` header unless
the request has one. It throws when the server closes the connection before answering.

```jello
// expect: 200 hello /greet?name=jello text/plain|200 got 5 bytes|404 |500 |200 hello /again
import std.concurrent.Process as Process;
import type { Process, Inbox } from std.concurrent.Process;
import std.net.Tcp as Tcp;
import type { Listener, Stream } from std.net.Tcp;
import std.net.Http as Http;
import type { Request, Response, Header, Client } from std.net.Http;

let handle = fn(req: Request): Response {
  if (req.path() == "/missing") { return new Response(404, ""); };
  if (req.path() == "/boom") { throw "boom"; };
  if (req.verb == "POST") {
    return new Response(200, "got " + Std.string(req.body.len()) + " bytes");
  };
  new Response(200, "hello " + req.target).withHeader("Content-Type", "text/plain")
};

// RES's status and body.
let show = fn(res: Response): Bytes { Std.string(res.status) + " " + res.body };

let run = fn(): Bytes {
  let listener = Tcp.listen("127.0.0.1", 0);
  let port = listener.port();
  Process.spawn<I32>(fn(_: Inbox<I32>) { Http.serve(listener, handle); });

  let client = Http.connect("127.0.0.1", port);
  let first = client.get("/greet?name=jello");
  let kind = match (first.header("content-type")) { null => "none", value => value };
  let out = [show(first) + " " + kind];
  out.push(show(client.send("POST", "/upload", [new Header("X-Note", "demo")], "12345")));
  out.push(show(client.get("/missing")));
  out.push(show(client.get("/boom")));
  out.push(show(client.get("/again")));
  client.close();
  out.join("|")
};
run()
```
