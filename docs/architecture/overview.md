# KEEL — Architecture

The current-state entry point to KEEL's design. Start with the three-axis model below; the rest of
this document is the HTTP-server internals reference (state machines, memory model, zero-copy, TLS).
The *rules* those internals must preserve are in
[architecture_invariants.md](invariants.md); the dated verification history is in
[audits/README.md](../archive/audits/README.md); the consolidation plan is
[keel_improvement_roadmap.md](../roadmap/roadmap.md).

## Architecture at a glance — Transport / Engine / Provider

KEEL's networking is three **orthogonal, independently replaceable axes**. Protocols sit above all
three and touch no platform socket API or event engine directly.

```text
        application  →  protocols (HTTP/1.1, HTTP/2, WebSocket, SSE, DNS, TLS-above-stream)
                            │
                            ▼
   Transport   KlListener        KlStream          KlDatagram        ← semantic contracts (Tier-1)
   (semantics) (accept path)     (raw byte stream) (bounded message)
                            │
              ┌─────────────┴─────────────┐
              ▼                            ▼
   Engine     readiness  |  completion     Provider   POSIX | Winsock | lwIP | EFI
   (model)    epoll/kqueue/  io_uring/      (stack)    sockets        raw    EFI_*
              WSAPoll/poll   IOCP/pollcomp
```

- **Transport axis** — the three **Tier-1 semantic primitives**, each a `STABLE` public API whose
  *function + ownership contract* is committed and whose struct layout is opt-in/unstable
  (`*_detail.h`):
  - [`KlListener`](../../include/keel/listener.h) — accept-path state machine (credit reservation,
    lease handoff, confirmed-detachment close). Contract: [transport_surface.md](public_api.md).
  - [`KlStream`](../../include/keel/stream.h) — raw byte transport (bounded write queue, strict
    read pause/resume, graceful/abortive close). Contract: [stream_contract.md](../contracts/stream.md).
  - [`KlDatagram`](../../include/keel/datagram.h) — bounded message transport (fixed-slot admission,
    message boundaries, source/local metadata). Contract: [datagram_contract.md](../contracts/datagram.md).
    Extended-UDP features (batching, GSO/GRO, multicast, per-packet TOS/ECN) are capabilities of
    `KlDatagram` itself (`KL_DGRAM_CAP_*`); the earlier separate byte-budget UDP object was
    consolidated into it and removed. The historical two-object rationale is archived in
    [datagram_vs_udp.md](../archive/designs/datagram_vs_udp.md).
- **Engine axis** — readiness ([event.h](../../include/keel/event.h): register interest → wait →
  re-arm) and completion ([completion.h](../../src/completion.h): submit owned op → track → retire) are
  *peer models*, negotiated by capability ([event_caps.h](../../src/event_caps.h)). The **production**
  backends preserve their native model (epoll/kqueue/WSAPoll/poll are readiness; io_uring/IOCP are
  completion) — neither is emulated in terms of the other. The sole deliberate exception is
  **pollcomp**, a portable *test double* that implements the completion contract over `poll()` so the
  completion driver can run under ASan on any POSIX host; it is a CI/testing backend, not a
  production one. Details: [event_provider_design.md](../archive/designs/event_provider_design.md).
- **Provider axis** — the network stack behind [socket.h](../../src/socket.h)
  (`KlSocketProvider`): POSIX [socket_posix.c](../../src/socket_posix.c), Winsock, and the
  `integrations/` adapters (lwIP, EFI). Providers are transport-mechanical — no protocol knowledge.

**Dependency direction (one way).** New protocols depend *downward* only:
`protocol → Tier-1 transport → driver/adapter → engine + provider`. A protocol TU reaches the
network through `KlListener`/`KlStream`/`KlDatagram` and the socket/event abstractions — never by
including a platform networking/event header or the raw completion seam, and never by calling an
engine directly. A lower seam is used only when a missing semantic is documented and reviewed. This
is invariant I10, enforced by `make check-tier1-boundary` (the complement of `check-sockaddr-neutral`).
The gate is **default-deny**: every `src`/`src/protocols` TU is governed except an allowlisted `TIER1_INFRA`
layer (event backends, socket providers, platform glue, the completion driver/adapters, the transport
machines, and the run-loop/async-connect drivers `http_server.c` / `http_client_async.c`) — so a newly added
protocol TU is covered automatically, and only infrastructure may include a backend header.

Every combination's runtime status is the compatibility matrix in
[capability_matrix.md](../operations/capability_matrix.md) and the axis audit's matrix in
[keel_axis_audit.md](../archive/audits/keel_axis_audit.md). The invariants that keep the axes separate — and keep
completion lifetimes safe — are enumerated in [architecture_invariants.md](invariants.md).

> The diagrams and sections below predate the three-axis vocabulary and describe the **HTTP server
> data path** specifically (readiness/POSIX). They remain accurate for that path; read them as the
> protocol-layer detail beneath the Transport axis.

## System Overview

```
  ┌────────────────────────── Server Side ──────────────────────────────────┐
  │                           KlHttpServer                                      │
  │  ┌──────────────────────────────────────────────────────┐              │
  │  │                KlEventCtx (s.ev)                      │              │
  │  │  ┌──────────┐   ┌──────────┐   ┌──────────────┐     │              │
  │  │  │ EventLoop│   │Allocator │   │ Watchers     │     │              │
  │  │  │ (kqueue/ │   │ (pluggable│   │ (FD callback │     │              │
  │  │  │  epoll/  │   │  vtable) │   │  linked list)│     │              │
  │  │  │  iouring)│   └──────────┘   └──────────────┘     │              │
  │  │  └──────────┘                                        │              │
  │  └──────────────────────────────────────────────────────┘              │
  │                      │                                                  │
  │  ┌───────────┐  ┌────┴───┐  ┌──────────┐  ┌────────────┐             │
  │  │ ConnPool  │  │ Router │  │ThreadPool│  │ KlAsyncOp  │             │
  │  │(pre-alloc)│  │ (scan) │  │ (workers)│  │ (suspend/  │             │
  │  └─────┬─────┘  └────┬───┘  └──────────┘  │  resume)   │             │
  │        │              │                     └────────────┘             │
  │        ▼              ▼                                                │
  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐           │
  │  │   TLS    │  │ Handler  │  │ Response │  │ BodyReader │           │
  │  │(vtable)  │  │(user fn) │  │ (buffer/ │  │  (vtable)  │           │
  │  └──────────┘  └──────────┘  │  file/   │  └────────────┘           │
  │                               │  stream) │                            │
  │  ┌───────────────────┐       └──────────┘                            │
  │  │  RequestParser    │                                                │
  │  │  (llhttp vtable)  │                                                │
  │  └───────────────────┘                                                │
  └────────────────────────────────────────────────────────────────────────┘

  ┌───────────── Client Side (standalone or embedded) ─────────────────────┐
  │                                                                         │
  │  KlHttpClient (async)          kl_http_client_request (sync, blocking)          │
  │  ┌──────────────────┐     ┌────────────────────────────────┐          │
  │  │ KlEventCtx *     │     │ socket → connect → send →     │          │
  │  │ state machine:   │     │ recv → parse → return          │          │
  │  │  CONNECTING →    │     └────────────────────────────────┘          │
  │  │  TLS_HANDSHAKE → │                                                  │
  │  │  SENDING →       │     KlHttp1ResponseParser (llhttp vtable)            │
  │  │  RECEIVING →     │     KlUrl (URL parser, CRLF guard)              │
  │  │  DONE            │                                                  │
  │  └──────────────────┘                                                  │
  └────────────────────────────────────────────────────────────────────────┘
```

**Server request flow**: socket → event loop (readiness) → connection (read) → **TLS decrypt** → request parser (headers) → router (match) → body reader (data) → handler → response (send) → **TLS encrypt** → socket

**Client request flow**: `kl_url_parse` → socket (non-blocking connect) → **TLS handshake** → send request → receive response → response parser → `KlHttpClientResponse`

### KlEventCtx — Composable Event Context

`KlEventCtx` contains the event loop, allocator, and watcher list. It is embedded in `KlHttpServer` via `s.ev`, but can also be used standalone — the async HTTP client and thread pool only need `KlEventCtx`, not a full server. This enables client-only programs that share the same event loop.

## Event Loop Abstraction

KEEL provides a unified event API over four backends:

```c
int  kl_event_init(KlEventLoop *loop);
int  kl_event_add(KlEventLoop *loop, int fd, KlEventMask mask, void *udata);
int  kl_event_mod(KlEventLoop *loop, int fd, KlEventMask mask, void *udata);
int  kl_event_del(KlEventLoop *loop, int fd);
int  kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);
void kl_event_close(KlEventLoop *loop);
```

The kqueue, epoll, and io_uring backends use **edge-triggered** semantics — the event fires once when a fd becomes ready, not continuously while it's ready. This means fewer syscalls under load but requires draining the fd completely on each notification. The poll backend uses **level-triggered** semantics (standard POSIX `poll(2)` behavior).

| Backend | Platform | Mechanism |
|---------|----------|-----------|
| **kqueue** | macOS, BSD | `EV_ADD \| EV_CLEAR` (edge-triggered) |
| **epoll** | Linux | `EPOLLET \| EPOLLIN \| EPOLLOUT` |
| **io_uring** | Linux 5.6+ | `IORING_OP_POLL_ADD` (readiness, not async I/O) |
| **poll** | Any POSIX | `poll(2)` (level-triggered, universal fallback) |

The io_uring backend uses poll-add mode (readiness notification), not async read/write. This makes it a drop-in for epoll with io_uring's batched submission advantage, without requiring a fundamentally different I/O model.

The server processes up to `KL_EVENTS_PER_TICK` (64) events per `kl_event_wait` call, with a poll timeout of `KL_POLL_TIMEOUT_MS` (1000ms) to ensure the timeout sweep runs regularly.

## Connection State Machine

```
                    accept()
                       │
                       ▼
            ┌───────────────────┐
            │  TLS_HANDSHAKE    │ (if TLS configured)
            │  (non-blocking)   │
            └────────┬──────────┘
                     │ KL_TLS_OK
                     ▼
               ┌──────────────┐
               │   READING    │◄──────────────────┐
               │  (headers)   │                    │
               └──────┬───────┘                    │
                      │ HEADERS_OK                 │ keep-alive
                      ▼                            │ (reset)
              ┌───────────────┐                    │
              │ READING_BODY  │                    │
              │ (body reader) │                    │
              └───────┬───────┘                    │
                      │ message complete           │
                      ▼                            │
              ┌───────────────┐                    │
              │  PROCESSING   │                    │
              │   (handler)   │                    │
              └───────┬───────┘                    │
                      │ handler returns            │
                      ▼                            │
              ┌───────────────┐                    │
              │   SENDING     │────────────────────┘
              │  (response)   │
              └───────┬───────┘
                      │ !keep_alive || error
                      ▼
              ┌───────────────┐
              │    CLOSED     │
              └───────────────┘
```

**State transitions in detail:**

- **READING → READING_BODY**: Parser returns `KL_HTTP1_PARSE_HEADERS_OK`. Route is matched. If the route has a body reader factory and the request has a body (Content-Length > 0 or chunked), the factory is called to create a reader. Any remaining data in the read buffer is fed to the body reader immediately.

- **READING → PROCESSING**: Parser returns `KL_HTTP1_PARSE_OK` (complete message, no body). Route is matched, handler is called directly.

- **READING_BODY → PROCESSING**: Parser returns `KL_HTTP1_PARSE_OK` (body complete). `on_complete` is called on the body reader. Handler is invoked.

- **PROCESSING → SENDING**: Handler returns. Response is assembled and send begins. For buffer responses, `writev` sends headers + body atomically. For file responses, headers are sent first, then `sendfile`. For streaming, the handler already wrote via the write callback.

- **SENDING → READING**: Keep-alive is set and response fully sent. Connection is reset (response cleared, parser reset, read buffer compacted) and returns to READING for the next request.

- **SENDING → CLOSED**: Not keep-alive, or error during send. Connection is released back to the pool.

- **Any state → CLOSED**: Timeout sweep detects idle connection, read/write returns error, or pool needs to reclaim the slot.

## Two-Phase Parsing

Headers are parsed first, then the body — separately. This is critical because:

1. **Routing needs headers.** The path and method must be known before the body arrives to select the right handler and body reader factory.

2. **Header pointers point into read_buf.** `KlHttpRequest.path`, `KlHttpRequest.method`, and all header name/value pointers are direct pointers into the connection's `read_buf`. No allocation, no copying. These pointers are valid as long as the read buffer isn't overwritten.

3. **Body reading may overwrite read_buf.** For bodies larger than `KL_HTTP_CONN_READ_BUF_SIZE` (8192 bytes), the connection reuses the read buffer as a sliding window. Once body reading starts, the header region of read_buf may be overwritten. This is why the body reader factory receives the request while header pointers are still valid — it can extract Content-Type, Content-Length, etc. during construction.

The parser vtable signals this with `KL_HTTP1_PARSE_HEADERS_OK` (headers complete, body pending) vs `KL_HTTP1_PARSE_OK` (entire message complete).

## Body Reader Vtable

```c
struct KlHttpBodyReader {
    int  (*on_data)(KlHttpBodyReader *self, const char *data, size_t len);
    void (*on_complete)(KlHttpBodyReader *self);
    void (*on_error)(KlHttpBodyReader *self);
    void (*destroy)(KlHttpBodyReader *self);
};
```

**Factory pattern:**

```c
typedef KlHttpBodyReader *(*KlHttpBodyReaderFactory)(KlAllocator *alloc,
                                              KlHttpRequest *req,
                                              void *user_data);
```

The factory is called after `KL_HTTP1_PARSE_HEADERS_OK`, while header pointers are valid. It inspects the request (Content-Type, Content-Length) and either:
- Returns a reader — body data will be fed to `on_data`
- Returns NULL — KEEL sends 415 Unsupported Media Type

**Data flow:**

1. Parser calls `on_data` on the body reader for each chunk of body data
2. The reader accumulates, parses, or streams the data
3. When the parser signals message complete, `on_complete` is called
4. The handler accesses the finished reader via `req->body_reader`
5. After the handler returns, `destroy` is called

If `on_data` returns `-1`, the parse is aborted and KEEL sends 413 Payload Too Large.

**Built-in readers:**

| Reader | Factory | user_data | Behavior |
|--------|---------|-----------|----------|
| `KlHttpBufReader` | `kl_http_body_reader_buffer` | `(void *)(size_t)max_size` | Accumulates into growable buffer. 0 = unlimited. |
| `KlHttpMultipartReader` | `kl_http_body_reader_multipart` | `KlHttpMultipartConfig *` | RFC 2046 multipart/form-data parser. NULL = defaults. |

## Multipart State Machine

```
    on_data()
       │
       ▼
  ┌──────────┐     find "\r\n--boundary"     ┌─────────────────┐
  │ PREAMBLE │──────────────────────────────►│ AFTER_BOUNDARY  │
  └──────────┘                                └────────┬────────┘
                                                       │
                                              "\r\n" ──┤── "--\r\n"
                                                       │        │
                                                       ▼        ▼
                                              ┌──────────┐  ┌──────┐
                                              │ HEADERS  │  │ DONE │
                                              └────┬─────┘  └──────┘
                                                   │ "\r\n\r\n"
                                                   ▼
                                              ┌──────────┐
                                              │  BODY    │
                                              └────┬─────┘
                                                   │ find "\r\n--boundary"
                                                   │
                                                   ▼
                                          ┌─────────────────┐
                                          │ AFTER_BOUNDARY  │ (loop)
                                          └─────────────────┘
```

**Overlap buffer algorithm:** Boundaries can span across `on_data` calls. The reader keeps the last `delimiter_len - 1` bytes from each `on_data` call in an overlap buffer. On the next call, the overlap + new data are scanned together for the boundary. This handles the worst case: a boundary split at any byte position across chunks.

**Part metadata extraction:** When entering HEADERS state, part headers are accumulated in a 2048-byte header buffer. On `\r\n\r\n`, the reader parses `Content-Disposition: form-data; name="..."; filename="..."` and `Content-Type: ...` into the `KlHttpMultipartPart` struct.

**Configurable limits:**

| Limit | Field | Default | Effect when exceeded |
|-------|-------|---------|---------------------|
| Per-part size | `max_part_size` | unlimited | `on_data` returns -1 → 413 |
| Total body size | `max_total_size` | unlimited | `on_data` returns -1 → 413 |
| Number of parts | `max_parts` | unlimited | `on_data` returns -1 → 413 |

## Response Modes

```c
typedef enum {
    KL_HTTP_BODY_NONE,      /* no body (HEAD, 204, 304) */
    KL_HTTP_BODY_BUFFER,    /* body in memory — writev(headers + body) */
    KL_HTTP_BODY_FILE,      /* body is a file — sendfile(2) */
    KL_HTTP_BODY_STREAM     /* chunked transfer encoding */
} KlHttpBodyMode;
```

### Buffer mode (`kl_http_response_body_borrow` / `kl_http_response_body_copy`)

Headers and body are sent in a single `writev(2)` call — one syscall for the entire response. The response builder formats headers into a growable buffer, then `writev` sends `[header_iov, body_iov]` atomically.

### File mode (`kl_http_response_file`)

1. Headers are sent via `write(2)`
2. On Linux: `TCP_CORK` is set, then `sendfile(2)`, then `TCP_CORK` is cleared — coalescing headers and file data into minimum packets
3. On macOS: `sendfile(2)` with the macOS-specific API (reversed src/dst parameters, length via pointer)
4. Fallback: `read` + `write` loop for platforms without `sendfile`

The caller passes an open `fd` and file size. KEEL manages the fd lifetime — it closes the fd when the response is freed.

### Stream mode (`kl_http_response_begin_stream` / `kl_http_response_end_stream`)

1. `begin_stream` sends headers with `Transfer-Encoding: chunked`
2. Returns a `KlHttpResponseWriteFn` callback — each call formats data as an HTTP chunk (`<hex-len>\r\n<data>\r\n`) and writes it to the socket
3. `end_stream` sends the terminating `0\r\n\r\n` chunk

The write function signature `int (*)(void *ctx, const char *data, size_t len)` is designed to be passed directly to JSON serializers or any streaming encoder.

## Allocator Design

```c
typedef struct {
    void *(*malloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} KlAllocator;
```

**Why `size` is passed to `free` and `old_size` to `realloc`:**

Standard `free(ptr)` requires the allocator to store size metadata (typically 8-16 bytes before the pointer). By passing the size explicitly, KEEL enables:

- **Arena allocators** — bump pointer, free is a no-op, reset frees everything
- **Pool allocators** — fixed-size slabs, size determines which pool
- **Tracking allocators** — count bytes allocated/freed without hidden metadata
- **OS page allocators** — `munmap` needs the size

The default allocator wraps stdlib `malloc`/`realloc`/`free`, ignoring the size parameters.

## Router Design

The router uses a flat array of `KlHttpRoute` structs, scanned linearly on each request:

```c
int kl_http_router_match(KlHttpRouter *r,
                    const char *method, size_t method_len,
                    const char *path, size_t path_len,
                    KlHttpRoute **matched,
                    KlHttpParam *params, int *num_params);
```

Matching is segment-based (split on `/`). Each segment is compared with `memcmp` (exact) or captured as a parameter (`:name` prefix). Up to `KL_HTTP_ROUTER_MAX_PARAMS` (16) parameters are extracted.

**Return values:**
- `200` — match found, `*matched` set, params populated
- `405` — path matched at least one route but method didn't match any
- `404` — no route matches the path

**Why linear scan, not a trie?** For a typical API with 10-50 routes, a linear scan with early-exit is faster than a trie due to cache locality. The route table fits in a few cache lines. A trie adds pointer chasing and memory overhead that only pays off at hundreds of routes.

## Timeout Mechanism

```
 ┌───────────────────────────────────────────────────────────┐
 │  Event Loop Tick                                          │
 │                                                           │
 │  1. kl_event_wait(timeout=1000ms)                        │
 │  2. Process up to 64 ready events                        │
 │  3. Every ~400ms: sweep all active connections            │
 │     └─ if (now - conn.last_active_ms > read_timeout_ms)  │
 │        └─ send 408 → close                               │
 └───────────────────────────────────────────────────────────┘
```

- **Clock**: `kl_monotonic_ms()` — `clock_gettime(CLOCK_MONOTONIC)` on Linux, `mach_absolute_time()` on macOS
- **Stamping**: `last_active_ms` is updated on every successful `read(2)` or `write(2)` — so active transfers are never timed out
- **Sweep**: Runs after processing events, not on every connection operation — amortized O(n) over the pool

**Slow-loris protection**: An attacker sending one byte per second keeps the connection in READING state but never completes headers. The timeout sweep catches this — `last_active_ms` advances on each read, but if the total time from connection open exceeds the timeout without completing a request, the connection is closed.

## Memory Model

### Pre-allocated connection pool

```c
KlHttpConnPool {
    KlHttpConn *conns;      /* array of max_connections KlHttpConn structs */
    KlHttpConn *free_list;  /* singly-linked free list through next_free */
}
```

All connections are allocated at server init. `kl_http_conn_acquire` pops from the free list, `kl_http_conn_release` pushes back. No `malloc` during request handling. If the pool is exhausted, new connections are rejected at `accept(2)`.

Each `KlHttpConn` contains an 8KB `read_buf` inline — no separate allocation for the read buffer.

### Sliding window read buffer

For bodies larger than 8KB, the read buffer is reused as a sliding window:

1. Headers are parsed from `read_buf[0..hdr_len]`
2. Leftover data after headers is fed to the body reader
3. Subsequent `read(2)` calls fill the entire `read_buf` from offset 0
4. Each chunk is fed to the body reader via `on_data`

This means header pointers are invalidated once the body exceeds the initial read — which is why the body reader factory receives the request while headers are still valid.

### Response header buffer

The response builds headers into a growable buffer (initial capacity 512 bytes). For typical responses with 3-5 headers, this never needs to grow. The buffer is reused across keep-alive requests via `kl_http_response_reset`.

## Zero-Copy Techniques

1. **Header pointers into read buffer** — `KlHttpRequest` fields are direct pointers into `read_buf`, not copied strings
2. **sendfile(2)** — file responses bypass userspace entirely, kernel copies file→socket
3. **writev(2) batching** — headers + body sent as a single scatter-gather I/O, one syscall
4. **TCP_CORK** — on Linux, coalesces headers + sendfile data into minimum TCP segments
5. **Streaming writes** — chunked transfer encoding writes directly to the socket fd, no intermediate buffer
6. **Route parameter capture** — `KlHttpParam` values point into `read_buf`, not allocated strings

## TLS Transport Layer

KEEL provides a pluggable TLS vtable (`KlTls`) so users can bring any TLS backend (BearSSL, LibreSSL, OpenSSL, rustls-ffi) without vendoring dependencies.

### Vtable interface

```c
struct KlTls {
    KlTlsResult (*handshake)(KlTls *self, KlSocketHandle fd);
    kl_ssize_t  (*read)(KlTls *self, KlSocketHandle fd, void *buf, size_t len);
    kl_ssize_t  (*write)(KlTls *self, KlSocketHandle fd, const void *buf, size_t len);
    KlTlsResult (*shutdown)(KlTls *self, KlSocketHandle fd);
    size_t      (*pending)(KlTls *self);
    void        (*reset)(KlTls *self);
    void        (*destroy)(KlTls *self);
    const char *(*alpn_protocol)(KlTls *self);                       /* NULL = not supported */
    int         (*set_hostname)(KlTls *self, const char *hostname);  /* SNI, NULL = not supported */
    int         (*peer_cert)(KlTls *self, KlPeerCert *out);          /* mTLS client cert, NULL = not supported */
    /* optional completion-mode transport hooks (feed_input / drain_output / at_eof) —
       NULL on readiness-only backends; see include/keel/tls.h */
};
```

The `set_hostname` function enables SNI (Server Name Indication) for outbound TLS connections — used by the HTTP client to set the hostname before the handshake. Backends that don't support SNI set this to NULL.

### Handshake state

New connections with TLS enter `KL_HTTP_CONN_TLS_HANDSHAKE` instead of `KL_HTTP_CONN_READING`. The handshake is non-blocking — it returns `WANT_READ` or `WANT_WRITE` to indicate which event to wait for. The event loop re-arms accordingly. On `KL_TLS_OK`, the connection transitions to `KL_HTTP_CONN_READING` and proceeds normally.

### Pending drain

Edge-triggered event loops fire once per readiness transition. TLS may decrypt multiple application records in a single `SSL_read`. The `pending()` function reports buffered plaintext. After each `tls->read()`, the connection layer checks `pending()` and loops to drain all available data — preventing stalls when the TLS library has decrypted data but no new TCP segment triggers a readiness event.

### Sendfile fallback

`sendfile(2)` transfers data directly in the kernel, bypassing userspace. This is incompatible with TLS encryption. When TLS is active, file responses fall back to `pread()` into a stack buffer followed by `tls->write()`.

### Connection lifecycle

- **Init**: TLS sessions are pre-allocated (one per connection slot) via the factory
- **Accept**: If TLS configured, state starts at `KL_HTTP_CONN_TLS_HANDSHAKE`
- **Keep-alive**: TLS session persists across HTTP requests (no re-handshake)
- **Release**: `tls->shutdown()` (close_notify) → `tls->reset()` → `close(fd)`
- **Pool free**: `tls->destroy()` frees all session resources
