# Authoring a non-HTTP protocol on Keel (Phase 5)

Keel's networking core is not HTTP-only. A protocol author can build an arbitrary
TCP (or datagram) server on the **same public primitives** the HTTP server is
built on, with no HTTP machinery (`KlServer`, router, request/response) and no
internal headers. This document is the authoritative map of that surface; the
working proof is `tests/test_stream_transport.c` — a length-prefixed framed-echo
server built entirely from what is described here.

## The surface (all public)

| Concern | API | Header |
|---|---|---|
| Event loop + allocator | `KlEventCtx` (`kl_event_ctx_init` / `_free` / `kl_event_ctx_run`) | `event_ctx.h` |
| Readiness callbacks | `kl_watcher_add` / `kl_watcher_mod` / `kl_watcher_del`, `KlWatcherFn`, `KL_EVENT_READ`/`WRITE` | `event_ctx.h`, `event.h` |
| Sockets (bring-your-own capable) | `KlSocketProvider` + `KlSocketOps` (socket/bind/listen/accept/connect/recv/send/close/set_nonblocking/set_reuseaddr/get_local_addr/…), `kl_socket_provider_posix()`, `kl_sock_native_fd()` | `socket.h`, `handle.h`, `net.h` |
| Outbound backpressure | `KlDrain` (`kl_drain_init`/`_write`/`_flush`/`_pending`/`_data`/`_consume`/`_free`) | `drain.h` |
| One-shot timers | `kl_timer_add`, `KlTimerFn` | `timer.h` |
| Connection suspension | `KlAsyncOp` (`kl_async_suspend`/`_complete`/`_cancel`) | `async.h` |
| Datagram dispatch (precedent) | `KlUdpServer` | `udp_server.h` |

The socket ops are reached through the provider on the context —
`ctx->sockets` (or `kl_socket_provider_posix()` when it is NULL) —
`sp->ops->accept(sp->context, lfd, ...)`, etc. This is the same seam that lets you
swap in a custom stack (lwIP, a test transport); a plain TCP protocol just uses
the built-in POSIX/Winsock provider.

## The pattern (readiness model)

```text
socket → set_reuseaddr → bind → listen → set_nonblocking      (via sp->ops)
   └─ kl_watcher_add(ctx, listener, READ, on_accept)

on_accept:  loop sp->ops->accept until would-block; per conn:
   set_nonblocking; kl_drain_init(out, write_fn=send-wrapper);
   kl_watcher_add(ctx, conn, READ, on_ready, conn_state)

on_ready(READ):  sp->ops->recv into an accumulation buffer; parse whole frames;
   echo/handle each via kl_drain_write; then re-arm interest =
   READ | (kl_drain_pending ? WRITE : 0)   via kl_watcher_mod

on_ready(WRITE): kl_drain_flush; re-arm (drop WRITE once the drain empties)

peer close (recv == 0) / error:  kl_watcher_del; sp->ops->close; free state
```

Backpressure is entirely the drain's: `kl_drain_write` sends inline and buffers
whatever the socket won't take (write_fn returns `0` on would-block); the author
arms `WRITE` interest while `kl_drain_pending` and drops it when the buffer
empties. Bound the buffer with `kl_drain_set_max_size` for untrusted peers.

## Scope

This is the **readiness** transport surface (watcher + provider ops + drain) — the
default `KlEventCtx` on epoll/kqueue/poll/WSAPoll. It is what a protocol author
targets to build a server today. The completion axis drives Keel's *own*
protocols through the internal completion driver; a public completion-model
authoring surface is out of scope for Phase 5 (the readiness surface above is the
supported protocol-author API).

## What Phase 5 did *not* add

No new server framework. The exercise confirmed the existing public primitives
are sufficient and ergonomic enough to build a real framed protocol — so the only
deliverables are this document and the `stream_transport` test. If a future
protocol needs a convenience (e.g. a `kl_tcp_listen` one-liner), it can be added
without disturbing this surface.

## Reference

`tests/test_stream_transport.c` — a 4-byte-length-prefixed framed echo server on
the surface above, exercised by a raw-socket client with tiny, empty, medium
(8 KiB), and large (128 KiB, exceeds the socket buffer → drain buffering +
writable-driven flush) frames plus back-to-back pipelining. ASan+UBSan clean
(`make debug-test`) and leak-clean under LSan in the Linux container.
