# Phase 7 — Event/socket capability negotiation — Design

**Status:** designed, implementation gated. Depends on Phase 6 (a real non-POSIX
provider exists) and the Phase 4 public provider selection. Additive, medium risk.
Unblocks Phase 8 (IOCP) and Phase 9 (lwIP raw), which are the real consumers.

**Scope decisions (locked with the user, 2026-07-26):**
- **Formalize the contract only.** Give the event backend a capability surface,
  turn the server's *implicit* native-fd assumption into a *checked* event↔socket
  negotiation, and name the readiness-vs-completion vocabulary — but ship **no new
  event model**. `COMPLETION` is a reserved capability name with no backend behind
  it; IOCP/io_uring-completion plumbing is Phase 8.
- **Internal-first.** Negotiation and event-loop capabilities live in the internal
  seam (a new `src/event_caps.h`, consumed by `event_ctx`/server). This doc records
  the intended *public* shape; it is frozen and exposed only once a real consumer
  (Phase 8 IOCP or Phase 9 lwIP-raw) exercises it — mirroring the Phase-4-after-6
  "build the real consumer first, freeze the public API once" decision. There is no
  BYO-event-loop injection API today, so a public event-capability surface would be
  half a bridge (you can pass a provider, but not an event backend).

---

## 1. The problem — an unchecked coupling

Capabilities today live on **one side only**: the socket provider
(`KlSocketProvider.capabilities`, `KL_SOCK_CAP_NATIVE_FD | _WRITEV | _SENDFILE` in
`include/keel/socket.h`). The event loop has **no** capability or model surface —
`KlEventLoop` is `{ int fd; void *_backend; KlAllocator *alloc; }`
(`include/keel/event.h`) and the backend is chosen at **compile time** via the
Makefile `EVENT_SRC` (epoll / kqueue / io_uring / poll / wsapoll).

The server wires the two together on an **assumption**, not a contract
(`src/server.c:337`):

```c
/* A custom socket provider must expose native OS descriptors — the readiness
 * event loop polls them. */
if (s->config.sockets &&
    !kl_socket_provider_has_cap(s->config.sockets, KL_SOCK_CAP_NATIVE_FD)) {
    s->last_error = KL_ERR_SOCKET;
    return -1;
}
```

This checks the **provider** for `NATIVE_FD` and *assumes* the compiled-in event
loop is a readiness poller that can watch native fds. `NATIVE_FD` is doing double
duty — it conflates two independent facts:

1. **"the handle is a real OS descriptor"** (a socket-provider property), and
2. **"this event loop can watch that handle"** (an event-backend property).

Today they always coincide (every backend is readiness-over-native-fd), so the
shortcut is harmless. It stops being harmless the moment a second event model or a
non-native provider appears — exactly Phases 8/9. The lwIP reference already
surfaced this (`examples/lwip/README.md`): its provider and `lwip_poll` event
backend are a *matched set by convention*, with nothing enforcing that the
`NATIVE_FD` provider is watchable by the compiled-in loop.

**Phase 7 makes "can this event loop watch this provider's handles?" a negotiated
query instead of a baked-in assumption** — a seam Phases 8/9 hook into, with
byte-identical behavior today (the readiness+native-fd world is one point in the
new space).

---

## 2. Vocabulary — two orthogonal axes

The negotiation is over two independent properties, each a capability bitset
(mirroring the established `KL_SOCK_CAP_*` style — bit flags, `uint`-wide,
future-extensible):

**Event model** — how the loop reports work:
- `READINESS` — reports read/write readiness; the caller then does the I/O.
  epoll, kqueue, poll, WSAPoll, and io_uring-in-readiness-shape all live here.
- `COMPLETION` — **reserved name, no implementation.** The loop performs the I/O
  and reports it done (IOCP, io_uring true-completion). Named now so 8/9 have a
  value to declare; unadvertised by every current backend.

**Handle domain** — what kind of handle the loop can watch:
- `NATIVE_FD` — OS descriptors (all current backends; lwIP socket API too, since
  `lwip_poll` takes lwIP's small-int fds).
- (future) opaque/provider-specific handles — e.g. lwIP *raw* `tcp_pcb *`, which no
  readiness loop can poll. Reserved implicitly by *absence* of `NATIVE_FD`; no
  named bit until Phase 9 needs one.

The event axis stays **orthogonal to the socket seam** — this is finding **F3**
from `docs/pal_review.md`, a preserved invariant: `src/socket.h` must never include
`event.h`, and vice-versa. Negotiation happens at a neutral third point that
already holds both (§4), never by cross-including.

---

## 3. Internal surface (Phase 7 ships this)

A dedicated internal header, sibling to the `src/socket.h` seam (not the public
`keel/event.h`, keeping the capability surface uncommitted):

```c
/* src/event_caps.h — INTERNAL. No ABI commitment (Phase 7).
 * The compile-time-selected event backend advertises what it can watch, so the
 * wire-up layer can negotiate it against a socket provider. Deliberately NOT in
 * <keel/event.h>: the public event-capability API is frozen only when a real
 * completion/raw consumer lands (Phase 8/9). F3: this header does not include
 * <keel/socket.h>; the compat check (event_ctx) includes both. */
#ifndef KEEL_SRC_EVENT_CAPS_H
#define KEEL_SRC_EVENT_CAPS_H
#include <keel/event.h>

#define KL_EVENT_CAP_READINESS  (1u << 0)  /* reports read/write readiness */
#define KL_EVENT_CAP_NATIVE_FD  (1u << 1)  /* watches OS-native descriptors */
#define KL_EVENT_CAP_COMPLETION (1u << 2)  /* RESERVED — no backend yet (Phase 8) */

/* Implemented once per event backend TU (event_epoll.c, …). A one-liner returning
 * a compile-time-constant set; `loop` is accepted for a future per-loop-mode
 * backend (io_uring readiness vs completion) but is unused by today's backends. */
unsigned kl_event_caps(const KlEventLoop *loop);

#endif
```

Each backend adds a trivial definition, e.g.:

```c
/* event_epoll.c / event_kqueue.c / event_poll.c / event_wsapoll.c / event_iouring.c */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}
```

io_uring returns the same today (it is used in a readiness-emulation shape). When
Phase 8 gives it a true-completion mode, *that* build/mode adds
`KL_EVENT_CAP_COMPLETION` — the `loop` parameter is why the signature takes it now.

No change to public `KlEventLoop` (no new struct field — caps are a function of the
compiled backend, not per-instance state), so `keel/event.h` is untouched and the
public ABI does not move.

---

## 4. Negotiation — one neutral meeting point

`KlEventCtx` already owns **both** sides (`include/keel/event_ctx.h`): the
`KlEventLoop loop` and `const struct KlSocketProvider *sockets`. It is the F3-safe
place to negotiate — `event_ctx.c` may include both internal headers; neither
seam includes the other.

```c
/* event_ctx.c (internal). Can the ctx's event loop watch the ctx's provider's
 * handles? For the only model shipped (readiness over native fds): the provider
 * must expose native fds AND the loop must be a native-fd readiness poller. */
int kl_event_ctx_sockets_compatible(const KlEventCtx *ctx) {
    const KlSocketProvider *p = ctx->sockets;         /* NULL = built-in POSIX */
    unsigned ev = kl_event_caps(&ctx->loop);
    /* Provider side: native-fd handle (NULL provider defaults to native). */
    int provider_native = kl_socket_provider_has_cap(p, KL_SOCK_CAP_NATIVE_FD);
    /* Loop side: readiness poller of native fds. */
    int loop_can_watch  = (ev & (KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD))
                          == (KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD);
    return provider_native && loop_can_watch;
}
```

**The rule generalizes** (the shape 8/9 fill in): a provider is watchable by a loop
when their models line up — a readiness loop needs a native-fd provider; a
completion loop (Phase 8) will instead require the provider to route I/O through the
loop's submit path; a raw provider (Phase 9) needs a loop that watches its opaque
handles. Phase 7 implements only the readiness+native-fd conjunction; the others
are rejected (they can't be constructed yet anyway).

**Call sites — replace the assumption with the check:**
- `src/server.c:337` — the provider-only `NATIVE_FD` guard becomes
  `if (!kl_event_ctx_sockets_compatible(&s->ev)) { s->last_error = KL_ERR_SOCKET; return -1; }`.
  Behavior is **identical today** (every backend is readiness+native-fd, so the
  check reduces to the old provider-only test) but is now two-sided and truthful.
- Client async start (`kl_client_start_s`/`_pooled`) — same guard after the
  provider is installed on the ctx, so a mismatched pairing fails fast at wire-up
  rather than obscurely at `kl_event_add`.

`KL_ERR_SOCKET` is reused (coarse taxonomy, per the locked Phase-4 error decision);
no new public error code.

---

## 5. Intended public shape (documented, NOT shipped in Phase 7)

Recorded so the eventual freeze is a lift, not a redesign. Exposed only when Phase
8/9 provides a real consumer (and, likely, a BYO-event-loop injection API — the
missing half that makes a public event-capability surface worth committing):

- Promote the `KL_EVENT_CAP_*` bits and a `kl_event_caps()`-style query into
  `keel/event.h` (or a public `keel/event_caps.h`), matching how `KL_SOCK_CAP_*`
  live in the public `keel/socket.h`.
- A public `kl_event_socket_compatible(loop, provider)` predicate so BYO platform
  ports (the lwIP reference pattern: provider **+** event backend shipped as a
  pair) can assert their pairing under a documented contract.
- Until then the contract is internal and enforced by Keel's own wire-up; BYO
  ports pair provider+backend by convention (as the lwIP reference does), now with
  an internal check catching an incoherent server/client construction.

---

## 6. Deliverables

- `src/event_caps.h` — new internal header: `KL_EVENT_CAP_*` + `kl_event_caps()` (§3).
- `src/event_epoll.c`, `event_kqueue.c`, `event_poll.c`, `event_wsapoll.c`,
  `event_iouring.c` — each a one-line `kl_event_caps()` (`READINESS | NATIVE_FD`).
- `src/event_ctx.c` (+ decl in `event_ctx`'s internal reach) —
  `kl_event_ctx_sockets_compatible()` (§4).
- `src/server.c` — replace the `:337` provider-only guard with the two-sided check.
- `src/client.c` — same guard at async start.
- `tests/test_event_caps.c` (new) — assert every built backend advertises
  `READINESS | NATIVE_FD`; `kl_event_ctx_sockets_compatible()` accepts the built-in
  and a native-fd mock provider, and **rejects** a synthetic non-native provider
  (the mock from `test_socket_provider.c`) — proving the guard is a real
  negotiation, not a constant `true`. Runs on POSIX + Windows CI.
- `docs/pal_transformation_design.md` — flip the Phase 7 status line to done.

No `keel/*.h` changes (internal-first); no CI job changes (the new test joins the
normal suites).

---

## 7. Validation

- **Behavioral no-op today:** full `make test` green on every backend
  (`make`, `make BACKEND=poll`, `make BACKEND=iouring`, Windows/WSAPoll); bench flat
  (the negotiation runs once at wire-up, off the I/O path).
- **The guard is real:** `test_event_caps` rejects a non-native provider against a
  readiness loop — the case the old code could not distinguish from "no provider."
- **Cross-platform:** the new test + guard compile clean under MinGW (`-Werror`)
  and run in the Windows core suite.
- **Seam intact:** `src/socket.h` still does not include `event.h`, and
  `src/event_caps.h` does not include `keel/socket.h` (F3 grep-assertable).

---

## 8. Out of scope / stop conditions

- **No completion backend, no submit/complete hooks** — reserved name only (Phase 8).
- **No opaque-handle event watching** — lwIP-raw's `tcp_pcb *` is Phase 9.
- **No public event-capability API** — deferred to the real-consumer freeze (§5).
- **No BYO-event-loop injection** — a separate, larger change; noted as the
  precondition that makes a public event-cap surface worthwhile.
- **Stop and report** if formalizing the contract forces a public
  `keel/event.h` change, or if the two-sided check is not byte-identical on any
  current backend — either means the axis model is wrong and needs rethink before
  code.
