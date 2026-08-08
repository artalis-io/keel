# Generic Transport Audit (Phase 1)

**Date:** 2026-08-08
**Goal:** consolidate Keel's transport layer into a small, protocol-neutral stream/listener
API that is identical across readiness and completion I/O and across transport providers —
by **evolving existing primitives**, not adding a parallel subsystem.

**Scope reminder:** this is an *audit*, not a redesign. No production protocol (MQTT/QUIC/…)
is in scope. The deliverable of Phase 1 is this document; code changes come later, incrementally.

---

## 1. Executive summary

Keel already contains every primitive a generic `KlStream`/`KlListener` needs — but they are
**not packaged as a standalone stream object**. Instead the transport concept is:

1. **Typed on `KlConn`** (the HTTP-*server* connection struct) or reached through
   `KlConn`-scoped helpers, so a non-HTTP protocol can't hold "a stream" without dragging in
   HTTP state; and
2. **Implemented three times** with the same observable semantics but different plumbing:
   the **server readiness** path (`connection.c` + `conn_read`/`conn_write`), the **server
   completion** path (`completion_server.c` + `kl_comp_post_recv`/`post_send`), and the
   **client** path (`client_async.c` watcher-relay + `client_sync.c` blocking).

There is **no `KlStream`, `KlListener`, or `KlEndpoint` type today** (verified: grep finds
none). The good news: the axis separation below them is already clean (`KlSocketProvider` +
`KlSockAddr` + `KlSocketHandle` + the completion/readiness negotiation), so consolidation is a
matter of **carving a transport object out of `KlConn` and retyping the completion I/O ops onto
it** — not building new machinery.

The single highest-leverage change: `KlCompletionOps.post_recv(KlConn*)` / `post_send(KlConn*)`
are typed on `KlConn`. Retyping them onto a generic stream handle is the seam that unlocks
non-HTTP completion-mode protocols.

---

## 2. What already exists (the reusable substrate)

### 2.1 Socket axis — `KlSocketProvider` (`src/socket.h`) — KEEP AS-IS

The low-level, already-protocol-neutral, already-model-neutral vtable. This is the layer
*below* the proposed `KlStream`; it needs **no change**.

- Handle: `KlSocketHandle` (`include/keel/handle.h`) — pointer-width `intptr_t`,
  `KL_INVALID_SOCKET` sentinel, `kl_handle_valid()`. Already NOT an `int fd`.
- Address: `KlSockAddr` (`include/keel/sockaddr.h`) — neutral; providers marshal to/from native
  `sockaddr` at their own seam. Already NOT `struct sockaddr` in the portable API.
- Ops (via `kl_sock_*` inline seam): `socket`, `bind`, `listen`, `accept`, `connect`, `recv`,
  `recv_peek`, `send`, `writev`, `sendfile`, `close`, `set_nonblocking`, `set_cloexec`,
  `set_reuseaddr`/`reuseport`/`ipv6only`/`tcp_nodelay`/`set_cork`, `get_so_error`,
  `get_local_addr`, and `io_status()` → `KlIoStatus` (WOULD_BLOCK/INTERRUPTED/RESET/CLOSED/FATAL).
- Providers: `socket_posix.c`, `socket_winsock.c`; overlapped providers in the event TUs
  (`iouring`/`iocp`/`pollcomp`); `integrations/lwip` (BSD + raw NO_SYS); `integrations/uefi`
  (EFI_TCP4).

**Assessment:** this IS the generic transport substrate. `KlStream` should sit *directly on top
of it*, not reinvent it.

### 2.2 Event axis — readiness + completion — KEEP; NEGOTIATION IS SOUND

- Readiness: `KlEventLoop` (`event.h`: `init/add/mod/del/wait/close/caps/native_provider`).
- Completion: `KlCompletionOps` (`src/completion.h`) — `drain`, `prime_accepts`, `post_accept`,
  `post_recv`, `post_send`, `post_sendfile`, `post_connect`, `cancel`, `post_udp_recv`,
  `post_udp_send`.
- Negotiation: `kl_event_ctx_sockets_compatible()` (a completion loop needs an overlapped
  provider; a readiness loop needs a native-fd provider). Capability flags in `event_caps.h`.

**Assessment:** the axes are correctly orthogonal and negotiated. The problem is not the axes —
it's that the completion ops are **typed on `KlConn`/`KlUdp`/`KlServer`**, not a generic stream.

### 2.3 The de-facto stream — embedded in `KlConn` (`connection.h`)

`KlConn` mixes a **transport subset** with **HTTP state**:

| Transport subset (→ belongs in `KlStream`) | HTTP/server state (→ stays in `KlHttpConn`) |
|---|---|
| `KlSocketHandle fd` | `KlRequest req`, `KlResponse res` |
| `struct KlEventCtx *ctx` (→ provider + loop) | `KlParser *parser`, `hdr_sent`, `route_result` |
| `KlSockAddr peer_addr`, `peer_source` | `KlWsServerConn *ws`, `KlH2ServerConn *h2`, `h2_config` |
| `char *read_buf; size_t read_len, read_cap` | `KlRouter *router`, `file_io`, `access_log` |
| `int read_paused` (read backpressure) | `max_body_size`, `request_start_ms`, … |
| `KlTls *tls; int tls_want` | `struct KlAsyncOp *async_op` (handler suspend) |
| `last_active_ms` (idle/deadline) | |

The readiness byte-I/O seam is already generic in shape but `KlConn`-typed
(`src/internal.h`):

```c
static inline ssize_t conn_read (KlConn *c, void *buf, size_t len);   /* tls?->read  : sock_recv */
static inline ssize_t conn_write(KlConn *c, const void *buf, size_t len); /* tls?->write : sock_send */
static inline int     conn_write_all(KlConn *c, ...);
```

**Assessment:** `conn_read`/`conn_write` are *literally* `kl_stream_read`/`kl_stream_write` in
disguise — they already abstract TLS-vs-socket. They just need to take a `KlStream*` (the
transport subset above) instead of a `KlConn*`.

### 2.4 Backpressure — already factored, already generic

- **Write:** `KlDrain` (`include/keel/drain.h`) — a bounded write buffer with `write_fn`,
  `max_size` cap, `on_drain` callback, `kl_drain_write`/`flush`/`pending`. Already
  protocol-neutral (takes a `KlDrainWriteFn`, not a `KlConn`). Used by the server streaming path
  and the completion server (`comp_stream_pump`).
- **Read:** `KlConn.read_paused` + `kl_request_pause_body`/`resume_body` (readiness: drop READ
  interest; completion: don't re-post recv). The *mechanism* is generic; the *entry points* are
  HTTP-request-scoped (`kl_request_*`).

**Assessment:** `KlDrain` is directly reusable as the stream write-backpressure. Read-pause needs
a generic `kl_stream_pause`/`resume` (the mechanism already exists; only the naming is HTTP-ish).

### 2.5 Operation lifecycle / deadlines / cancellation — reuse `KlAsyncOp` + `KlTimer`

- `KlAsyncOp` (`async.h`): suspend/complete/cancel with `on_resume`/`on_deadline`/`on_cancel` —
  a terminal-once primitive. **This is the `KlOp` the spec describes; do not build a new one.**
- `KlTimer` (`timer.h`): one-shot deadlines on `KlEventCtx` (min-heap).
- Completion cancellation: `kl_comp_cancel(ctx, fd)` + the generation-guard / quarantine
  discipline (EFI provider) enforce exactly-one terminal completion.

**Assessment:** the async-op + timer + cancel primitives already satisfy the "exactly one
terminal outcome" invariant. The stream layer should *use* them, not replace them.

### 2.6 Datagrams — already stream-separate (KEEP SEPARATE)

`KlUdp` (`udp.h`) + `KlUdpServer` are a distinct datagram surface (`bind`, `recv_start`,
`send_to`, capped send queue). A `keel/datagram.h` provider-ops tag already exists (referenced
from `KlUdpConfig`). The spec explicitly says **do not merge streams and datagrams** — Keel
already honors this. `KlDatagram`/`KlEndpoint` for UDP would be a thin rename/formalization of
`KlUdp`, not a merge.

### 2.7 TLS — already a transport wrapper (`KlTls`, `tls.h`)

`KlTls` (handshake/read/write/shutdown/pending/reset/destroy + optional
`feed_input`/`drain_output`/`at_eof` for completion memory-BIO) already wraps *the socket seam*,
not HTTP: socket-BIO mode uses `t->sp = ctx->sockets`; completion mode feeds ciphertext via
`feed_input`. `conn_read`/`conn_write` already dispatch `tls ? tls->op : sock_op`.

**Assessment:** TLS is already "wrap a transport," but the transport it wraps is expressed as
`(KlSocketHandle fd, KlSocketProvider *sp)` + the `KlConn`-scoped memory-BIO. Pointing it at a
`KlStream` is a retype, not a redesign. (The recent completion-TLS review confirmed the memory-BIO
recv contract — every completion backend feeds ciphertext — which a `KlStream` can own.)

---

## 3. Duplication (the same semantics implemented N times)

| Concern | Server readiness | Server completion | Client |
|---|---|---|---|
| **byte read** | `conn_read` (`internal.h`) on READ event (`server.c` dispatch) | `kl_comp_post_recv(KlConn*)` → `comp_on_read` (`completion_server.c`) | `client_async.c` watcher-relay `kl_sock_recv`; `client_sync.c` blocking `kl_sock_recv`+`kl_plat_poll1` |
| **byte write** | `conn_write`/`conn_write_all` + `KlDrain` | `kl_comp_post_send(KlConn*)` → `comp_on_write` | watcher-relay `kl_sock_send`; blocking `kl_sock_send` |
| **accept** | `server.c` readiness loop → `kl_sock_accept` | `prime_accepts`/`post_accept` + `KL_COMP_ACCEPT` → `comp_on_accept` | n/a |
| **connect** | n/a (server) | `kl_comp_post_connect` | `client_async.c` Happy-Eyeballs (`post_connect` completion **or** watcher-relayed non-blocking connect); `client_sync.c` blocking connect |
| **read pause** | drop READ interest (`kl_request_pause_body`) | don't re-post recv | (client rarely pauses) |
| **write backpressure** | `KlDrain` + writable re-arm | `comp_stream_pump` (one overlapped send in flight) + `KlDrain` | client send loop / would-block |

**Three observations:**
- The *client* has an entirely separate transport loop from the *server*, even though both do
  "connect a TCP stream, read/write bytes, honor backpressure." A `KlStream` used by both would
  delete a whole parallel implementation's worth of semantics.
- The server readiness and completion paths already share the **model-blind core**
  (`kl_conn_dispatch_request`/`ingest_body`/`send_complete` in `conn_internal.h`) — proof the
  consolidation pattern works; it just hasn't been applied *below* the parser (to the byte
  transport) or to the *client*.
- Accept has three spellings but one meaning ("hand me the next connected stream").

---

## 4. Coupling: protocol code depending on HTTP/server structures

- **`KlCompletionOps.post_recv(KlConn*)` / `post_send(KlConn*)` / `post_sendfile(KlConn*)`**
  (`completion.h:98-102`) — the completion transport primitives are **typed on the HTTP-server
  connection**. A non-HTTP completion protocol cannot post a read/write without a `KlConn`. **This
  is the #1 coupling to break.**
- **`conn_read`/`conn_write`** (`internal.h`) — the readiness byte seam is `KlConn`-typed.
- **`prime_accepts(KlServer*)` / `post_accept(KlServer*)`** — accept is typed on `KlServer` (it
  reaches `s->pool`, `s->listen_fd`, `s->ev`). A generic `KlListener` would carry its own
  accept-target pool + endpoint instead of reaching into `KlServer`.
- **Read backpressure entry points are `kl_request_*`** (`request.h`) — HTTP-request-scoped names
  for a generic stream mechanism.
- **`KlTls` completion memory-BIO** is driven by `completion_server.c` (`comp_tls_drive`) which is
  server-shaped; the *mechanism* is generic but the *driver* lives in the HTTP server.

Everything above the socket seam that a protocol touches is reachable only via `KlConn`/`KlServer`
/`KlRequest`. That is exactly the dependency the task wants removed.

---

## 5. Native-descriptor / model / transport assumptions

### Native-descriptor assumptions — MOSTLY ALREADY GONE
- The portable layers use `KlSocketHandle` (pointer-width) + `KlSockAddr`, not `int fd` /
  `struct sockaddr`. The freestanding UEFI + lwIP-raw providers prove no native fd is assumed.
- **Residual:** `KlSocketHandle` is still *spelled* `fd` throughout `KlConn`/client code and
  compared with `kl_handle_valid`. That's cosmetic (it's already opaque), but a generic API should
  never name it `fd`.
- **`sendfile` / `KlFileIO`** assume a file descriptor source (`int in_fd`) — an HTTP
  zero-copy optimization. A generic `KlStream` must NOT require this; it belongs to an optional
  file-response extension, not the core stream contract.

### Readiness-only assumptions
- `server.c` dispatch is written in readiness terms (READ/WRITE-ready → act), but it already
  delegates the completion branch to `kl_server_run_completion_loop`. The client's watcher-relay
  is readiness-shaped even on completion loops (it relays `KL_COMP_WATCHER`).

### Completion-only assumptions
- `post_recv`/`post_send`/`post_accept` + `KL_COMP_*` events are completion-native and typed on
  `KlConn`/`KlServer`. The readiness path has no equivalent "posted op" object — it acts inline on
  readiness. A `KlStream` must expose **one** read/write contract that both satisfy (the readiness
  impl acts on readiness; the completion impl posts an op), which is exactly the model-blindness
  goal.

### Transport-specific assumptions
- `set_tcp_nodelay`/`set_cork`/`sendfile` are TCP-isms invoked from the HTTP paths; a generic
  stream should treat them as **optional provider capabilities**, not mandatory contract.
- lwIP-raw and EFI already run the *same* completion driver, proving the transport-specific bits
  are correctly confined to providers.

---

## 6. Public vs internal surface (today)

- **Public (`include/keel/`):** `socket.h`? No — the seam `src/socket.h` is INTERNAL; the public
  knobs are `KlConfig.sockets` / `KlClientConfig.sockets` (a `const KlSocketProvider*`). `handle.h`,
  `sockaddr.h`, `event.h`, `event_ctx.h`, `tls.h`, `drain.h`, `timer.h`, `udp.h`, `async.h`,
  `connection.h` are public. `connection.h` exposes `KlConn` (HTTP-server-shaped).
- **Internal (`src/`):** `socket.h` seam, `internal.h` (`conn_read`/`write`), `completion.h`,
  `conn_internal.h`, `io_engine.h`, `event_caps.h`, `proto_hooks.h`.

**Implication:** a `KlStream`/`KlListener` public header is *new public surface*, but most of what
it needs is already public (handle/sockaddr/event_ctx/tls/drain/timer). The socket seam should
stay internal; the stream API is the protocol-author-facing layer above it.

---

## 7. Compatibility risks (for the later migration)

1. **`KlCompletionOps` retype (`KlConn*` → stream handle)** touches every completion backend
   (`event_iouring/iocp/pollcomp`, lwIP-raw, EFI). Must keep the completion vtable shape and the
   terminal-once + buffer-ownership invariants. Highest-risk change; do it behind the existing
   dispatch (`completion_dispatch.c`) with the stream handle embedding/aliasing `KlConn`'s
   transport subset so no backend logic changes semantics.
2. **Freestanding archives** (`libkeel_freestanding{,_server}`) — a `KlStream` TU must stay
   freestanding-clean (no OS/errno/stdio) so the UEFI client/server keep linking. The transport
   subset (handle/ctx/sockaddr/tls/drain) is already freestanding.
3. **The client's Happy-Eyeballs connect** is intricate (races addresses, per-attempt delay,
   deadline). Migrating the client onto `KlStream` must preserve it — treat connect as a
   `KlStream` factory that internally keeps the HE logic, not a naive single-connect.
4. **TLS memory-BIO ownership** (completion mode) — the recent review found a copy-required
   contract on the send path; a `KlStream` that owns the send buffer must preserve it.
5. **No behavior/ABI regressions** — 67 test suites + firmware/lwIP/Windows CI must stay green;
   ABI is source-compat only (static relink), so we can retype internal structs freely but must
   not break public `KlConfig`/`KlClientConfig`/`KlConn` field expectations that tests rely on.

---

## 8. Recommended consolidation (conservative, evolve-don't-parallel)

**Do not add a parallel subsystem.** Carve the transport subset out of `KlConn` and reuse
everything below it:

1. **`KlStream`** = the transport subset of `KlConn` (handle + `KlEventCtx*` (→ provider+loop) +
   `KlSockAddr` peer/local + read buffer + `read_paused` + `KlTls*` + a `KlDrain` for write
   backpressure). Its ops are today's `conn_read`/`conn_write` (readiness) unified with
   `post_recv`/`post_send` (completion) behind one contract:
   `kl_stream_read`/`write`/`writev`/`pause`/`resume`/`shutdown_write`/`close`/`abort`/`cancel` +
   `local/peer_endpoint` + `set_deadline`. Model-blindness reuses the existing
   `completion_dispatch.c` seam (retyped to the stream handle) and the readiness dispatch.
2. **`KlListener`** = bind/listen/accept/cancel/close over the socket provider's
   `listen`/`accept` + the completion `prime_accepts`/`post_accept`/`KL_COMP_ACCEPT`, carrying its
   own accepted-stream target instead of reaching into `KlServer`.
3. **`KlEndpoint`** = a formalization of `KlSockAddr` + a transport-kind tag (TCP/Unix/lwIP/…) so
   `connect`/`bind` dispatch to the right provider. `KlSockAddr` already carries family; the kind
   tag is the only addition. **Do not force non-socket endpoints into `sockaddr`.**
4. **`KlOp`** = reuse `KlAsyncOp` + `KlTimer` + `kl_comp_cancel`; formalize the five terminal
   states as documentation over the existing primitive. No new op system.
5. **`KlDatagram`/UDP** = leave `KlUdp` as-is; optionally alias `KlEndpoint` for its addresses.
   Keep separate from streams.
6. **`KlConn` becomes `{ KlStream *stream; <HTTP state> }`** — HTTP keeps its parser/req/res/ws/h2
   but its bytes flow through `KlStream`. The client uses `KlStream` directly. This *deletes* the
   client's parallel transport loop and unifies server readiness/completion under the stream.

**Migration order (test-driven, per the task):** (a) Phase-8 length-prefixed echo test written
against the *new* `KlStream`/`KlListener` to lock the contract; (b) client transport → `KlStream`;
(c) server transport → `KlStream` (KlConn wraps it); (d) WebSocket/HTTP-2 reuse where practical.
Each step keeps all suites green + sanitizer-clean.

**What NOT to do (confirmed against the spec):** no futures/promises/coroutines; no stream/datagram
merge; no native fd in the generic API; no new mandatory dependency; keep `sendfile`/`nodelay`/etc.
as optional capabilities, not core contract.

---

## 9. Phase-1 conclusion

The consolidation is **extraction + retyping**, not construction:
- The substrate (socket provider, sockaddr, handle, event/completion negotiation, drain, timer,
  async-op, TLS wrapper) is already generic and model-negotiated.
- The transport *object* is missing: it lives dissolved inside `KlConn` and re-implemented in the
  client. Carving `KlStream`/`KlListener` out of `KlConn` and retyping `KlCompletionOps` onto the
  stream handle is the whole job.
- Next deliverable: **`docs/stream_contract.md`** (Phase 3) — the normative read/write/backpressure/
  pause/close/cancel/deadline semantics that both the readiness and completion implementations, and
  every provider, must satisfy — written *before* touching HTTP code.
