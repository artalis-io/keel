# Phase 8b-4, UDP over the completion loop, Design

**Status:** designed, implementation gated. Builds on 8b-0..3 (the platform-independent
completion axis + the TCP connection driver). Larger than 8b-1..3: `KlUdp` is
readiness/watcher-based, so completion support generalizes the completion axis and
the `KlEventCtx` run model. Windows-runtime-validated (Windows-IOCP CI).

**Scope (locked with the user):**
- **Standalone `KlUdp` too**, not just a `udp_server` sharing the TCP loop, so
  `kl_event_ctx_run` (the generic/standalone tick) gains a completion path, and
  client-side UDP (e.g. the DNS resolver) can run over a completion loop.
- **Overlapped sends**: `kl_udp_send_to` posts `WSASendTo` and the capped send
  queue drains on send-completions (not just synchronous `sendto`).

**Governing orthogonality (the whole point):**
1. **Readiness loop untouched.** On a readiness loop `KlUdp` uses its watcher +
   `recvmsg`/`sendmsg` path exactly as today; only a completion loop takes the new
   path. Byte-identical on POSIX.
2. **`KlUdp` public API untouched.** `kl_udp_recv_start` / `kl_udp_send_to` / the
   `on_recv`(`_segments`) callbacks / every config knob are unchanged, a handler
   never learns whether a datagram arrived via `recvmsg` or `WSARecvFrom`. No
   `include/keel/*.h` change.
3. **Completion is a concept; IOCP is one implementation.** The datagram completion
   *logic* (post a recv, on completion deliver to `on_recv`, re-post) is
   platform-independent; `WSARecvFrom`/`WSASendTo`/`OVERLAPPED` live only in the
   IOCP backend. A future io_uring/AIO backend reuses the logic.

---

## 1. The problem

`KlUdp` receives by arming a **readiness watcher** on its `KlEventCtx`:
`kl_watcher_add(udp->ctx, udp->fd, KL_EVENT_READ, udp_on_ready, udp)` (`udp.c:42`),
and `udp_on_ready` does `recvmsg`. On a **completion** loop (`event_iocp.c`),
readiness watchers never fire: `kl_event_wait` reports nothing; the kernel only
delivers *completions* for posted overlapped ops. So datagrams must arrive via a
posted `WSARecvFrom` completion, not a watcher.

Two consumers must work on a completion loop:
- a **`udp_server`** sharing the TCP server's loop (the server run loop already runs
  the completion tick), and
- a **standalone `KlUdp`** on its own `KlEventCtx`, driven by `kl_event_ctx_run`,
  which today is readiness-only (`kl_event_wait` + watcher dispatch), so it too needs
  a completion path. This is the generalization the broad scope requires: today
  *only the TCP server* runs on a completion loop.

---

## 2. Generalize the completion axis to heterogeneous consumers

8b-0's `KlCompletionEvent` is TCP-connection-shaped (`KlConn *conn` + ACCEPT/READ/
WRITE). To carry datagram completions to a `KlUdp`, the event becomes
**consumer-agnostic**, a `void *target` + a widened kind set, while the *generic
driver* routes each event to the right handler:

```c
/* completion.h (internal): additions, no Win32 type */
typedef enum {
    KL_COMP_ACCEPT, KL_COMP_READ, KL_COMP_WRITE,   /* TCP conn (target = KlConn* / KlServer* for accept) */
    KL_COMP_UDP_RECV, KL_COMP_UDP_SEND             /* datagram (target = KlUdp*) */
} KlCompKind;

typedef struct {
    void       *target;     /* KlConn* (READ/WRITE) · KlServer* (ACCEPT) · KlUdp* (UDP_*) */
    KlCompKind  kind;
    size_t      bytes;
    int         ok;
    void       *buf;        /* UDP_RECV: the datagram buffer the op used */
    struct sockaddr_storage addr;   /* UDP_RECV src / ACCEPT peer */
    socklen_t   addr_len;
    KlSocketHandle accepted_fd;     /* ACCEPT */
} KlCompletionEvent;

/* Datagram post primitives (backend-implemented; IOCP = WSARecvFrom / WSASendTo). */
int kl_comp_post_udp_recv(KlUdp *udp);                                   /* into udp's recv buffer */
int kl_comp_post_udp_send(KlUdp *udp, const void *data, size_t len,
                          const struct sockaddr *dst, socklen_t dlen);   /* overlapped WSASendTo */
```

**One generic completion tick, shared by both run loops**, the routing lives in the
platform-independent `completion_driver.c`:

```c
/* completion_driver.c: platform-independent. Drains the ctx's loop and routes each
 * event to its consumer. No Win32 symbol. Used by BOTH the server run loop and the
 * standalone kl_event_ctx_run. */
int kl_comp_run(KlEventCtx *ctx, int max, int timeout_ms) {
    KlCompletionEvent ev[N];
    int n = kl_comp_drain(ctx, ev, N, timeout_ms);
    for (i < n) switch (ev[i].kind) {
        case KL_COMP_ACCEPT: comp_on_accept((KlServer*)ev[i].target, &ev[i]); break;
        case KL_COMP_READ:   { KlConn *c = ev[i].target; comp_on_read(server_of(c), c, &ev[i]); } break;
        case KL_COMP_WRITE:  { KlConn *c = ev[i].target; comp_on_write(server_of(c), c, &ev[i]); } break;
        case KL_COMP_UDP_RECV: kl_udp_comp_on_recv((KlUdp*)ev[i].target, &ev[i]); break;
        case KL_COMP_UDP_SEND: kl_udp_comp_on_send((KlUdp*)ev[i].target, &ev[i]); break;
    }
    return n;
}
```

- `server_of(c)` recovers the server from a connection **without** a public-header
  field: `containerof(c->ctx, KlServer, ev)` (every pooled conn's `ctx == &server->ev`;
  internal idiom, no `KlConn`/`KlServer` layout change on the public API).
- `kl_comp_drain(ctx, …)` takes the **ctx** (not the server) so it is server-agnostic
  , a standalone UDP loop drains the same way. (The server-scoped accept priming from
  8b-0..3 moves to server startup, §4.)

The existing `kl_io_engine_run_completion(s)` (the server's io_engine seam) becomes a
one-liner: `return kl_comp_run(&s->ev, …)`.

---

## 3. `kl_event_ctx_run` gains a completion path (standalone consumers)

`kl_event_ctx_run` (async.c) is the standalone/client tick (readiness `kl_event_wait`
+ watcher dispatch). It branches once on the loop model:

```c
int kl_event_ctx_run(KlEventCtx *ctx, int max, int timeout) {
    if (kl_event_caps(&ctx->loop) & KL_EVENT_CAP_COMPLETION)
        return kl_comp_run(ctx, max, timeout);      /* completion consumers (UDP, ...) */
    ... existing readiness wait + dispatch ...       /* unchanged */
}
```

`kl_comp_run` is defined in `completion_driver.c` (completion builds) and **stubbed**
in `io_engine.c` (non-completion builds, never reached, since no readiness backend
advertises `COMPLETION`), exactly like `kl_io_engine_run_completion` today, so async.c
links on every platform with no `#ifdef`.

---

## 4. `KlUdp`: pick the path by the loop's model (readiness untouched)

`kl_udp_recv_start` chooses at arm time, from `kl_event_caps(&udp->ctx->loop)`:

```c
if (kl_event_caps(&udp->ctx->loop) & KL_EVENT_CAP_COMPLETION)
    kl_comp_post_udp_recv(udp);          /* completion: post WSARecvFrom(s) */
else
    kl_watcher_add(udp->ctx, udp->fd, …, udp_on_ready, udp);   /* readiness: unchanged */
```

- **Recv:** the completion path posts `kl_comp_post_udp_recv` (one or a small pool of
  outstanding `WSARecvFrom`s into the udp recv buffer + a per-op source-addr buffer).
  On completion, `kl_udp_comp_on_recv(udp, ev)` calls the **existing** datagram
  delivery core (the GRO-split / `on_recv_segments` / `on_recv` logic in `udp.c`,
  lifted behind an internal `kl_udp_deliver(udp, buf, len, src, …)`: model-blind,
  shared with `udp_on_ready`), then re-posts. `on_recv` is fed identical bytes.
- **Send (overlapped):** `kl_udp_send_to` on a completion loop enqueues then posts
  `kl_comp_post_udp_send` (`WSASendTo`); the capped send queue drains on
  `KL_COMP_UDP_SEND` completions (`kl_udp_comp_on_send` → dequeue next / fire the
  on-drain callback). The public queue cap + `on_drain` semantics are unchanged; only
  the *trigger* moves from write-readiness to send-completion.
- **`udp_server`** shares the TCP server's `KlEventCtx`, so its recvs are drained by
  the server's `kl_comp_run` tick; a **standalone `KlUdp`** is drained by
  `kl_event_ctx_run` (§3). Either way `KlUdp` posts through the same axis.

Server accept priming (8b-0's first-drain `iocp_start`) moves to the server's
listen-socket setup so `kl_comp_drain` stays server-agnostic; UDP recv priming is the
`kl_comp_post_udp_recv` at `recv_start`.

---

## 5. IOCP implementation (event_iocp.c only)

- `kl_comp_post_udp_recv`: `WSARecvFrom` into the udp buffer, op tagged UDP_RECV with
  a per-op `sockaddr_storage` for the source; the completion key / `CONTAINING_RECORD`
  carries the `KlUdp*`.
- `kl_comp_post_udp_send`: `WSASendTo`, op tagged UDP_SEND (target `KlUdp*`), the
  datagram copied into the op (owned until completion).
- `kl_comp_drain`: recognizes UDP ops → yields `KL_COMP_UDP_RECV`/`_SEND` events
  (source addr via the op's addr buffer). No partial handling for datagrams (a UDP
  send/recv is whole-datagram).
- The udp socket is associated with the port at `recv_start` (the abstract
  `kl_event_add` = associate).

All `WSA*`/`OVERLAPPED` stays here; `completion_driver.c`, `udp.c`, and the delivery
core carry **zero** Win32 symbols.

---

## 6. Orthogonality litmus (grep-assertable)

- **Axis 1 (public API):** no `include/keel/*.h` change; `KlUdp`/`KlConn` public
  layout and the udp/response/server APIs are untouched (`server_of` uses
  `containerof`, not a new field).
- **Axis 2 (readiness untouched):** `udp_on_ready` / `recvmsg` / watcher path
  unchanged; the model branch is a single `kl_event_caps` check. POSIX byte-identical
  (`make test`, incl. the udp suites).
- **Axis 3 (concept vs impl):** 0 Win32/IOCP symbols in `completion_driver.c`,
  `completion.h`, `udp.c`, or the delivery core; they live only in `event_iocp.c`.

---

## 7. Staging (each a CI-green increment)

| Increment | What | Validated |
|---|---|---|
| **8b-4a** | Generalize the axis: `KlCompletionEvent.target` (void*), the shared `kl_comp_run` tick, `server_of` containerof; `kl_io_engine_run_completion` → wrapper. **Pure refactor** of 8b-0..3. | TCP smoke stays green; POSIX byte-identical |
| **8b-4b** | `kl_event_ctx_run` completion branch + `kl_comp_run` stub in io_engine.c. | POSIX byte-identical; standalone loop reaches the tick |
| **8b-4c** | UDP **recv** over completion: `kl_udp_deliver` lift + `kl_comp_post_udp_recv` + `kl_udp_comp_on_recv` + `recv_start` model branch. | Windows-IOCP UDP echo smoke (recv) |
| **8b-4d** | UDP **overlapped send**: `kl_comp_post_udp_send` + queue-drain on completion. | Windows-IOCP UDP roundtrip smoke |

8b-4a/b are platform-neutral refactors (fully POSIX-testable); 8b-4c/d are the
Windows-runtime pieces (compile-gated + a new `smoke_iocp_udp` on the Windows-IOCP CI
job).

---

## 8. Risks & stop conditions

- **`server_of` containerof** assumes every completion-driven conn's `ctx` is a
  `KlServer.ev`. True for pooled server conns (the only source of conn ops); asserted.
  If a non-server conn ever posts completion ops, stop, the assumption breaks.
- **Datagram buffer lifetime** across an outstanding `WSARecvFrom`/`WSASendTo` (the op
  owns the send copy; the recv buffer must not be reused until completion; bound by
  a small fixed outstanding-op count).
- **`kl_event_ctx_run` generalization** touches the shared standalone tick; must stay
  byte-identical on readiness (the branch is dead there). Litmus + `make test`.
- **Runtime-only validation** for 8b-4c/d via the Windows-IOCP UDP smoke, iterated
  against CI as in 8a.

## 9. Out of scope

- Batch/offload datagram paths (`recvmmsg`/GSO/GRO) over completion, the completion
  recv is one-datagram-per-op first; batching is a later refinement.
- Full HTTP client over IOCP (the async client's own completion path): separate.
- TLS over IOCP (8b-5), the last and deepest increment.
