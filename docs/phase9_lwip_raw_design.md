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

## Architectural shape (finding, 2026-08-03): it's a completion BACKEND, not a runtime drop-in

The shipped **readiness** lwIP provider is a pure runtime drop-in: `kl_event_provider_lwip()` +
`kl_socket_provider_lwip()` injected via `KlEventCtx`/`KlConfig` into a **stock** `libkeel.a`,
because the readiness path (`kl_event_wait` → dispatch → `conn_read/write`) is already in every
build.

The **completion** path is different: `completion_driver.c` and the `kl_comp_*` primitives
(`kl_comp_run`/`kl_comp_drain`/`kl_comp_post_recv/send/accept/sendfile`, `io_engine.h`) are
**link-time backend selections** — the Makefile `BACKEND` block compiles them only for
`iouring`/`iocp`/`pollcomp`; a readiness build links the `io_engine.c` **stubs** and contains no
completion driver. So a runtime-injected lwIP-raw `KlEventProvider` alone would find `kl_comp_run`
resolved to the do-nothing stub. Therefore Phase 9 is a **completion backend build**, analogous to
`BACKEND=iouring` (which needs `liburing`): it links `completion_driver.c` + `event_lwip_raw.c`
(which provides the `kl_comp_*` primitives, the `KlEventOps`, `kl_event_caps`=COMPLETION,
`kl_event_native_provider`, **and** an overlapped-capable socket provider whose `KlSocketHandle`
is a `tcp_pcb *`). Because lwIP is BYO, the build requires `LWIP_DIR` (like `iouring` needs
liburing headers). Concretely: a `BACKEND=lwipraw` (or an `integrations/lwip` target) that builds
the core sources with `EVENT_SRC=event_lwip_raw.c` + `COMPLETION_SRC=completion_driver.c` against
BYO lwIP, then runs the loopback test.

*Alternative (not chosen now):* first refactor the completion axis to be **runtime-injectable**
(promote `kl_comp_*` to vtable methods on the completion `KlEventProvider`, put `completion_driver.c`
in every build) — then lwIP-raw would be a runtime drop-in like the readiness one, matching the
socket/event/datagram axes that already went runtime. That is a larger, separate PAL refactor; the
BACKEND approach ships Phase 9 without it and is consistent with today's completion backends. Revisit
if a second out-of-tree completion backend ever wants runtime injection.

`completion_driver.c` itself is model-blind (consumes `KlCompletionEvent` + calls the `kl_comp_*`
primitives), so it is **reused unchanged** — Phase 9 supplies only the lwIP-raw backend TU.

## Staged implementation plan (each stage independently tested)

The spike replaces its `for`-loop with KEEL's event loop; the real work is a `KlEventProvider`:

- **P9-1 — provider skeleton + loop drive. DONE.** `integrations/lwip/event_lwip_raw.c` (+ the
  lwIP-only `lwip_raw_glue.c` seam TU, `keel_lwip_raw.h`) exposes `kl_event_provider_lwip_raw()` +
  `kl_socket_provider_lwip_raw()` (completion caps / `KL_SOCK_CAP_OVERLAPPED`), and implements the
  `completion.h` backend primitives — `kl_comp_drain` = one lwIP tick (`sys_check_timeouts()` +
  `netif_poll()`) returning 0 events; `kl_comp_post_*`/`kl_comp_cancel` are P9-2+ stubs that link.
  Built via a `BACKEND=lwipraw` root-Makefile case (mirrors `iouring`: `EVENT_SRC=event_lwip_raw.c`
  + `COMPLETION_SRC=completion_driver.c`, gated on `LWIP_DIR`); `completion_driver.c` reused
  unchanged. **Header-clash note:** lwIP's `def.h` (`htons`/`ntohs` macros) + `arch.h` (`ssize_t`)
  clash with the host socket headers the KEEL socket seam pulls into `event_lwip_raw.c`, so all lwIP
  contact is confined to `lwip_raw_glue.c` (lwIP headers, no KEEL socket headers), meeting the
  backend across `lwip_raw_glue.h` with neutral/opaque types — **P9-2+ must keep this split** (the
  `tcp_accept`/`tcp_recv` callbacks + pcb handling live in the glue TU). Test:
  `make -C integrations/lwip loopback-raw` builds the completion libkeel over NO_SYS=1 lwIP and
  ticks an lwIP-raw `KlEventCtx` (no server) → `P9-1 PASS`; CI-gated in the `lwip` job. Default
  epoll + `BACKEND=iouring` builds verified unaffected.
- **P9-2 — listen/accept + recv completion + minimal send. DONE.** `tcp_accept`/`tcp_recv`/
  `tcp_sent` (in `lwip_raw_glue.c`) surface `KL_COMP_ACCEPT`/`KL_COMP_READ`/`KL_COMP_WRITE` to the
  **unchanged** `completion_driver.c`, which drives the roundtrip verbatim (no driver contract gap
  found). A raw-backed `KlServer` answers `GET /` → 200 `{"ok":true}` over loopback to a raw-API
  test client. Test: `make -C integrations/lwip loopback-raw` → `P9-2 PASS` (ASan-clean; CI-gated).
  Key mechanics: **pcb↔KlConn** via the pcb's `tcp_arg` (+ a glue per-pcb slot) so recv/sent/err
  callbacks tag their owner; `KlSocketHandle` carries the `tcp_pcb *`. **recv ordering / fast-client
  race:** a client can deliver its request in the *same* tick as the accept, before the driver posts
  a recv — so `tcp_recv` copies the (chained) pbuf into a per-pcb staging buffer and calls
  `tcp_recved` immediately; `kl_comp_drain` surfaces `KL_COMP_READ` (staging→`c->read_buf`) only for
  *armed* conns (those the driver has `kl_comp_post_recv`'d), picking up the raced request on the
  next drain. **NO_SYS=1 single-thread:** `kl_server_run` owns the lwIP tick on one thread; the test
  client's lwIP calls are marshalled onto it via KEEL timers (no data race). `tcp_listen` relocates
  the pcb — `kl_comp_prime_accepts` adopts the relocated LISTEN handle so close targets a live pcb.
  Seam split kept ironclad (no `struct sockaddr` or lwIP type crosses `lwip_raw_glue.h`).
- **P9-3 — send + backpressure + file send. DONE.** A per-pcb owned copy of the full response +
  `written`/`acked` offsets: `lwr_send_pump` loops `tcp_write(COPY)` in chunks bounded by
  `tcp_sndbuf(pcb)` and `KL_LWR_TCP_WRITE_MAX` (0xffff), then `tcp_output`; `tcp_write`→`ERR_MEM`
  (queue full) stops the round (backpressure); `tcp_sent(pcb,len)` advances `acked` and re-pumps the
  tail, and only at `acked==total` is the **single** terminal `KL_COMP_WRITE` surfaced (the driver
  never sees a partial write, per the completion.h contract) and the buffer released.
  `kl_comp_post_sendfile` (was a stub) pread's the file after the head into the same buffer and
  reuses the pump (approach (a)); the glue only reads `file_fd` — the response layer owns closing it
  (matches `event_pollcomp.c`, no double-close). Cap `KL_LWR_POST_SEND_MAX` = 16 MiB. Test: a 64 KB
  buffered response (spans many `tcp_sent` rounds + real `ERR_MEM` stalls; length+checksum+first/last
  byte verified) and a file response → `P9-3 PASS (buffered)` + `(file)`. **ASan+UBSan+LSan-clean**
  (fixed a real 64-bit alignment issue by setting `MEM_ALIGNMENT=8` in `lwipopts_raw.h`, not by
  suppressing). `completion_driver.c` + `src/` + root Makefile unchanged. *P9-4 hooks:* the per-conn
  send buffer is freed on close/abort (`lwr_conn_free`→`lwr_send_reset`) and by `kl_comp_cancel`
  (`kl_lwr_send_release`); the `tcp_err`/RST-mid-send path should also free-by-owner.
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
