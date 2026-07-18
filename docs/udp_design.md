# UDP Support — Design

Status: **Layers A (`KlUdp`) + C-server (`KlUdpServer`) done; `dns_resolver` planned.**
Decisions taken (2026-07-18):
- Deliver **three layers**: the `KlUdp` socket primitive, a built-in async DNS
  resolver over it, and a `KlUdpServer` datagram-dispatch surface.
- Keep the socket layer **general-purpose and portable now**; defer QUIC/HTTP-3
  machinery (source-addr on wildcard binds, GSO/GRO, ECN, `recvmmsg`) until
  QUIC lands.
- Send path **buffers internally with a byte cap** (KlDrain-style), dropping
  whole datagrams only when the cap is exceeded.

## Motivation

UDP is the one missing *primitive* in Keel's event model — everything today is
`SOCK_STREAM`. Adding it unblocks, in priority order:

1. **A truly non-blocking async DNS resolver.** Today `KlClient` falls back to
   blocking `getaddrinfo` (on the thread pool) unless the user brings c-ares.
   A UDP DNS resolver makes the async client non-blocking out of the box.
2. **HTTP/3 / QUIC** (separate future epic) — needs a UDP datagram loop first.
3. **Datagram services** — syslog, statsd/metrics push, service discovery,
   DTLS, game/telemetry protocols — served on the *same* event loop as HTTP.

It sits directly on the existing `KlEventCtx` / `KlWatcher` abstraction: a UDP
socket is a non-blocking fd registered for `KL_EVENT_READ` (and `KL_EVENT_WRITE`
only while the send queue is draining). No new event-loop concepts.

---

## Architecture fit

Three new modules, each independently testable, mirroring Keel's conventions
(`kl_` prefix, `KlError last_error`, paired `_init`/`_free`, bring-your-own
allocator, zero allocation on the success hot path):

| Module | Header | Depends on |
|--------|--------|------------|
| `udp`          | `udp.h`          | `event_ctx`, `allocator`, `error` |
| `dns_resolver` | `dns_resolver.h` | `udp`, `resolver` (implements the vtable), `timer` |
| `udp_server`   | `udp_server.h`   | `udp` |

`dns_resolver` and `udp_server` are thin consumers of `udp` — the primitive
carries all the socket/backpressure complexity.

**Allocation discipline.** The recv buffer is pre-allocated at `init`. The
success path (immediate `sendto`, `recvfrom` into the fixed buffer) allocates
nothing. The *only* allocation is a queued datagram node under backpressure
(EAGAIN) — the same trade `KlDrain` already makes, and off the hot path.

---

## A. `KlUdp` — socket primitive

```c
typedef struct KlUdp KlUdp;

/** Called once per received datagram, on the event-loop thread. */
typedef void (*KlUdpRecvFn)(KlUdp *udp, const void *data, size_t len,
                            const struct sockaddr *src, socklen_t src_len,
                            void *user_data);

/** Called when the send queue transitions from non-empty back to empty. */
typedef void (*KlUdpDrainFn)(KlUdp *udp, void *user_data);

typedef struct {
    KlEventCtx  *ctx;            /* event loop (borrowed — must outlive udp) */
    int          family;        /* AF_INET / AF_INET6 / AF_UNSPEC (auto from bind_addr) */
    const char  *bind_addr;     /* NULL = unbound (client sockets) */
    uint16_t     bind_port;     /* 0 = ephemeral */
    size_t       recv_buf_size; /* per-datagram recv buffer; 0 = 2048, max 65535 */
    size_t       max_send_queue;/* backpressure cap in bytes; 0 = 256 KiB */
    int          reuse_addr;    /* SO_REUSEADDR */
    int          reuse_port;    /* SO_REUSEPORT (fan-out across workers) */
    KlAllocator *alloc;         /* NULL = default */
} KlUdpConfig;

int   kl_udp_init(KlUdp *udp, const KlUdpConfig *cfg);
void  kl_udp_free(KlUdp *udp);

/* Optional: fix the peer. Enables kl_udp_send() and filters inbound source. */
int   kl_udp_connect(KlUdp *udp, const struct sockaddr *peer, socklen_t peer_len);

/* Receive: register READ interest; on_recv fires once per datagram. */
int   kl_udp_recv_start(KlUdp *udp, KlUdpRecvFn on_recv, void *user_data);
void  kl_udp_recv_stop(KlUdp *udp);

/* Send. Returns 0 if sent or queued, -1 on error / over-cap (last_error set). */
int   kl_udp_send_to(KlUdp *udp, const void *data, size_t len,
                     const struct sockaddr *dest, socklen_t dest_len);
int   kl_udp_send(KlUdp *udp, const void *data, size_t len);  /* connected peer */

/* Backpressure introspection. */
void     kl_udp_on_drain(KlUdp *udp, KlUdpDrainFn cb, void *user_data);
size_t   kl_udp_send_queued(const KlUdp *udp);  /* bytes currently queued */
uint64_t kl_udp_dropped(const KlUdp *udp);      /* datagrams dropped over cap */

int      kl_udp_fd(const KlUdp *udp);
KlError  kl_udp_last_error(const KlUdp *udp);
```

### Receive semantics

On `KL_EVENT_READ`, drain with `recvfrom` into the pre-allocated buffer, calling
`on_recv` per datagram until `EAGAIN`. To preserve event-loop fairness under
edge-triggered backends, cap at **N datagrams per tick** (default 64); if more
remain, leave READ armed / re-post so the loop round-robins other fds instead of
starving on one busy socket. Oversized datagrams (`> recv_buf_size`, flagged by
`MSG_TRUNC`) are delivered truncated with `last_error = KL_ERR_TOO_LARGE` and a
counter bump — never a buffer overflow.

`src` points into a per-`udp` scratch `sockaddr_storage`, valid only for the
duration of the `on_recv` call — copy it to keep it.

### Send semantics + backpressure (KlDrain-style)

`kl_udp_send[_to]` attempts an immediate `sendto`. UDP `sendto` is all-or-nothing
(no partial datagram), so:

- **Success** → return 0, nothing queued.
- **EAGAIN** → copy the datagram (+ dest addr) into a FIFO queue, add
  `KL_EVENT_WRITE` interest via `kl_watcher_mod`; on writability, flush oldest-first
  until EAGAIN or empty. When the queue empties, drop WRITE interest and fire
  `on_drain`.
- **Queue would exceed `max_send_queue`** → drop the datagram, bump
  `kl_udp_dropped`, set `last_error = KL_ERR_QUEUE_FULL`, return -1. (UDP is lossy
  by contract; a bounded queue prevents unbounded memory growth under a stalled
  socket.)
- **Hard error** (ECONNREFUSED on connected socket, etc.) → -1, `KL_ERR_IO`.

Queue nodes are the only heap allocation, and only under backpressure.

---

## B. Built-in async DNS resolver (`dns_resolver`)

Implements the existing `KlResolver` vtable (`resolver.h`) over a `KlUdp` socket,
so it drops straight into `KlClientConfig.resolver` and replaces the blocking
`getaddrinfo` fallback.

```c
typedef struct {
    const char  *nameserver;  /* NULL = parse /etc/resolv.conf, else "1.1.1.1" */
    uint16_t     port;        /* 0 = 53 */
    int          timeout_ms;  /* per attempt; 0 = 5000 */
    int          attempts;    /* retransmits before failing; 0 = 2 */
    int          prefer_ipv6; /* try AAAA first, fall back to A */
    KlAllocator *alloc;
} KlDnsResolverConfig;

/* Returns a KlResolver* to plug into KlClientConfig.resolver.
 * Freed via the vtable's destroy(). */
KlResolver *kl_dns_resolver_create(KlEventCtx *ctx, const KlDnsResolverConfig *cfg);
```

**Behaviour:**
- One shared `KlUdp` connected to the nameserver; per-query 16-bit transaction
  IDs mapped to in-flight `KlResolveReq`s. Response matched by txn id + question.
- Builds A and/or AAAA queries; parses the response header, skips the question,
  walks answer RRs, extracts `A`/`AAAA`, chases in-packet `CNAME`. Fills
  `KlResolveResult` (`sockaddr_storage` + family) and calls `done_fn` on the
  event-loop thread.
- **Timeout + retransmit** via `KlTimer` (min-heap already in `KlEventCtx`):
  resend up to `attempts`, then `done_fn(..., KL_ERR_TIMEOUT, ...)`.
- **Literal IPs** short-circuit (no query). Because the `KlResolver` contract
  permits **synchronous completion** (see `resolver.h` / `resolver_cache.c`), the
  literal-IP path may call `done_fn` inside `resolve()` — documented, and the
  cache decorator already handles it.
- Errors: `NXDOMAIN`/`SERVFAIL` → `KL_ERR_DNS`; truncated (`TC` bit) response →
  `KL_ERR_DNS` for now (TCP-fallback deferred; note below).
- **Security:** the response parser consumes untrusted network input — it gets a
  dedicated **libFuzzer target** (`fuzz_dns`) alongside the existing parser/multipart
  fuzzers, and bounds every pointer against the packet end (compression-pointer
  loops capped, no unbounded label chains).

**Deferred (documented, not built):** TCP fallback on the `TC` truncation bit,
EDNS0 buffer sizing, DNSSEC, `/etc/hosts`, search domains, mDNS. `search`/`ndots`
from `resolv.conf` can be a fast follow.

---

## C. `KlUdpServer` — datagram dispatch

A bind + handler surface symmetric with `KlServer`, for line/datagram services.
A thin wrapper over `KlUdp`.

```c
typedef struct KlUdpServer KlUdpServer;

typedef void (*KlUdpHandlerFn)(KlUdpServer *s, const void *data, size_t len,
                               const struct sockaddr *src, socklen_t src_len,
                               void *user_data);

typedef struct {
    const char  *bind_addr;    /* "0.0.0.0" / "::" */
    uint16_t     port;
    size_t       recv_buf_size;
    size_t       max_send_queue;
    int          reuse_port;   /* fan-out across SO_REUSEPORT workers */
    KlAllocator *alloc;
} KlUdpServerConfig;

int   kl_udp_server_init(KlUdpServer *s, KlEventCtx *ctx,
                         const KlUdpServerConfig *cfg,
                         KlUdpHandlerFn handler, void *user_data);
/* Reply to the sender (or any dest) from inside the handler. */
int   kl_udp_server_reply(KlUdpServer *s, const void *data, size_t len,
                          const struct sockaddr *dest, socklen_t dest_len);
void  kl_udp_server_free(KlUdpServer *s);
```

Crucially it **shares a `KlEventCtx`** — it does not own a thread or loop. One
process can serve TCP HTTP (`KlServer`) *and* a UDP service on a single event
loop, or run standalone via `kl_event_ctx_run`. Multi-core scaling is the same
horizontal `SO_REUSEPORT` story as the TCP server.

---

## QUIC / HTTP-3 readiness (deferred, forward-compatible)

The general-purpose API is chosen so the QUIC-specific bits slot in additively,
without breaking callers:

- **Source addr on wildcard binds** — QUIC servers bound to `0.0.0.0`/`::` must
  reply *from the exact local address* the client hit. This needs
  `IP_PKTINFO`/`IPV6_RECVPKTINFO` (capture local addr on recv) and setting it on
  send. Added later as a `KlUdpConfig` opt-in flag plus a richer recv variant
  (`KlUdpRecvMeta { local_addr, ecn, tos }`) that supplements — not replaces —
  `KlUdpRecvFn`.
- **GSO/GRO segmentation offload** (`UDP_SEGMENT`/`UDP_GRO`) and **`recvmmsg`
  batching** — throughput features, opt-in flags on the config; transparent to
  existing consumers.
- **ECN** — carried in the future `KlUdpRecvMeta`.

None of these change the v1 surface; they are strictly additive.

---

## Error handling

Reuses existing `KlError` codes: `KL_ERR_SOCKET`, `KL_ERR_BIND`, `KL_ERR_IO`,
`KL_ERR_TIMEOUT`, `KL_ERR_DNS`, `KL_ERR_QUEUE_FULL` (send over cap),
`KL_ERR_TOO_LARGE` (truncated recv), `KL_ERR_INVALID_ARG`, `KL_ERR_ALLOC`. No new
codes needed for v1.

---

## Testing plan

- **`KlUdp`**: loopback echo round-trip (IPv4 + IPv6); source-addr correctness;
  connected vs unconnected; backpressure (force EAGAIN via tiny `SO_SNDBUF`,
  assert queue growth → `on_drain` → dropped counter past the cap); oversized
  datagram truncation; recv fairness cap. Under ASan/UBSan + `BACKEND=poll`.
- **`dns_resolver`**: stand up a `KlUdpServer` as a **mock nameserver** returning
  canned responses — resolve → correct A/AAAA; timeout + retransmit; NXDOMAIN →
  `KL_ERR_DNS`; CNAME chase; literal-IP shortcut (sync-completion path);
  end-to-end (`KlClient` fetch-by-name through the mock resolver against a local
  `KlServer`). Plus the `fuzz_dns` libFuzzer target on the response parser.
- **`KlUdpServer`**: bind + echo; reply-to-sender; `reuse_port` fan-out; shared
  event loop with a `KlServer` in one process.

---

## Sequencing & effort

1. **`KlUdp` primitive** (~1.5 days) — socket + recv drain + capped send queue +
   tests.
2. **`KlUdpServer`** (~0.5 day) — thin wrapper + tests (also serves as the DNS
   mock harness).
3. **`dns_resolver`** (~2 days) — query build + response parse + `fuzz_dns` +
   timeout/retry + client integration.

Each ships as its own CI-green commit, mirroring the client-identity trio
rollout. Total ≈ 4 days.

---

## Non-goals (this iteration)

- QUIC / HTTP-3 itself — this unblocks it; the transport is a separate epic.
- Multicast group membership (`IP_ADD_MEMBERSHIP`) — add on demand.
- DoT / DoH resolvers (DoH would ride the existing HTTP client), DNSSEC, mDNS.
- Native Windows datagram support — consistent with Keel's POSIX/Cosmopolitan target.
```
