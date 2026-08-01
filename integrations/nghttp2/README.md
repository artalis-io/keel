# Keel ⇄ nghttp2 integration

First-party adapters implementing Keel's HTTP/2 **session** vtables on top of
[nghttp2](https://github.com/nghttp2/nghttp2):

- **client** — `KlH2ClientSession` (`include/keel/h2_client.h`)
- **server** — `KlH2ServerSession` (`include/keel/h2_server.h`)

Keel's HTTP/2 code is framing-agnostic: it drives a session vtable and never
speaks HPACK/frames itself. These adapters plug nghttp2 in behind that seam.
nghttp2 is confined to the adapter `.c` files — **no nghttp2 type appears in
`keel_h2_nghttp2.h` or any Keel core header**.

This lives **outside** the dependency-light core: plain `make` / `make test`
never touch nghttp2.

## Bring your own nghttp2

nghttp2 is **not vendored and never downloaded**. Supply it yourself via
`NGHTTP2_DIR` (a system prefix or a source tree) or let the build fall back to
`pkg-config --cflags/--libs libnghttp2`.

The adapters use the **classic** nghttp2 API (`nghttp2_submit_request`,
`nghttp2_session_mem_recv`, the `ssize_t` send / data-source callbacks), which is
present and non-deprecated across a wide version range — no `*2` (`size_t`) API
is required.

### Version matrix

| nghttp2 | Status | Notes |
|---------|--------|-------|
| 1.64.x | Supported | Primary local target (Homebrew `libnghttp2`). |
| 1.59.x | Supported | Validated in CI-parity container (Ubuntu 24.04) under ASan+UBSan+LSan. |
| 1.x (≥ ~1.0) | Expected to work | Classic API only; no `*2` calls. |

> On Homebrew the headers ship in the **`libnghttp2`** formula, not `nghttp2`
> (which is the CLI tools). Use `NGHTTP2_DIR=$(brew --prefix libnghttp2)`.

## Build

```sh
make integration-nghttp2 NGHTTP2_DIR=$(brew --prefix libnghttp2)   # from repo root
# or directly:
cd integrations/nghttp2 && make NGHTTP2_DIR=$(brew --prefix libnghttp2)
```

Produces `libkeel_h2_nghttp2.a` (both client + server adapter objects).

## Link & wire up

```sh
cc app.c \
   -Iinclude -Iintegrations/nghttp2 \
   -L. -lkeel -Lintegrations/nghttp2 -lkeel_h2_nghttp2 \
   $(pkg-config --libs libnghttp2)
```

Client:

```c
KlH2ClientConfig cfg = {
    .tls     = &my_tls_cfg,
    .session = kl_h2_nghttp2_client_session,   // factory
};
KlH2ClientConn *c = kl_h2_client_connect(ev, &alloc, &cfg, url, on_err, ud);
```

Server:

```c
KlH2ServerConfig h2 = { .factory = kl_h2_nghttp2_server_session };
// wire into the server's HTTP/2 upgrade path (KlH2ServerConfig)
```

Requests are issued with `:scheme` `https` (the Keel h2 client is TLS-oriented;
h2c cleartext is out of scope for this adapter).

## Test

```sh
make integration-nghttp2 NGHTTP2_DIR=... && \
  (cd integrations/nghttp2 && make test NGHTTP2_DIR=...)
```

`test_roundtrip.c` pairs the client and server sessions **in memory** — no
sockets, no Keel event loop — feeding each side's `on_send` output into the
other's `recv`, so a full `GET / → 200` with a body is exercised through real
nghttp2 framing. It validates both adapters against each other. Run under
ASan+UBSan+LSan in the Linux container for leak/UAF coverage:

```sh
container run --rm -v "$PWD:/src" ubuntu:24.04 bash -c '
  apt-get update && apt-get install -y build-essential libnghttp2-dev pkg-config
  cd /src && cc -g -O1 -fsanitize=address,undefined \
    -Iinclude -Iintegrations/nghttp2 $(pkg-config --cflags libnghttp2) \
    integrations/nghttp2/h2_nghttp2_client.c integrations/nghttp2/h2_nghttp2_server.c \
    integrations/nghttp2/test_roundtrip.c src/allocator.c \
    $(pkg-config --libs libnghttp2) -o /tmp/rt && ASAN_OPTIONS=detect_leaks=1 /tmp/rt'
```

## Ownership notes

- **Client:** per-stream state (accumulated response headers + a copy of the
  request body) hangs off nghttp2's per-stream `user_data` and is freed exactly
  once on stream close. The request body is copied at submit time (Keel's
  copied-immediately write contract), so the caller may free it at once.
- **Server:** request headers are accumulated per stream and delivered in a
  single `on_request` call (the driver copies out during that call); response
  bodies are copied at submit time because nghttp2 pulls DATA frames
  asynchronously, and freed on stream close.
- **Short writes:** nghttp2's send callback re-queues whatever the Keel `send`
  callback leaves unsent (`return < len` → nghttp2 buffers the tail), so frames
  are never truncated under backpressure.

## Compatibility promise

Source + static-relink only. The adapters track the public `KlH2ClientSession` /
`KlH2ServerSession` vtables in `include/keel/`.
