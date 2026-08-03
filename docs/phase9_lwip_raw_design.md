# Phase 9 — lwIP raw-API completion event provider — Design + go decision

**Status (2026-08-03): GO, gated on testability — testability PROVEN by a spike.**

This is the last event-axis frontier from `docs/pal_review.md` (the "one true extensibility
boundary") and `docs/lwip_platform_design.md`: a completion-model event provider driven by lwIP's
**raw `tcp_*` callback API**, as an alternative to the shipped **readiness** lwIP provider
(`event_lwip.c`, which polls the sockets layer via `lwip_poll`).

## Why a second lwIP provider at all

The shipped lwIP integration is `NO_SYS=0`: lwIP runs its own **tcpip thread**, exposes the BSD
sockets/netconn API, and KEEL rides it via `socket_lwip.c` + the `lwip_poll` readiness backend.
That works, but it carries the sockets/netconn layer + a thread + mailboxes — weight an embedded
target may not want, and it is a *readiness* shim over a stack whose native shape is *completion*
(callbacks). The raw API is lwIP's native, lightest interface: `tcp_new`/`tcp_bind`/`tcp_listen`/
`tcp_accept`/`tcp_recv`/`tcp_sent`/`tcp_poll` callbacks, no sockets, no thread.

## The crux: NO_SYS=1

lwIP's raw `tcp_*` functions and callbacks are **not thread-safe** and must run either on the
tcpip thread (NO_SYS=0) or in a single bare mainloop (`NO_SYS=1`). A KEEL-driven provider wants the
latter: **KEEL's `KlEventCtx` run loop IS the lwIP mainloop.** Each tick calls
`sys_check_timeouts()` (drives retransmit/TIME-WAIT/etc.) and `netif_poll()` (drains queued
loopback/TX pbufs); the raw `tcp_*` callbacks fire inline on that one thread, so no locking is
needed and the callbacks map cleanly onto the completion driver (`kl_comp_post_recv/send` become
`tcp_write`/`tcp_recved`; `tcp_recv`/`tcp_sent` callbacks become completion events). This is a
different lwIP build config from the shipped one, so Phase 9 ships a **separate `lwipopts_raw.h`**
(`NO_SYS=1`, no sockets/netconn) alongside the existing `lwipopts.h` (`NO_SYS=0`).

## Testability gate — PROVEN (the reason this doc says GO)

The whole effort was explicitly gated on "only if we can test it." A standalone spike settles it:

- **`integrations/lwip/raw_loopback_spike.c` + `lwipopts_raw.h` + `make raw-spike`** build a
  `NO_SYS=1` lwIP (29 files: `src/core/*.c` + `src/core/ipv4/*.c` — **no** `src/api`, **no**
  `sys_arch.c`, **no** lwip-contrib) and run a raw-API TCP echo server + client on `127.0.0.1`
  over the auto-created loopback netif, **entirely in-process, no tap device, no root**. The
  roundtrip completes on the first mainloop tick:
  ```
  SPIKE PASS: raw+loopback+NO_SYS roundtrip after 1 ticks — sent="hello-lwip-raw-noSys" echo="hello-lwip-raw-noSys"
  ```
- It runs in the **Apple `container` Linux VM** and in **CI** (a step in the `lwip` job:
  `make -C integrations/lwip raw-spike LWIP_DIR="$HOME/lwip"`), reusing the BYO-lwIP clone the job
  already fetches. Cost: ~1s clone + ~1s build+run — negligible.

So a raw/completion lwIP provider is testable exactly like the existing socket+poll loopback test
(in-process over the loopback netif), which is what "can we test it" required. **Go.**

### Spike findings that shape the implementation

1. **No lwip-contrib needed.** contrib's unix port is the *threaded* (`NO_SYS=0`) `sys_arch`; for
   `NO_SYS=1` it is harmful (its `cc.h` binds `LWIP_RAND()` to `lwip_port_rand()`, dragging in
   `sys_arch.c`). The spike supplies a self-contained ~15-line `arch/cc.h` + trivial
   `arch/sys_arch.h` (generated into a scratch `raw_build/`), plus `sys_now()` via
   `clock_gettime(CLOCK_MONOTONIC)`.
2. **lwipopts override:** lwIP's `opt.h` does an unconditional `#include "lwipopts.h"`; the raw
   build compiles in `raw_build/` whose one-line `lwipopts.h` forwards to `../lwipopts_raw.h`, put
   first on the include path — so the raw build gets `NO_SYS=1` while the existing `loopback`
   target (which uses the `NO_SYS=0` `lwipopts.h` in `.`) is untouched.
3. **Loopback in NO_SYS=1:** `LWIP_HAVE_LOOPIF=1` auto-creates the loop netif at `lwip_init()`;
   `LWIP_NETIF_LOOPBACK_MULTITHREADING` **must be 0**; the mainloop drains it with
   **`netif_poll(netif)`** (per-netif; `netif_poll_all` is a contrib-only symbol). Locate the
   loopif by scanning `NETIF_FOREACH` for the `127.0.0.1` address, then `netif_set_up`/
   `set_link_up`/`set_default`.

## Staged implementation plan (each stage independently tested)

The spike replaces its `for`-loop with KEEL's event loop; the real work is a `KlEventProvider`:

- **P9-1 — provider skeleton + loop drive.** `event_lwip_raw.c` exposing `kl_event_provider_lwip_raw()`
  (completion caps). `kl_event_wait`-equivalent tick = `sys_check_timeouts()` + `netif_poll()` +
  a bounded sleep; wire `KlEventCtx.event_provider`. Test: an lwIP-raw KlEventCtx ticks without a
  server.
- **P9-2 — listen/accept + recv completion.** Map `tcp_accept`/`tcp_recv` → `KL_COMP_ACCEPT`/
  `KL_COMP_RECV` into `completion_driver.c`; a raw-backed `KlServer` answers one request over
  loopback (the spike, but through the real server path). Test: `GET /` → 200 in-process.
- **P9-3 — send + backpressure.** `kl_comp_post_send` → `tcp_write`+`tcp_output`; `tcp_sent`
  callback → send completion + drain; honour `tcp_sndbuf` for backpressure. Test: a response
  larger than one segment; partial-send resume.
- **P9-4 — close/cancel/lifetime.** `tcp_err`/`tcp_close`, close-with-outstanding, idle timeout via
  `sys_check_timeouts`. Test under ASan/UBSan (pcb + pbuf ownership; no leak/UAF).
- **P9-5 — KlEventLoop.fd widening (axis-audit F1).** A raw provider has no pollable fd; revisit
  the public `KlEventLoop.fd` (currently "epoll_fd/kqueue_fd, -1 for io_uring") toward a portable
  handle if the loop object must expose one. Assess; only widen if forced.

Each stage rides `make -C integrations/lwip <target>` in the `lwip` CI job, in-process over
loopback — no new test infrastructure.

## Non-goals

- Not vendoring lwIP (BYO, as today).
- Not replacing the readiness lwIP provider — the raw provider is an *alternative* backend; both
  ship, selected by `KlEventCtx.event_provider`.
- Not a real-hardware netif (tap/ethernet) in CI — loopback is sufficient to test the provider
  semantics; hardware ports are a deployment concern.
