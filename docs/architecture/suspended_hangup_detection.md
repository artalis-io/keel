# Detecting a client hang-up while a server connection is suspended

Status: **design for review, revision 2. Nothing here is implemented.** Revision 2 records the review
decisions and replaces reasoning with measurements from the real CI runners wherever they exist.

## Problem

`kl_async_suspend` removes the client's socket from the event loop. If the client goes away while the
handler is parked, Keel does not notice until it next writes. Hull parks handlers on outbound streams
and fetches; an upstream that keeps sending heartbeats keeps the handler parked indefinitely, and each
parked handler holds one of Hull's 64 in-flight request slots. Measured in Hull: 64 clients that gave
up left 64 sockets in CLOSE_WAIT and locked out outbound HTTP for the whole app.

CLOSE_WAIT alone is not a leak: a server may legitimately finish a response after the client's FIN.
The defect is a suspended connection that **cannot observe** abandonment, so it never completes and
never releases its slot. What is missing is transport-liveness observation for a connection whose
application data is deliberately not being consumed.

## Decisions

| Question | Decision |
|---|---|
| Cancel a suspended HTTP/1.1 connection on a peer FIN? | Yes, by default, with a documented half-close exception |
| Enum or boolean for the server option? | Enum, `CANCEL` = 0 = default |
| Public `KL_EVENT_HANGUP`? | Yes, capability-gated, transport-level meaning only |
| IOCP | No undocumented `AFD_POLL`; a documented mechanism or the sweep fallback |
| WSAPoll | Decide from measurement (done: see the matrix) |
| Sweep fallback | Acceptable with bounded cost and explicit limitations |
| HTTP/2 | Out of scope (see Scope) |
| Version | 3.5.0, if the default-on behavior ships |

Layering: **HTTP decides what peer abandonment means; the event layer reports transport events; the
socket/provider layer preserves transport semantics.** No backend cancels anything, and no backend
knows about HTTP.

## Scope

In scope: HTTP/1.1 connections suspended with `kl_async_suspend`, on every event backend and the lwIP
and UEFI providers.

Out of scope:

- **HTTP/2.** An HTTP/2 handler cannot suspend: only the HTTP/1.1 connection sets
  `KlHttpRequest._server_ctx`, so `kl_http_request_conn()` returns NULL for an HTTP/2 request and
  `kl_async_suspend` refuses it. There is no suspended HTTP/2 stream to cancel.
- **A paused request body** (`kl_http_request_pause_body`, registered with no interest) has the same
  blind spot; a follow-up can reuse this mechanism.
- The client side (`KlHttpClient`), which owns its sockets and reads them.

## What a peer looks like on the wire

From the server, a client's `close()` and its `shutdown(SHUT_WR)` are the **same event**: one FIN.
Nothing distinguishes them. The probe confirms a legitimate half-close on every platform: after the
FIN the server's write succeeds and the client receives it. The half-close exception is therefore a
policy choice, not a detection. A reset (RST) always means the peer is gone.

## Capability matrix (measured)

Probes: `tools/hangup_probe/` on the `diag/hangup-probe` branch (diagnostic only; not built, not
shipped). Each row arms the watch on the server's accepted socket **before** the peer acts, never
reads, counts re-reports over 1000 zero-timeout waits, and reads `SO_ERROR` last. Runners (GitHub,
2026-11-23): Ubuntu 26.04.1, kernel 7.0.0-1020-azure, x86_64; macOS 26.7.2 (Darwin 25.6.0), arm64;
Windows 10.0.26100, x86_64. The Linux results also match a local Linux 5.10 run, the macOS ones a
local macOS run.

| Peer action | epoll `EPOLLRDHUP` + `EPOLLET` | Linux poll `POLLRDHUP` | io_uring `POLL_ADD(POLLRDHUP)` | kqueue `EVFILT_READ` + `EV_CLEAR` | WSAPoll, `events = 0` | macOS poll, no interest | IOCP 1-byte `MSG_PEEK` receive |
|---|---|---|---|---|---|---|---|
| FIN (close, or half-close) | RDHUP | RDHUP | RDHUP | `EV_EOF`, `fflags` 0 | HUP | nothing | 0 bytes |
| FIN behind unread request bytes | RDHUP | RDHUP | RDHUP | `EV_EOF`, `fflags` 0, `data` 27 | HUP | nothing | 1 byte: FIN hidden |
| RST | RDHUP HUP ERR | HUP ERR RDHUP | HUP ERR RDHUP | `EV_EOF`, `fflags` 54 (ECONNRESET) | HUP ERR | nothing | error 64 |
| RST behind unread bytes | RDHUP HUP ERR | HUP ERR RDHUP | HUP ERR RDHUP | `EV_EOF`, `fflags` 54 | HUP ERR | nothing | 1 byte: reset hidden |
| Pipelined bytes, no close | nothing | nothing | nothing (stays armed) | one report, `EV_EOF` 0 | nothing | nothing | 1 byte |
| FIN, then later RST | RDHUP, then RDHUP HUP ERR | RDHUP, then HUP ERR RDHUP | RDHUP (one-shot) | `EV_EOF`, then `fflags` 54 | HUP, then HUP (no ERR) | nothing | 0 bytes |
| Re-reports after the first (spin) | 0 | **1000/1000** | re-arming completes at once | 0 | **1000/1000** | n/a | every re-post completes at once once FIN or data is present |

Not in the table:

- epoll in level-triggered mode re-reports 1000/1000; Keel's epoll backend is already edge-triggered
  (`EPOLLET`), and its kqueue backend already uses `EV_CLEAR`.
- macOS poll with `POLLIN` reports `IN HUP` on a FIN but re-reports pipelined data forever.
- IOCP zero-byte receive: completes with 0 bytes for a FIN **and** for data, so it cannot tell them
  apart. Reset: error 64.
- `MSG_PEEK` (all platforms): 0 on a FIN with nothing queued, 1 byte when anything is queued (the FIN
  is hidden), `ECONNRESET`/10054 after a reset.
- `SO_ERROR`: `ECONNRESET` on Linux and macOS only if no one read it first (reading clears it; after
  FIN then RST, Linux reports `EPIPE`). **On Windows it stays 0 after a reset.** It cannot classify a
  hang-up portably.

Conclusions:

1. A clean close never produces HUP or ERR on the POSIX mechanisms. A policy of "cancel on HUP/ERR
   only" misses every client that closed cleanly: the CLOSE_WAIT case Hull measured.
2. Every mechanism that reports a hang-up also reports a reset distinctly: ERR on epoll, poll,
   io_uring and WSAPoll, a non-zero `fflags` on kqueue, an error completion on IOCP. The backend that
   saw the event is the right place to classify it.
3. epoll, Linux poll, io_uring, kqueue and WSAPoll all see a FIN behind unread bytes without consuming
   them, and none reports pipelined bytes as a hang-up.
4. Linux poll and WSAPoll are level-triggered and spin unless the backend disarms after reporting.
   io_uring's poll is one-shot, and re-arming after a FIN completes at once.
5. macOS/BSD poll cannot watch for a hang-up at all.
6. IOCP's documented receives cannot be a persistent watch: once a FIN or any byte is present, every
   re-post completes at once.

Still reasoned, not measured: lwIP (raw and sockets), UEFI, WSAPoll on a socket already associated with
an IOCP port, and `CancelIoEx` of a pending peek (the probe never left one pending to cancel).

## Design

### Event axis: transport observation only

Two report bits and one interest bit in the public `KlEventMask`:

```c
KL_EVENT_HANGUP = 4,  /* interest: report the peer's write-side shutdown or a transport failure,
                         never readable data.
                         report: the peer shut down its sending direction (FIN), or the connection
                         failed. Says nothing about whether the socket can still be written. */
KL_EVENT_ERROR  = 8   /* report only: the transport reported an error (a reset), as classified by
                         the backend from what it observed. */
```

And one capability bit, which is a **verified guarantee**, not merely support for a platform flag:

```c
#define KL_EVENT_CAP_HANGUP (1u << 3)
/* With HANGUP interest and no READ interest, the backend
 *   - reports a FIN, including one behind unread bytes,
 *   - reports a reset with KL_EVENT_ERROR,
 *   - never reports readable data,
 *   - reports each transition once (no re-report until the next kl_event_mod).
 * Each backend that sets it carries tests for all four. */
```

Reading the report:

| Reported | Meaning |
|---|---|
| `HANGUP` | the peer shut down its sending direction (a FIN); the socket may still be writable |
| `HANGUP \| ERROR` | the connection failed (a reset or another transport error) |
| `ERROR` alone | never reported |

Rules that keep the bits from changing existing behavior:

- `KL_EVENT_ERROR` is **report-only**. It cannot be requested; passing it as interest is ignored.
- `HANGUP` and `ERROR` are reported **only to a registration whose interest includes
  `KL_EVENT_HANGUP`**. A registration with READ or WRITE interest is reported exactly as today (a
  backend keeps folding HUP/ERR into READ for it), so every existing callback sees the same masks.
  Every in-tree consumer tests individual READ/WRITE bits; none compares a whole mask.
- The event layer has no error callback of its own, so no second terminal notification is created.
  The one terminal path is the server's `kl_async_cancel`, which is already idempotent
  (exactly one `on_cancel`).
- The event layer never cancels and never interprets a FIN. A runtime-injected `KlEventOps` that
  predates these bits does not advertise the capability, so the server never asks it for HANGUP and
  it keeps working unchanged.

Per-backend mapping (measured unless marked):

| Backend | HANGUP interest | Reported as HANGUP | Adds ERROR | Disarm | CAP_HANGUP |
|---|---|---|---|---|---|
| epoll | `EPOLLRDHUP \| EPOLLET` | RDHUP, HUP or ERR | ERR | edge-triggered already | yes |
| kqueue | `EVFILT_READ` + `EV_CLEAR`; a report without `EV_EOF` is swallowed (one wake per data arrival, no spin) | `EV_EOF` | `fflags` != 0 | `EV_CLEAR` already | yes |
| poll (Linux) | `POLLRDHUP` | RDHUP, HUP or ERR | ERR | entry's events cleared after a report until the next `mod` | yes |
| poll (macOS/BSD, Cosmopolitan off Linux) | none | n/a | n/a | n/a | no: sweep |
| WSAPoll | `events = 0` | HUP | ERR | entry skipped after a report until the next `mod` | yes |
| io_uring | one-shot `POLL_ADD(POLLRDHUP)`, counted in `comp_ops`, removed on resume or close | the completion | ERR | one-shot; not re-armed after a report | yes |
| pollcomp | as the host poll | as the host poll | as host | as host | Linux yes, else no |
| IOCP | none: no posted watch | n/a | n/a | n/a | no: sweep |
| lwIP raw (reasoned) | provider reports watched connections whose `closed` flag is set | `closed` | `dead` | report once | yes, after tests |
| UEFI (reasoned) | none | n/a | n/a | n/a | no: sweep |

### Socket axis: the fallback probe (decision pending)

The sweep needs a synchronous point query per socket that never blocks and never consumes data. It
is used **only** for connections whose loop lacks `KL_EVENT_CAP_HANGUP`; a connection is watched by the
event backend or probed by the sweep, never both. So the query does not duplicate the event
backend's responsibility: it covers exactly the backends that cannot carry that responsibility.

Two options.

**Option A: existing `recv_peek` only, no new interface.** The sweep calls `recv_peek(fd, buf, 1)`:

| `recv_peek` result | Meaning to the sweep |
|---|---|
| 0 | FIN observed, nothing queued in front of it |
| error, connection reset class | transport failure |
| 1 byte | **indeterminate**: data is queued, and any FIN behind it is hidden |
| would-block | no terminal condition observed |
| other error | indeterminate |

This expresses FIN, failure and indeterminate correctly. What it cannot express is a FIN or reset
**behind queued bytes**, and on Windows it gives up the measured precision of WSAPoll. Note the
second-to-last row: "no terminal condition observed" from a peek is accurate (nothing is queued and no
FIN has arrived), but it is not a statement that the peer is connected.

**Option B: an optional provider operation with an explicit result.**

```c
typedef enum {
    KL_PEER_UNSUPPORTED   = 0, /* the provider cannot query this socket */
    KL_PEER_INDETERMINATE = 1, /* queried, but the answer is hidden (e.g. bytes queued ahead of a FIN) */
    KL_PEER_NO_TERMINAL   = 2, /* queried; no FIN and no failure observed. NOT a liveness guarantee */
    KL_PEER_WRITE_SHUTDOWN= 3, /* the peer shut down its sending direction (FIN) */
    KL_PEER_FAILED        = 4  /* transport failure (reset) */
} KlPeerState;

/* Optional. Non-blocking; never consumes, never changes socket state. NULL: the sweep uses
 * recv_peek per Option A. */
KlPeerState (*peer_state)(void *ctx, KlSocketHandle fd);
```

Zero is UNSUPPORTED, so a zeroed or partly initialized result never reads as "fine". No value means
"connected": the strongest non-terminal answer is NO_TERMINAL.

| Provider | Implementation | FIN behind unread bytes |
|---|---|---|
| POSIX on Linux | `poll(POLLRDHUP, 0)`: RDHUP = WRITE_SHUTDOWN, HUP/ERR = FAILED, nothing = NO_TERMINAL | **seen** (measured) |
| POSIX elsewhere | NULL (Option A's `recv_peek`) | not seen |
| Winsock | `WSAPoll(events = 0, 0)`: HUP = WRITE_SHUTDOWN, HUP ERR = FAILED, nothing = NO_TERMINAL | **seen** on a plain socket (measured); **unverified** on an IOCP-associated socket |
| lwIP raw (reasoned) | `closed` / `dead` flags | seen |
| UEFI (reasoned) | `EFI_TCP4.GetModeData`: CloseWait or later = WRITE_SHUTDOWN, a reset state = FAILED | seen |

What B buys over A, concretely: on Windows (IOCP and, as a fallback, WSAPoll) and on UEFI, a FIN
behind pipelined bytes becomes observable within one sweep interval instead of only at the op's
deadline. On Linux it matters only where the loop lacks CAP_HANGUP (an injected backend), since every
built-in Linux backend has it. On macOS/BSD poll it buys nothing.

Recommendation: start with **Option A** (no new interface) and keep B as a measured follow-up,
**unless** IOCP is considered important enough to need the precise Windows probe from day one. The
deciding measurement is WSAPoll on an IOCP-associated socket; if it does not work, B's main benefit
on Windows disappears.

### HTTP policy: what abandonment means

New option, appended to `KlHttpServerConfig`, zero meaning the decided default:

```c
typedef enum {
    KL_HTTP_SUSPEND_HANGUP_CANCEL     = 0, /* default: a FIN or a reset cancels the suspended op */
    KL_HTTP_SUSPEND_HANGUP_RESET_ONLY = 1, /* half-close kept: only a reset cancels */
    KL_HTTP_SUSPEND_HANGUP_OFF        = 2  /* today's behavior: nothing is watched */
} KlHttpSuspendHangup;
```

1. **Suspend.** If the policy is not OFF and the loop has `KL_EVENT_CAP_HANGUP`:
   `kl_event_mod(fd, KL_EVENT_HANGUP)` instead of `kl_event_del`. Otherwise remove the fd as today;
   the connection is covered by the sweep (unless OFF).
2. **A HANGUP report on a SUSPENDED connection.**
   - With ERROR, or under CANCEL: `kl_async_cancel` (the existing exactly-one-terminal guard: one
     `on_cancel`, the connection closed, nothing written).
   - FIN under RESET_ONLY: keep the op and record `peer_fin`, so the response, once resumed, is
     written and the connection closed rather than kept alive. A later reset is still observable on
     the edge-triggered backends (measured: epoll and kqueue report it again). On the level-triggered
     and one-shot backends the report has disarmed the watch, so the connection moves to the sweep for
     the reset. Exception, measured: on Windows a reset after a FIN adds no ERR to WSAPoll and the
     peek still shows EOF, so a reset after a half-close is not observable there; the op's deadline
     bounds it.
3. **Sweep fallback.** One timer per server at a fixed interval (default 1 s), scheduled only while at
   least one suspended connection needs it, never per loop iteration. Each tick walks the server's
   list of pending ops (`s->async_ops`, which a retired op has already left), skips connections the
   loop is watching, and runs the fallback probe (Option A or B, above) once per connection. Never
   blocking, never consuming. Nothing is probed after cancel, resume or `kl_http_server_free` (the
   timer is cancelled with the last such connection). An INDETERMINATE or UNSUPPORTED answer changes
   nothing; the connection is probed again next tick.
   **Limitation, stated as such:** the one-interval figure is the maximum delay only where the probe
   can see the condition. A FIN or reset behind unread bytes is invisible to the peek, so with
   Option A, and on every provider marked "not seen", such a connection is bounded only by the op's
   own deadline, however short the interval.
4. **Resume** (`kl_async_complete`): re-arm from a registered fd (the existing `async_rearm` already
   falls back from add to modify). On io_uring, remove the posted poll before
   `kl_http_comp_resume`, with the existing `comp_ops` / `comp_closing` accounting so the slot is
   released only after its last completion.
5. **Timeouts:** unchanged. A suspended connection stays exempt from the idle sweep; only a hang-up,
   an error or the op's own deadline ends it.
6. **Ordering:** the cancel runs from the loop's I/O processing (or the sweep timer), outside any
   dispatch frame for that connection. The `on_cancel` contract in `async.h` already covers this.

### TLS

Detection is at the transport. A peer that sends `close_notify` and then FIN is detected by the FIN.
`close_notify` with the TCP connection left open is not detected: it is application data in a TLS
record, and reading it would consume the stream. Such a connection ends at the op's deadline.

### What does not change

- Pipelined requests and body bytes are never read while suspended, and never wake the loop
  repeatedly.
- `kl_http_server_free` with suspended ops: unchanged (it cancels every pending op).
- Source compatibility: appended enum members, an appended config field whose zero value is the
  decided default, report and capability bits, and (Option B only) an optional provider operation. Behavior change: by default a
  suspended op whose client closes is cancelled instead of running to completion. Minor release with
  an upgrade note.

## Tests

Event backends (one set per CAP_HANGUP backend, proving the guarantee rather than the flag):

- FIN, and FIN behind unread bytes: one HANGUP, no ERROR; the bytes are still readable afterwards.
- Reset, and reset behind unread bytes: HANGUP with ERROR.
- Pipelined bytes only: no report; loop wake count stays flat over a wait period.
- No re-report until the next `kl_event_mod`.

Fallback probe (Option A, or `peer_state` under Option B): the same scenarios, expecting INDETERMINATE where a FIN is hidden and never a terminal answer for pipelined bytes alone.

Server (mapped to Hull's list):

- Client closes while suspended: `on_cancel` once, socket closed, nothing written; every backend, the
  fallback ones through the sweep.
- Client half-closes while suspended: cancelled under CANCEL; under RESET_ONLY the op survives, the
  response is delivered and the connection then closes.
- Reset while suspended: cancelled under CANCEL and RESET_ONLY; untouched under OFF.
- Pipelined request while suspended: no spin, not consumed, served after resume.
- TLS client closes while suspended (mbedTLS e2e).
- `kl_async_complete` racing a hang-up in the same tick: exactly one terminal.
- `kl_http_server_free` with suspended ops after hang-ups; no probe after free.
- Many suspended connections (for example 1000) on a fallback backend: sweep cost per tick measured
  and reported (time per tick, probes per tick). The test asserts structural bounds that do not depend
  on runner speed (one probe per connection per tick, no probe for a retired op, no tick with no
  fallback connection), not a universal CPU threshold.
- OFF: today's behavior, byte for byte.
- An injected `KlEventOps` without CAP_HANGUP: sweep fallback.

## Open questions

1. **`KL_EVENT_ERROR`.** A report-only bit, so the backend that observed a reset classifies it. The
   alternative is `SO_ERROR`, which measurement shows is not portable (0 on Windows, cleared on read).
   Recommendation: add it.
2. **Fallback probe: Option A (`recv_peek` only) or Option B (`peer_state`).** Recommendation: A
   first, B only if the IOCP measurement below works and IOCP precision is wanted now.
3. **IOCP.** Sweep only (no posted watch). Measure WSAPoll on an IOCP-associated socket before
   relying on it (unverified). If it fails, IOCP keeps the peek with its documented blind spot.
4. **lwIP and UEFI** stay reasoned until their integration tests exercise the scenarios.
5. **Sweep interval** fixed at 1 s, or a server option.
