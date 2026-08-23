# Phase 8b — completion-axis breadth (bodies, streaming, files, UDP, TLS) — Design

**Status:** designed, implementation gated. Builds on Phase 8a (the completion
connection driver, currently serving plaintext GET/HEAD HTTP over IOCP). Additive,
high risk, completion-backend validated by the Windows-IOCP CI job.

**Two governing constraints (from the user) — both are orthogonality axes:**

1. **Non-invasive to surface public APIs.** Extending the completion path to request
   bodies, TLS, UDP, and streaming/file responses must **not percolate the
   completion model into the public APIs** of those surfaces. `KlBodyReader`,
   `KlTls`, `KlUdp`, and `KlResponse` (file/stream) stay model-agnostic — a body
   reader / TLS backend / UDP handler / streaming response never learns whether its
   bytes came from a readiness `recv` or a completed `WSARecv`.

2. **Completion is a platform-independent *concept*, not an IOCP detail.** Completion
   is an event **axis**, a peer to readiness — exactly as `event.h` is the abstract
   readiness axis with epoll / kqueue / poll / WSAPoll as interchangeable
   *implementations*, completion has an abstract axis with **IOCP as one
   implementation** (io_uring-completion and POSIX AIO are future siblings). The
   generic completion **driver logic** (the connection state machine over
   completions) must therefore be **platform-independent** — `WSARecv`/`WSASend`/
   `AcceptEx`/`TransmitFile`/`GetQueuedCompletionStatusEx`/`OVERLAPPED` must **not
   percolate** beyond the IOCP implementation TU and induce platform dependency
   where the concept, not the mechanism, belongs.

---

## 1. The two axes, drawn out

```
                     readiness axis                     completion axis
 abstract concept    event.h (kl_event_add/wait)        completion.h (post op / drain completions)   ← platform-independent
 implementations     event_epoll/kqueue/poll/wsapoll    event_iocp (IOCP)  · io_uring-comp*  · AIO*  ← platform-specific
 generic driver      server readiness loop (inline)     completion_driver.c (state machine)          ← platform-independent
 model-blind core    conn_internal.h / response_internal.h  (shared by BOTH axes)                    ← platform + model independent
```

8a proved the **model-blind core** reuse (both axes call `kl_conn_dispatch_request`
etc.). 8b adds the missing separation on the completion side: split 8a's
`event_iocp.c` — which today mixes the generic driver logic with the IOCP
mechanics — into a **platform-independent completion driver** + an **IOCP
implementation of the completion axis**. Then every 8b surface is added to the
*generic* driver, with each platform supplying only its overlapped-op primitive.

---

## 2. The abstract completion axis (internal, platform-independent)

A new internal `src/completion.h` — the completion counterpart of `event.h`. It
names **what** a completion transport does, never **how** (no Win32 types):

```c
/* Platform-independent completion op kinds. */
typedef enum { KL_COMP_ACCEPT, KL_COMP_READ, KL_COMP_WRITE, KL_COMP_SENDFILE } KlCompKind;

/* One finished async op, handed from a completion backend to the generic driver. */
typedef struct {
    void       *conn;      /* KlConn* (READ/WRITE/SENDFILE) */
    KlCompKind  kind;
    size_t      bytes;     /* transferred */
    int         ok;        /* 0 = failed/peer-closed */
    KlSocketHandle accepted_fd;              /* ACCEPT: the new socket */
    struct sockaddr_storage peer;            /* ACCEPT: filled by the backend */
    socklen_t   peer_len;
} KlCompletionEvent;

/* A completion transport (one impl per completion-capable platform). Opaque —
 * the backend owns its OVERLAPPED pools / port / SQEs. Post primitives are the
 * only way the generic driver expresses async intent; drain yields finished ops. */
int kl_comp_post_accept(KlEventLoop *loop, struct KlServer *s);
int kl_comp_post_recv (KlEventLoop *loop, KlConn *c);           /* into c->read_buf */
int kl_comp_post_send (KlEventLoop *loop, KlConn *c, const void *buf, size_t len);
int kl_comp_post_sendfile(KlEventLoop *loop, KlConn *c, int file_fd,
                          uint64_t offset, size_t count);
int kl_comp_drain(KlEventLoop *loop, KlCompletionEvent *out, int max, int timeout_ms);
```

The **generic driver** `src/completion_driver.c` (platform-independent — contains
**no** Win32/IOCP symbol) is the completion tick, driving connections via the
model-blind core and the abstract post primitives:

```c
int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {   /* the io_engine seam */
    KlCompletionEvent ev[N];
    int n = kl_comp_drain(&s->ev.loop, ev, N, timeout_ms);   /* backend-specific inside */
    for (i < n) switch (ev[i].kind) {
        case KL_COMP_ACCEPT: comp_on_accept(s, &ev[i]);   /* acquire conn, post_recv, post_accept */
        case KL_COMP_READ:   comp_on_read (s, &ev[i]);    /* kl_conn_dispatch_request → post_send / post_recv */
        case KL_COMP_WRITE:  comp_on_write(s, &ev[i]);    /* kl_conn_send_complete → keep-alive / close */
        ...
    }
}
```

**IOCP becomes just an implementation** of `completion.h` in `event_iocp.c`: the
port, the `OVERLAPPED` op pool, `AcceptEx`/`WSARecv`/`WSASend`/`TransmitFile`, and
`GetQueuedCompletionStatusEx` → `KlCompletionEvent`. A future `io_uring`-completion
or POSIX-AIO backend implements the same `completion.h` and **reuses
`completion_driver.c` unchanged**.

---

## 3. Staging (dependency order; each a CI-green increment)

| Increment | What | Platform-independent part | IOCP-implementation part |
|---|---|---|---|
| **8b-0** | **Extract the completion axis** | `completion.h` + `completion_driver.c` (generic driver lifted from 8a's `event_iocp.c`) | `event_iocp.c` reduced to the `completion.h` impl (post primitives + GQCS drain) |
| **8b-1** | request bodies | `kl_conn_ingest_body` (model-blind) + driver READING_BODY | `WSARecv` (unchanged primitive) |
| **8b-2** | file responses | driver `KL_BODY_FILE` → `kl_comp_post_sendfile` | `TransmitFile` |
| **8b-3** | streaming | `KlDrain` sink hook + driver chunk pump | chunk `WSASend` |
| **8b-4** | UDP | generic datagram completion dispatch (reuse `KlUdp` callback) | `WSARecvFrom`/`WSASendTo` (+ a Windows-only `udp_io` sibling) |
| **8b-5** | TLS | internal buffered BIO (byte source only; `KlTls` vtable unchanged) | `WSARecv`/`WSASend` of ciphertext |

**8b-0 is first and is a pure refactor** — POSIX byte-identical (it doesn't touch
the readiness path), the IOCP smoke stays green, and it establishes the
platform-independent axis every later increment builds on. 8b-1..4 are tractable;
8b-5 (TLS) is the deepest inversion and is sequenced last (may become its own phase).

---

## 4. Per-surface non-invasiveness (public APIs untouched)

- **Request bodies (8b-1):** expose `kl_conn_ingest_body(KlConn*, size_t)` in
  `conn_internal.h` (the `READING_BODY` core lifted from `kl_conn_on_readable`,
  which keeps calling it inline → byte-identical). `KlBodyReader.on_data/on_complete`
  unchanged.
- **File responses (8b-2):** the generic driver requests `kl_comp_post_sendfile`;
  IOCP does `TransmitFile`, POSIX-AIO would do something else. `kl_response_file()`
  and `KL_BODY_FILE` unchanged; the IOCP provider may advertise the internal
  `KL_SOCK_CAP_SENDFILE`.
- **Streaming (8b-3):** the chunk pump is generic (produce chunk → `kl_comp_post_send`
  → next on completion); `KlDrain`'s public `KlDrainWriteFn`/`on_drain`/`max_size`
  and `kl_sse_*`/`kl_response_stream_*` unchanged.
- **UDP (8b-4):** the datagram dispatch core delivers to the existing `KlUdpRecvFn`;
  `kl_udp_recv_start`/`kl_udp_send_to` and config knobs unchanged.
- **TLS (8b-5):** the mbedTLS BIO's byte *source* becomes a per-conn completion
  buffer (internal mode in `tls_mbedtls.c`); the public `KlTls`
  handshake/read/write/pending vtable is unchanged.

---

## 5. Orthogonality litmus (grep-assertable, both axes, per increment)

- **Axis 1 (public APIs):** no `include/keel/*.h` change in any 8b changeset.
- **Axis 2 (concept vs implementation):** **no Win32/IOCP symbol** (`OVERLAPPED`,
  `WSA*`, `AcceptEx`, `TransmitFile`, `GetQueuedCompletionStatus*`, `iocp`) appears
  in `completion_driver.c`, `completion.h`, or any shared/POSIX TU — they live
  **only** in `event_iocp.c` (and a Windows-only `udp_io` sibling). `completion.h`
  pulls no `<windows.h>`.
- Every exposed model-blind core is called identically by the readiness path →
  POSIX stays byte-identical (`make test` green throughout).

---

## 6. Risks & stop conditions

- **8b-0 driver extraction** is the structural crux: if the generic driver cannot be
  lifted without an IOCP concept leaking into `completion_driver.c` (litmus axis 2),
  **stop** — the abstraction boundary is wrong.
- **TLS buffered-BIO (8b-5)**: if it needs a public `KlTls` change, **stop** (a
  completion-aware TLS transport is a separate, bigger design).
- **Streaming back-pressure** over completion must preserve the public
  `on_drain`/`max_size` semantics (one outstanding send, next chunk on completion).
- **`TransmitFile`/AIO file-handle lifetime** across the overlapped op.
- **Runtime-only validation** via the Windows-IOCP smoke tests, iterated against CI;
  each increment stays small so a blind cycle is cheap.

## 7. Out of scope (beyond 8b)

- A second completion *implementation* (io_uring-completion / POSIX AIO) — the axis
  is designed to accept one, but building it is future work.
- IOCP as the Windows default (WSAPoll stays default; IOCP opt-in via `BACKEND=iocp`).
- Any *public* completion API (frozen until a second consumer needs it — Phase 7
  internal-first rule holds).
- HTTP/2 and WebSocket completion drivers (they ride the readiness path today).
