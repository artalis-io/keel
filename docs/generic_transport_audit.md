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

**Assessment (correction):** `KlDrain` is the right *foundation* but is **not** drop-in for the
four-way write contract an earlier draft assumed. Today `kl_drain_write` returns only `0`/`-1`
(no partial-accept, no high-water result), treats exceeding `max_size` as a **sticky fatal**
error (`d->error`), and fires `on_drain` only on **non-empty → empty** (not a configurable
low-water mark). So a `KlStream` write API needs a **new concrete result type** (status +
accepted-count + `KlIoStatus`) and either atomic-accept semantics on top of `KlDrain` or a
`KlDrain` extension for partial-accept / low-water. Read-pause needs a generic
`kl_stream_pause`/`resume` (the `read_paused` mechanism exists; only the naming is HTTP-ish).

### 2.5 Operation lifecycle / deadlines / cancellation — PARTIAL substrate (correction)

> **Correction (review):** an earlier draft claimed `KlOp` "can be realized by `KlAsyncOp` +
> `KlTimer` + `kl_comp_cancel`." That is **not** true today and is retracted.

What actually exists:
- `KlAsyncOp` (`async.h`) is an **HTTP-handler-suspension** object, not a transport op: it holds
  `KlConn *conn`; its calls take `KlServer *` (`kl_async_suspend/complete/cancel`); its terminals
  are only **resume** / **cancel** (its `deadline` is *not* a terminal — it prompts the app to call
  complete-or-cancel). It has no completed/failed/timed-out/peer-closed states. **Do not reuse it
  for streams.**
- `kl_comp_cancel(ctx, fd)` cancels **by handle**, not per-operation. There is no way today to
  cancel *one write* while keeping the stream usable.
- `KlTimer` (`timer.h`) — one-shot deadlines on `KlEventCtx` (real, reusable).
- The completion axis *does* enforce exactly-one-terminal + drop-late-events per posted op
  (generation guards + op retirement), but only internally, keyed by handle/slot — not exposed as a
  per-op cancellable record.

**Assessment (NEW MACHINERY, not extraction):** per-operation cancellation and the five terminal
states are **not** available from existing primitives. This requires either (a) a small
transport-operation lifecycle (per-stream: one read + one write op record, plus a separately
cancellable `KlConnectOp` handle with a generation + terminal flag; accept is push-only, §8.2) — NOT a futures
framework — or (b) a deliberately coarser **stream-level** cancellation contract for the first
cut. `KlTimer` supplies deadlines; the op-identity/terminal layer must be built.

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

**Assessment (corrected):** the **`KlTls` vtable is reusable substrate**, but lifting its HTTP
completion driver into a generic TLS-wrapped stream is **new orchestration machinery, not a
retype** — the wrapper is a state machine over cross-direction wants (WANT_READ from a write /
WANT_WRITE from a read), handshake-before-delivery, `pending()` plaintext drain without another
event, memory-BIO ciphertext ownership, and close-notify vs FIN (see the stream contract §6bis).
So the vtable is reused as-is; its generic-stream integration is a **Phase C** build, not Phase A.
(The recent completion-TLS review confirmed the memory-BIO recv contract — every completion backend
feeds ciphertext — which the wrapped stream will own.)

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

## 8. Recommended consolidation — IMPLEMENTATION PHASES (review reset v2)

> **Correction (review):** an earlier draft used one "Tier 1/2/3" axis for *both* "how simple the
> capability is" and "what to build first," which wrongly made **Tier 1 look like pure extraction
> that's safe to implement now** — when it actually depends on new machinery (reservation, listener
> carve, deferred close). Those are **two different axes.** This section is the **implementation
> order** (Phase A → B → C → Optional); the *capability tiers* in `docs/stream_contract.md` describe
> what the contract promises. **No phase is "pure extraction" (Finding 7):** even Phase A is
> *semantics-preserving structural extraction*, not a no-logic retype — it moves the receive-buffer
> boundary above the provider and retypes the listener accept target (both change backend logic while
> preserving observable behavior). The baseline public surface is **Phase B** and requires the new
> foundations before it can ship.

### Phase A — structural extraction (semantics-preserving; internal; the receive-boundary move)
- **`KlStream`** = the transport subset of `KlConn`, **embedded** (below): handle + `KlEventCtx*` +
  `KlSockAddr` peer/local + read buffer + `read_paused` + a `KlDrain`. Plaintext only.
- **Move the receive-buffer boundary ABOVE the provider (the real Phase-A prerequisite — NOT a pure
  retype).** Today `post_recv(KlConn*)` is **not logic-neutral** in *every* completion backend, not
  just EFI (Finding 5). Each one inspects `c->tls` / `c->state` / `KL_CONN_PROXY_HEADER` and chooses
  between recv-into-`read_buf` (plaintext) vs recv-ciphertext-into-backend-scratch + `tls->feed_input`
  (TLS):
  - **io_uring** — `IOU_READ` → `op->buf = c->read_buf + c->read_len`; `IOU_TLS_RECV` → `op->sendbuf`
    scratch + `feed_input` (`src/event_iouring.c`).
  - **IOCP** — `KL_IOCP_READ` → `buf.buf = c->read_buf + c->read_len`; `KL_IOCP_TLS_RECV` → `op->sendbuf`
    scratch + `feed_input` (`src/event_iocp.c`).
  - **pollcomp** — `PC_READ` / `PC_TLS_RECV` → same split (`src/event_pollcomp.c`).
  - **EFI** — `el_post_recv`/drain (`event_efi.c`) → `read_buf` (plaintext) vs `g_tls_cipher` +
    `feed_input`.
  Those are TLS/HTTP-state decisions living **inside a backend**. Phase A must migrate **all** of them
  so the **provider only performs raw transport I/O into a caller-specified buffer**:
  ```c
  post_recv(KlStream *stream, void *buf, size_t cap);   /* provider: raw transport bytes only */
  ```
  - a **plaintext stream** passes its `read_buf`+space and delivers those bytes directly;
  - a **TLS wrapper** (Phase C) passes its own ciphertext scratch, feeds `KlTls`, delivers the
    resulting plaintext.
  **TLS-preservation path across the gap:** the generic TLS *wrapper stream* is Phase C, but production
  HTTPS must not regress in between. So once `post_recv` is raw, the **existing HTTP TLS completion
  driver** (the `comp_tls_*` glue that already owns ciphertext scratch + `feed_input`) **temporarily
  becomes the caller** that hands the provider its ciphertext buffer and runs the decrypt — i.e. the
  `c->tls` decision moves from *inside each backend* up into the one HTTP-side driver, unchanged in
  behavior, until Phase C generalizes it into the wrapper stream. This gives Phase A a concrete,
  no-regression path for TLS rather than leaving it stranded.
  This **removes the cross-backend `c->tls` peek** (a boundary the earlier review flagged) and is the
  step that makes `post_recv` genuinely stream-typed. Audit `post_send` the same way — confirm no
  backend reads HTTP response fields after the retype.
- **`post_sendfile(KlConn*)` stays HTTP-specific** (file transfer — Optional); not retyped in Phase A.
- Retype the **listener** accept target: `prime_accepts` / `post_accept` from `KlServer*` to a
  `KlListener*`, add `KL_COMP_ACCEPT.target = KlListener*`. This is **semantics-preserving backend
  retyping, NOT "no logic change"** (Finding 5): every completion backend must store the listener in
  each posted accept, carry it in `.target`, read **listener credit** instead of server-pool
  counters, route completions to the right listener, support multiple listeners per `KlEventCtx`, and
  detach listener-owned accept records on close. The server embeds/owns a `KlListener` and supplies
  its existing connection pool via the listener's acquire/release seam (no new pool/allocation).
- Generic read/write **delivery callbacks**; **stream-level** `pause`/`resume` (rename `read_paused`).
- **Coarse internal close only** — preserve today's behavior. Do NOT ship the public graceful
  `kl_stream_close` here (Phase B).
- **DECISION — embed `KlStream` in `KlConn`, keep the public name; this is a NARROW SOURCE BREAK at
  the field level, not "no break" (Finding 6 + Finding 3).** `KlConn`'s struct is **fully defined in a
  public header** (`include/keel/connection.h`) with fields `fd` / `peer_addr` / `read_buf` / `tls` /
  … — so moving them under `conn->stream.*` **breaks any external source that reads those fields**,
  whether or not *this* repo does. Be honest about the compatibility surface rather than claiming none
  is broken:
  - **Type/API compatibility IS preserved:** the type **name** `KlConn`, `sizeof(KlConn)`
    embeddability, and the documented accessor `kl_request_conn()` (returns `KlConn*`) — all unchanged.
  - **ABI is NOT promised** — Keel is statically relinked; struct layout may change between versions.
  - **Direct field-level source compatibility IS intentionally broken** — `conn->fd` becomes
    `conn->stream.fd`, etc. This is acceptable **only because the transport fields are hereby
    classified as internal/unstable** (a survey of `include/`/`examples/`/`tests/` finds no external
    reader — the only mention is one `peer_addr` doc comment). Phase A must **document that
    classification** in `connection.h` (mark the transport subset internal/unstable) so the break is
    declared, not silent.
  - **Ships:** keep the public struct **name** `KlConn`; move the transport subset into an embedded
    `KlStream stream;` leading member; migrate references from `conn->fd` / `conn->ctx` /
    `conn->peer_addr` / `conn->read_buf|read_len|read_cap` / `conn->read_paused` / `conn->tls` /
    `conn->tls_want` to `conn->stream.fd` (etc.). The HTTP fields (`req`/`res`/`parser`/`ws`/`h2`/
    `router`/`file_io`/`access_log`) stay top-level on `KlConn`. Add `kl_conn_peer_addr(KlConn*)` (and
    any other accessor genuinely needed) so external users have a *stable* path that survives the move.
  - **If true field-level source compatibility is later required** (a downstream turns out to read the
    fields): retain thin `#define`/accessor aliases through a deprecation cycle rather than reverting
    the embed. Not planned, since the fields are classified internal.
  - **Rejected:** field-level aliasing (a) (duplicated members / macro aliases — ugly, collision-prone,
    and unnecessary since nothing external reads the fields); `typedef KlHttpConn KlConn` (b) (a rename
    in disguise that still wouldn't preserve `conn->fd` field access, which we don't need anyway); a
    source-breaking public rename (c) (out of scope).
  Embedding keeps the allocation-free accept path (the stream is inline in the pooled `KlConn`, not
  `malloc`d).

### Phase B — baseline public contract (needs NEW machinery; this is the shippable API)
This is the "target public surface." It is NOT pure extraction — each item below is new work:
- **Atomic write + reservation** — the `KlStreamWriteResult` API with all-or-none acceptance, built
  on a `KlDrain` **reservation** op (reserve remainder-capacity before any provider send; see the
  fast-path note below). New `KlDrain` extension.
- **High/low-water writable notification** — one `on_writable` at low-water (extends `KlDrain`'s
  empty-only `on_drain`).
- **Strict pause** — hold an already-outstanding completion recv's bytes until resume.
- **Graceful close + detachment + `on_close`** — deferred close (flush accepted output within a
  close deadline; degrade to discard on timeout), and the DETACHMENT-based `on_close` terminal
  (confirmed non-reference to caller storage, NOT physical token retirement — EFI-safe). New
  lifecycle machinery.
- **Cancellable `KlConnectOp`** handle (caller-owned, deferred terminal) + a coarse **stream-level**
  cancel (stop delivery, cancel-by-handle, detach, `on_close`). Accept is push-only (`on_accept`,
  no public per-accept handle — cancel by closing the listener); see contract §8.2.
- **Listener credit + acquire/release** — the listener returns an accept credit when a stream
  closes; listener close detaches outstanding accepts. Capacity is listener-intrinsic, not
  `KlServer`-scoped.

> **Write fast-path (review Finding 3):** copying is NOT unconditional. Reserve enough queue
> capacity for a possible remainder, then attempt the direct send; if the provider consumes **all**
> bytes inline (readiness fast path), **no snapshot** is made; if it consumes a prefix, copy only the
> remainder into the already-reserved stream-owned queue; completion submissions always reference
> stream-owned stable storage, never caller memory. This keeps atomic caller-visible acceptance +
> immediate caller-buffer release + Keel's zero-copy small-write fast path.

### Phase C — advanced semantics (later)
- **Per-stream-operation terminals** — read-op / write-op operation identity records so cancel/
  timeout deliver exactly one terminal *per op* (Phase B only has coarse stream-level cancel).
- **Model-neutral EOF vs RST** — requires extending `KlCompletionEvent` (today only `bytes`+`ok`,
  no `KlIoStatus` category) so completion backends distinguish orderly FIN from reset like the
  readiness seam.
- **TLS-wrapped stream** — `KlTls` is reusable substrate, but lifting its HTTP completion driver
  into a generic TLS-wrapped stream is NEW orchestration machinery: handshake-before-delivery,
  WANT_READ-from-write / WANT_WRITE-from-read, drain `tls->pending()` without another event,
  memory-BIO ciphertext ownership, close-notify vs FIN, handshake deadline.

### Optional — capabilities that are NOT in the baseline contract
- **Half-close (`shutdown_write`)** — `KlSocketOps` has **no** `shutdown` op today; new provider work
  (POSIX/Winsock/lwIP-raw/EFI impls or explicit UNSUPPORTED) + separate read-open/write-open state +
  completion error-info changes + new lifecycle tests. Not assumed.
- **Abort / RST** — likewise no `abort` op exists; new provider work.
- **Tagged `KlEndpoint` union** — for a real non-socket address (named pipe / vsock) only.
- **File-transfer extensions** (`sendfile`/`KlFileIO`) — never core stream contract.

### Endpoint + datagram + sync-client corrections
- **`KlEndpoint`:** start as **`KlSockAddr` directly** (already neutral: IPv4/IPv6/Unix). Do NOT
  add `TCP`/`Unix`/`lwIP`/`EFI` "kinds" — provider selection stays in `KlEventCtx`/config; mixing
  provider identity into the endpoint would violate the very axis separation this project
  established. A tagged union comes only when a genuinely non-socket address type appears (Optional).
- **`KlDatagram`/UDP:** leave `KlUdp` as-is; keep separate from streams.
- **Sync client:** a push-only `KlStream` cannot literally replace `client_sync.c`'s blocking loop.
  The consolidation makes the **sync client an adapter that drives the generic async core**
  (runs/polls it to completion), not "the sync transport loop disappears."
- **`KlListener` capacity:** listener-intrinsic accept credit (the "configured capacity" today is
  *server-pool* policy) — accept is gated by that credit, the accepted stream is drawn from the
  caller-supplied pool via the listener's acquire/release seam, and the credit is returned when a
  stream closes. Not by reaching into `KlServer`.

**Migration order (test-driven):** (a) **Phase A** structural extraction (embedded `KlStream`,
retyped recv/send + listener accept, coarse close) — internal, no API; (b) **Phase B** ship the
baseline API (atomic write+reservation incl. `TOO_LARGE`, strict pause, graceful close/detachment,
the `KlConnectOp` handle + push `on_accept`, listener credit leases) and lock it with the Phase-8
length-prefixed echo test over TCP +
pollcomp; (c) client transport → `KlStream` (+ sync adapter); (d) server transport
(`KlHttpConn { KlStream stream; … }`); (e) **Phase C** EOF/RST taxonomy + TLS wrapping; (f) WS/H2
reuse. Each step green + sanitizer-clean.

---

## 9. Phase-1 conclusion (corrected v2)

The consolidation is **structural extraction (Phase A) + a baseline-contract build that needs new
machinery (Phase B) + advanced semantics (Phase C) + optional capabilities** — NOT "extraction +
retyping" wholesale, and Phase A alone is not the shippable API:
- The **substrate** (socket provider, sockaddr, handle, event/completion negotiation) is generic
  and model-negotiated — reuse unchanged.
- **Phase A** (semantics-preserving structural extraction — NOT a no-logic retype): carve the
  embedded `KlStream` + `KlListener` out of `KlConn`/`KlServer`, **move the receive-buffer boundary
  above the provider** (raw `post_recv(stream, buf, cap)` across all backends; HTTP TLS driver becomes
  the interim ciphertext caller), and retype `KlCompletionOps` recv/send/accept onto them behind the
  existing dispatch — preserves observable behavior + the allocation-free accept path, but does change
  backend logic.
- **Phase B** (the shippable public surface) is NOT free: atomic write + `KlDrain` reservation (with
  `TOO_LARGE` for oversized writes), low-water writable, strict pause, graceful close + DETACHMENT-based
  `on_close` (with the class-split cancel-to-completion vs synchronous-ack teardown), the cancellable
  `KlConnectOp` handle + push `on_accept` + listener credit leases — all new work.
- **Phase C**: model-neutral EOF-vs-RST (needs an extended completion event), per-op terminals, and
  the TLS-wrapped-stream state machine (`KlTls` is reusable substrate, but its generic integration is
  new orchestration, not a retype). **Half-close/abort/file-transfer/non-socket endpoints are
  Optional.**
- Accordingly `docs/stream_contract.md` stays **DRAFT** (not normative) until the Phase-B foundations
  land; its capability tiers map to these phases (Tier-1 capabilities = Phase B surface, etc.).
