# Windows readiness backend: write progress after a peer half-close

Status: **decided: a select()-based backend** (option B of section 4, with the decision recorded
in section 8), implemented in `src/event_winselect.c`. Evidence is from controlled probes on a real
Windows host (Windows 10.0.26300 AMD64, MSYS2 UCRT64 gcc 16.2), first against a WSAPoll build of PR
#514's head `37943db`. The probe sources are kept outside the tree (a local diagnostic branch).

## 1. The defect

A suspended streamed response on a readiness loop is watched for writability while its output waits
(PR #514). On the WSAPoll backend, a client that half-closes (`shutdown(SD_SEND)`, still reading)
stalls that stream under `RESET_ONLY` and `OFF`. Measured with a producer that pauses at 256 KiB
buffered and resumes only from `on_drain` (`probe_keel_halfclose.c`):

| policy | client | received | ended | on_drain | on_cancel |
|---|---|---|---|---|---|
| CANCEL | reads normally | 4 MiB | yes | 16 | 0 |
| CANCEL | half-closes | 69,711 B | no | 0 | 1 (a FIN cancels: the policy) |
| RESET_ONLY | half-closes | 69,711 B | no | 0 | 0 |
| OFF | half-closes | 69,711 B | no | 0 | 0 |

The same program on macOS kqueue and BSD poll delivers all 4 MiB in every row. PR #514's independent
re-review reached the same conclusion from source: on epoll, kqueue, Linux poll and BSD poll a clean
half-close never reaches the write watch; on WSAPoll it does.

## 2. Root cause: what WSAPoll can and cannot express

`src/event_wsapoll.c` blocks in one `WSAPoll(fds, n, timeout)` (`:207`) and maps, for every
registration that is not a hang-up registration, `POLLHUP` to `KL_EVENT_READ` (`:228-229`).

Winsock transport probe (`probe_win_write.c`): the server's send path is full, then the client
half-closes; phase A before the client reads, phase B after.

| mechanism | A: report | ready in 100 zero-timeout checks | B: report |
|---|---|---|---|
| WSAPoll(POLLWRNORM) | HUP | 100/100 | WRNORM HUP |
| WSAPoll(events = 0) | HUP | 100/100 | HUP |
| select(write, except) | none | 0/100 | write |
| WSAEventSelect(FD_WRITE, FD_CLOSE) | FD_CLOSE once | 0/100 | FD_WRITE once |

After a peer FIN, `WSAPoll` reports `POLLHUP` on every call **whatever interest was requested**, so a
`WSAPoll` wait returns at once and cannot block until writability. Its `POLLWRNORM` bit stays
accurate. `select()` and `WSAEventSelect` keep write readiness independent of the half-close. This is
a WSAPoll capability limitation, not a TCP, Winsock or HTTP one.

## 3. WSAEventSelect semantics (measured, `probe_win_evsel.c`)

| question | result |
|---|---|
| Hang-up-only interest (`FD_CLOSE`, no `FD_READ`) | FIN, half-close: `FD_CLOSE(0)`; reset: `FD_CLOSE(10053)`; **behind unread data too**; data alone never signals |
| Repeats | edge: each condition is recorded once until it changes |
| Re-issuing `WSAEventSelect` on an already-true condition | **not** recorded again (data queued, writable, FIN seen) |
| `FD_WRITE` after a half-close with the send path full | not signalled while full; `FD_WRITE` once the peer reads; no repeat |
| `FD_READ` after a partial `recv` | re-enabled (recorded again) |
| accept from an associated listener | the accepted socket **inherits** the association (cannot be set blocking: `WSAEINVAL`) |
| `WSAPoll(0)` / `select(0)` on an associated socket | both work, and do not consume the recorded event |
| Sharing event objects | 2000 sockets over 63 events: wake 4 us, 31 `WSAEnumNetworkEvents` calls to find the socket |
| Cross-thread `SetEvent` | wakes the wait (101 ms for a 100 ms signal) |
| A 1 MiB nonblocking `send` with `SO_SNDBUF` 8 KiB | took all of it (no partial send) |

`WSAWaitForMultipleEvents` waits on at most 64 handles, but many sockets may share one event object;
the cost is finding which socket of a signalled group changed.

## 4. Options

*Superseded by section 8: this table is the record of the investigation as first written. Option D
failed a proof obligation (section 5a), and option B's `FD_SETSIZE` concern is answered by sets sized
at run time.*

| | How it blocks | Hang-up-only (3.5.0) | Half-close write progress | Scale | Complexity |
|---|---|---|---|---|---|
| **A. WSAPoll + targeted WSAEventSelect fallback** (only for write-after-FIN sockets) | needs two waits at once: WSAPoll cannot wait on an event object, so it takes a helper thread or alternating timeouts | kept | yes | as today | **high**; rejected: two notification lifecycles, migration between them, and either a thread or polling |
| **B. select() backend** | one `select()` | **lost**: readability means data or FIN, so a hang-up-only watch either spins on unread data or misses a FIN behind it | yes | `FD_SETSIZE` (compile-time; 64 by default), O(n) copies per call | medium; regresses `KL_EVENT_CAP_HANGUP` |
| **C. Pure WSAEventSelect backend** | events only | kept (`FD_CLOSE`) | yes | groups of sockets per event, 63 groups | medium-high: every notification is edge-triggered and re-issuing does not re-record, so level semantics (a read left unread, a watcher re-armed) must be rebuilt per socket |
| **D. WSAPoll classifies, WSAEventSelect blocks** (recommended) | **one** blocking wait on up to 63 event objects; `WSAPoll(…, 0)` never blocks | kept exactly (same `WSAPoll` classification as today) | yes | as today (one O(n) `WSAPoll(0)` per wait) plus O(n/63) enumerations per signalled group | **medium, backend-local** |
| E. IOCP for this | completion axis | n/a | yes | n/a | rejected: mixes readiness and completion axes |
| F. Document the stall | n/a | kept | **no** | n/a | rejected: violates the streaming progress contract |

## 5. Option D as first designed (rejected, see 5a and 8)

Keep `WSAPoll` for what it does correctly (a level classification, including hang-up behind unread
data, measured in 3.5.0) and stop using it as the blocking primitive. Block only on event objects that
`WSAEventSelect` sets when anything about a registered socket changes.

### Wait (one blocking mechanism, no spinning)

```
wait(timeout):
  loop:
    WSAResetEvent(every group)            # reset first: a change after this re-signals
    n = WSAPoll(fds, count, 0)            # level classification, never blocks
    deliver = classify(revents, interest) for each registration
    if any deliverable: return them       # level: something still true is returned at once
    if timeout elapsed: return 0
    g = WSAWaitForMultipleEvents(groups, ..., remaining)   # the only block
    if signalled: WSAEnumNetworkEvents(s, NULL, ...) for each socket of group g   # clears records
```

* **Classification change** (the actual semantic fix): for a registration that is not a hang-up
  registration, `POLLHUP` means READ only when READ is requested. With WRITE interest alone,
  `POLLHUP` is not deliverable by itself: only `POLLWRNORM` is, and `POLLERR` is delivered as WRITE so
  the send fails and the caller sees the reset. Hang-up registrations keep today's mapping
  (`kl_hup_ready`).
* **No spin:** a half-closed socket with a full send path is classified "nothing deliverable", and the
  wait blocks on events; `FD_CLOSE` was recorded once and enumerated, so it does not re-signal.
  Measured in the prototype (`probe_win_hybrid.c`): 1 loop iteration over a 1 s wait, then WRITE as
  soon as the peer reads.
* **Race-free:** groups are reset before the classifying `WSAPoll(0)`; any change after it records a
  network event, which signals the group, which ends the wait.
* **Level semantics preserved:** every wait starts with a `WSAPoll(0)`, so a condition that is still
  true (a read left unread, a writable socket re-armed) is returned at once, as today (prototype: a
  read left unread is delivered again in 0 ms).
* **Timers:** the wait's timeout, as today. **Cross-thread wakeups:** `KlWakeup` on Windows is a
  loopback socket pair registered for READ, so it is an ordinary registration (prototype: 109 ms for a
  100 ms signal). No extra handle or thread.

### Registration

* `add` / `mod`: `WSAEventSelect(s, group_event, net_events(interest))`, with
  READ → `FD_READ | FD_ACCEPT | FD_CLOSE`, WRITE → `FD_WRITE | FD_CONNECT | FD_CLOSE`,
  HANGUP → `FD_CLOSE`. The event set only decides what wakes the wait; what is delivered is always
  the `WSAPoll(0)` classification.
* `del`: `WSAEventSelect(s, NULL, 0)`, so the socket can be made blocking again and signals nothing.
* Group assignment: round-robin over 63 event objects per loop (the 64th wait slot stays free), stored
  per registration beside `udata` and `hup`. No per-socket handle.

### Cost and limits

* Per loop: 63 event handles, created at init, closed at close. Per registration: one group index.
* Per wait: one `WSAPoll(0)` over all registrations (as today's single `WSAPoll`), one
  `WSAWaitForMultipleEvents`, and, when woken, `WSAEnumNetworkEvents` over the signalled group (about
  n/63 calls; 31 for 2000 sockets).
* Per `add`/`mod`/`del`: one `WSAEventSelect` (a syscall, like `epoll_ctl`).
* No thread, no timer, no polling, no undocumented interface, no IOCP, no HTTP knowledge.

### Side effects to handle

* **Accept inheritance:** an accepted socket inherits the listener's association. Keel registers it
  at once (the association is then replaced). A socket accepted but never registered on this loop
  (handed to another loop) would keep signalling this loop's group event and could not be made
  blocking; the backend cannot see it. Options: clear the association in the Winsock provider's
  accept (one `WSAEventSelect(s, NULL, 0)`; a provider change), or document that a socket accepted
  from a WSAPoll-loop listener belongs to that loop until registered elsewhere. To decide.
* **Blocking mode:** an associated socket cannot be made blocking; Keel's registered sockets are
  nonblocking already, and `del` releases the association.
* **`FD_WRITE` re-arming:** see 5a. Measured: an unwritable state without a would-block is common,
  and re-issuing `WSAEventSelect` does not re-arm `FD_WRITE`. The design as first written loses that
  wakeup.

### What changes elsewhere

* (As first written for option D; not what shipped.) `src/event_wsapoll.c` only, with the event API,
  `KL_EVENT_CAP_HANGUP`, the socket provider, the completion axis and HTTP unchanged.
* PR #514 then drops its WSAPoll workaround (`stream_write_off`, and the write watch's bare-READ
  peek), because a write-only registration no longer receives a bare READ for a FIN. BSD poll still
  reports a reset to a write-only registration as `POLLHUP | POLLERR`; giving `event_poll.c` the same
  classification (ERR as WRITE, HUP not deliverable to WRITE-only interest) removes the last reason for
  that path, and is a small follow-up.

## 5a. Proof obligations (measured after approval, `probe_win_proof.c`, `probe_win_zerosend.c`)

**B. Shared event objects: satisfied.** A network event recorded on a socket whose earlier record
was never enumerated still signals the (reset) event object again (`recv` then more data: signalled;
then a FIN: signalled). Resetting a shared event therefore cannot erase another socket's later
notification; the classifying `WSAPoll(0)` after the reset covers conditions that were already true.
An `FD_READ` that `recv` has not re-enabled does not signal again, which the level `WSAPoll(0)` also
covers.

**Accept inheritance: the provider fix works.** `WSAEventSelect(accepted, NULL, 0)` succeeds, keeps
the 5 pending bytes, lets the socket be made blocking, lets it be associated with another event (which
then records `FD_READ` for the queued data), and leaves the listener's `FD_ACCEPT` association intact.

**A. `FD_WRITE`: NOT satisfied as designed.** With every `SO_SNDBUF` (4 KiB, 8 KiB, 64 KiB, default)
and send size (1 KiB to 2 MiB) tried, a socket became unwritable (`WSAPoll`: no `POLLWRNORM`)
through **successful** sends alone, without any `WSAEWOULDBLOCK`; when the peer then read everything,
`FD_WRITE` was **never** recorded, with the association kept or re-issued while unwritable. A wait
that classified such a socket "not writable" and blocked would sleep through its writability: a lost
wakeup. (`SO_SNDBUF` 0 hung the probe and was dropped; Keel never sets it.)

**A possible repair, not yet adopted.** On such a socket a zero-length `send` returned
`WSAEWOULDBLOCK` in all 12 combinations (8 KiB and default `SO_SNDBUF`, 1 KiB to 1 MiB sends, peer
open or half-closed), transmitted nothing (the peer received exactly what was sent), and re-armed
`FD_WRITE`, which was then recorded when the peer read. The wait would issue it, before blocking, for
each TCP registration with WRITE interest that `WSAPoll(0)` classified unwritable. Caveats:

* It rests on **observed**, not documented, behavior: Microsoft documents a zero-length `send` as
  "treated as successful, returns zero", without describing a full send path.
* It must never be issued on a datagram socket (a zero-length `send` there sends an empty datagram),
  so the backend would record each registration's socket type (`SO_TYPE`) at `add`.
* UDP has the same `FD_WRITE` re-arm rule and has not been probed for an unwritable-without-would-block
  state.

## 5b. A select()-based wait (measured, `probe_win_select.c`)

| peer | select() read + except sets | WSAPoll(0) HUP | peek |
|---|---|---|---|
| data only | read | no | data |
| FIN, half-close | read | yes | EOF |
| FIN behind unread data | read | yes | data |
| reset (with or without data) | read | yes | error |

`select()` reports a FIN, a half-close and a reset only as readability, exactly like data, and its
except set never signals one. A hang-up-only watch therefore cannot be expressed: kept in the read set
it wakes on every check while unread data is queued (100/100, a spin); left out of the read set while
`WSAPoll(0)` says data is queued, it never wakes for a FIN arriving behind that data. WSAPoll can
classify the hang-up only when something else wakes the loop, so a select()-based backend cannot keep
`KL_EVENT_CAP_HANGUP` without a periodic wake. Write readiness after a half-close is correct (section 2:
not reported while the send path is full, reported once the peer reads), and the cost scales
acceptably with a dynamically sized `fd_set`:

| sockets | one becomes readable: wake (us) | 1000 idle 0-timeout calls (ms CPU) |
|---|---|---|
| 64 | 8 | 16 |
| 256 | 21 | 31 |
| 1000 | 73 | 62 |
| 2000 | 140 | 141 |

## 6. Tests the implementation PR needs (on `edge`)

* The Keel half-close measurement above: all policies deliver 4 MiB with `RESET_ONLY` and `OFF`.
* `test_http_suspend_hangup`, `test_completion_output`, `test_http_async`, `test_async`,
  `test_unix_socket` node tests, the datagram suites, and the Windows smoke targets, on WSAPoll.
* A spin guard: a half-closed socket with a full send path held for 1 s costs one loop iteration.
* Scale: 2000 registrations over 63 groups, one becomes ready.
* `FD_WRITE` edge cases (section 5), accept inheritance, and `del` releasing the association.

## 7. Complexity and sequencing

* Backend PR: medium, about 150 lines in one TU plus tests; independently reviewable.
* Then PR #514 removes its workaround (small).
* Then P0-4b.

## 8. Decision: select(), without KL_EVENT_CAP_HANGUP

Option D (section 5) failed its first proof obligation (section 5a): a socket becomes unwritable
through successful sends alone and `FD_WRITE` is then never recorded, so the wait could sleep through
writability. Its only repair found, a zero-length `send` to re-arm `FD_WRITE`, rests on undocumented
behavior and would make the event backend perform transport operations; it was rejected.

Write progress is a readiness invariant; a hang-up-only watch is an optional, capability-negotiated
enhancement, and Keel already has the mechanism for its absence. The Windows readiness backend is
therefore `select()` (option B), and it does not advertise `KL_EVENT_CAP_HANGUP`: suspended
connections on it use the 1 s peek sweep, as on IOCP (which keeps streaming progress through its
completions and also has no hang-up-only watch).

* `src/event_winselect.c` replaces `src/event_wsapoll.c`; it is the Windows readiness default, and
  `BACKEND=winselect` or the old `BACKEND=wsapoll` select it.
* The descriptor sets are sized at run time to the registrations: no registration is dropped because
  a fixed `FD_SETSIZE` ran out, and an `add` that cannot grow them fails.
* A socket closed without `kl_event_del` is detected only when a `select()` fails with
  `WSAENOTSOCK`: the backend then marks the handles the stack no longer knows as dead (not reported
  until a registration names the value again) and waits again, so one stale registration cannot
  stop the loop. If Winsock has already reused the value for another socket, `select()` does not
  fail, and the stale registration reports that socket's readiness to its old owner. The IOCP
  engine goes further for a watcher: a closed watched socket orphans its watch, which is never reported or posted
  again until a registration names the handle and the handle is verifiably on that loop's port
  (freshly associated, or associated by that loop itself), so a probe is never re-posted on a
  handle value that may by then name an unrelated socket (see `kl_event_del` in
  `include/keel/event.h` for the one remaining window and what teardown retains for it).
* A wait returns at most `max` events, and which of the ready registrations those are is decided
  by laps (`src/event_turn.h`), not by position: a wait reports only registrations not yet reported
  in the current lap, a lap ends when none of those is ready, and a new registration joins as
  already reported, so arrivals wait for the next lap instead of extending this one. A registration
  that stays ready is reported within ceil(N / `max`) waits for a set registered together and within
  2 * ceil(N / `max`) waits in general, connection churn included (every reported registration
  closed and replaced each wait). Scanning from slot 0 served the first `max` busy sockets on every
  wait and starved the rest; a removal is a plain swap with the last slot, since order plays no part.
* `KL_EVENT_HANGUP` alone is accepted and never reported. A failed connect (`select()` except set) is
  reported as WRITE, and the next operation sees the error.
* No socket-provider change is needed: accepted sockets inherit nothing from `select()`.
* The except set is classified when it fires, without tracking socket state. It reports a failed
  connect and also pending TCP urgent (out-of-band) data, which Keel never reads and which would stay
  set (a review found it: one urgent byte from a peer made a waiting writer spin at 100% CPU). When a
  socket is in the except set but not the write set, the backend reads `SO_ERROR`: an error (a failed
  connect, any socket error) is reported as WRITE so the next operation sees it; no error can only be
  urgent data, so that registration stops watching the except set until its identity changes (`add`,
  an upsert, a reused handle). One extra wakeup per urgent byte, no spin, and no inferred connection
  state: "not connected" (an unconnected UDP socket) is never mistaken for "connecting".
* An infinite wait with nothing to watch blocks, as epoll and kqueue do.

### Measured performance (Windows 10.0.26300, idle TCP pairs plus one active, round trip per event)

| registrations | select() | WSAPoll (before) |
|---|---|---|
| 64 | 16 us | 19 us |
| 256 | 29 us | 63 us |
| 1000 | 86 us | 412 us |
| 2000 | 160 us | 1330 us |

