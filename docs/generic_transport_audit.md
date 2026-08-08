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
transport-operation lifecycle (per-stream: one read + one write op record, plus separately
cancellable connect/accept handles, each with a generation + terminal flag) — NOT a futures
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

## 8. Recommended consolidation — work split into three tiers (review reset)

> **Correction (review):** an earlier draft framed the whole job as "extraction + retyping." That
> understates it. Only some of it is extraction; some is genuinely **new machinery**; some is
> **optional capability** that must not be smuggled into the first cut. The work is therefore
> split into three explicit tiers, and `docs/stream_contract.md` is downgraded to **DRAFT** until
> the Tier-2 choices are resolved.

### Tier 1 — pure extraction / retyping (low risk, evolve existing code)
- **`KlStream`** = the transport subset of `KlConn`, **embedded** (see below), carrying
  handle + `KlEventCtx*` (→ provider+loop) + `KlSockAddr` peer/local + read buffer + `read_paused`
  + a `KlDrain`. Plaintext-only at first (TLS is Tier 2).
- Retype the completion I/O target: `KlCompletionOps.post_recv`/`post_send`/`post_sendfile` and the
  readiness `conn_read`/`conn_write` from `KlConn*` to the generic stream handle, behind the
  existing `completion_dispatch.c` seam (no backend logic change).
- Generic read/write **delivery callbacks** and **stream-level** `pause`/`resume` (rename
  `read_paused`; keep the existing mechanism).
- Existing close semantics (recv≤0 → close) exposed as `kl_stream_close`.
- **Embed, do not pointer-ize:** `KlConn { KlStream stream; <HTTP state> }` (rename the HTTP object
  to `KlHttpConn` internally; container-of where needed). Keel preallocates connection + TLS slots;
  a separately-allocated `KlStream` would add per-accept allocation or a second pool. Embedding
  preserves locality + the allocation-free accept path.

### Tier 2 — required NEW machinery (must be designed before it can be normative)
- **Write-result / backpressure API** — a concrete result type (status + accepted-count +
  `KlIoStatus`), and a chosen policy (atomic-accept vs prefix-accept). `KlDrain` today is
  0/-1 atomic-accept with a sticky cap and empty-only drain callback — it needs an extension or an
  explicit atomic-accept wrapper, plus separate high/low-water if the writable signal is low-water.
- **Operation identity + cancellation** — `KlAsyncOp` is HTTP-suspension and `kl_comp_cancel` is
  by-handle (§2.5), so per-op cancel does not exist. Build either a small transport-op lifecycle
  (per-stream one-read + one-write op record with generation + terminal flag; separately
  cancellable connect/accept handles) OR ship a coarser **stream-level** cancel first.
- **Deferred-destruction ownership** — a normative UAF boundary (§ stream_contract close section):
  who frees the stream, whether `close` is sync or deferred, how the caller learns teardown
  finished (an `on_close`/`closed` terminal), whether pending callbacks are suppressed or delivered
  as cancellation. Must interoperate with the EFI quarantine (retirement is *confirmed*, not
  "immediate").
- **TLS-wrapped stream state machine** — plaintext ships first; TLS wrapping is a later phase with
  its own contract (handshake-before-delivery, WANT_READ-from-write / WANT_WRITE-from-read, drain
  `tls->pending()` without another event, memory-BIO ciphertext ownership, close-notify vs FIN,
  handshake deadline). Keep TLS depending on the generic stream, not on `KlConn`.

### Tier 3 — optional capabilities (NOT in the initial contract)
- **Half-close (`shutdown_write`)** — `KlSocketOps` has **no** `shutdown` op today; this is new
  provider work (POSIX/Winsock/lwIP-raw/EFI impls or explicit UNSUPPORTED) + separate
  read-open/write-open state + completion error-info changes + new lifecycle tests. Add later as an
  optional capability, or promote to Tier 2 with the new-work acknowledged — do not assume it.
- **Abort / RST** — likewise no `abort` op exists; new provider work.
- **Tagged `KlEndpoint` union** — for a real non-socket address (named pipe / vsock) only.
- **File-transfer extensions** (`sendfile`/`KlFileIO`) — optional, never core stream contract.

### Endpoint + datagram + sync-client corrections
- **`KlEndpoint`:** start as **`KlSockAddr` directly** (already neutral: IPv4/IPv6/Unix). Do NOT
  add `TCP`/`Unix`/`lwIP`/`EFI` "kinds" — provider selection stays in `KlEventCtx`/config; mixing
  provider identity into the endpoint would violate the very axis separation this project
  established. A tagged union comes only when a genuinely non-socket address type appears (Tier 3).
- **`KlDatagram`/UDP:** leave `KlUdp` as-is; keep separate from streams.
- **Sync client:** a push-only `KlStream` cannot literally replace `client_sync.c`'s blocking loop.
  The consolidation makes the **sync client an adapter that drives the generic async core**
  (runs/polls it to completion), not "the sync transport loop disappears."
- **`KlListener` capacity:** define a precise ownership model — listener-owned accepted-stream pool
  + a capacity credit (the "configured capacity" today is *server-pool* policy, not intrinsic to a
  listener). Accept is capacity-gated via that credit, not by reaching into `KlServer`.

**Migration order (test-driven):** (a) Phase-8 length-prefixed echo test against the Tier-1
`KlStream`/`KlListener` (plaintext, TCP + pollcomp) to lock the contract; (b) client transport →
`KlStream` (+ sync adapter); (c) server transport (`KlHttpConn { KlStream stream; … }`);
(d) TLS wrapping (Tier 2); (e) WS/H2 reuse. Each step green + sanitizer-clean.

---

## 9. Phase-1 conclusion (corrected)

The consolidation is **mostly extraction, partly new machinery, and some optional capability** —
not "extraction + retyping" wholesale:
- The **substrate** (socket provider, sockaddr, handle, event/completion negotiation) is generic
  and model-negotiated — reuse unchanged.
- The **transport object** is missing (dissolved in `KlConn`, re-implemented in the client) — Tier 1
  carves it out, embedding it and retyping `KlCompletionOps` onto the stream handle.
- But **per-op cancellation, a real write-result/backpressure API, deferred-destruction ownership,
  and TLS-wrapped-stream semantics do NOT exist today** — Tier 2 must design them before the
  contract is normative. **Half-close/abort are new provider work** — Tier 3.
- Accordingly `docs/stream_contract.md` is marked **DRAFT** (not normative) until the Tier-2 choices
  land, and its over-promised capabilities are demoted to their correct tier.
