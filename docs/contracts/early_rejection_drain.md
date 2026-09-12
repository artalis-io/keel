# Keel early-rejection drain

Authoritative contract for what Keel does to a connection when it sends a **final response that
terminates a request while unread request-body data may still be arriving**: a 413 on an oversized
upload, a 431, a 415, a 408 on a stalled upload, a 500 from a body reader. Companion to
`docs/contracts/stream.md`. Identical across the readiness and completion axes; only the read
mechanism differs.

## Why this exists

Closing a socket that still has unread received data makes TCP send **RST**, not FIN. A reset
discards data the peer has already buffered, so on Windows the client loses the very response that
explains the rejection; the symptom is a response the server provably sent and the client never
saw. POSIX is exposed to the same reset and is merely more forgiving about what the peer can still
read afterwards.

So the invariant is about teardown, not about writing:

> Once Keel has committed to sending a final response, connection teardown must not invalidate that
> response merely because unread request bytes remain.

## The bounded-effort guarantee, and its limit

The guarantee is deliberately narrow:

> Keel makes a **bounded** best effort to preserve the final response during early rejection. It
> does not permit an untrusted peer to force unbounded draining in order to guarantee delivery.

Those two goals genuinely conflict, and a flooding client can always create the conflict:

| goal | when they collide |
|---|---|
| deliver the final response reliably | loses |
| keep resource consumption bounded | **wins** |

Stated normatively, because it is a security boundary rather than an implementation detail:

> **If the peer continues transmitting beyond the configured rejection-drain budget, Keel may
> terminate the connection even if doing so prevents reliable delivery of the final response.**

`byte_cap_ends_the_drain_without_promising_delivery` in `tests/protocols/http/test_reject_drain.c`
exists to pin exactly that sentence. It asserts the drain **stops**, and deliberately does **not**
assert the client received the response. It is not an incomplete test and it is not a bug: making
the drain unlimited would make it pass and would hand any peer an unbounded hold on a
single-threaded loop. Do not "fix" it.

## Ownership

One internal entry point owns the whole transition, so no rejection site carries teardown policy
and none of them mentions TCP:

```
kl_http_conn_reject_final(conn, response, len)

    response
      -> physical write retirement      (completion axis: the write is retired, not merely queued)
      -> shutdown(KL_SHUT_WR)           (peer sees orderly end-of-stream; our receive side stays up)
      -> bounded input drain            (KL_HTTP_CONN_DRAINING)
      -> close
```

HTTP decides only *"this is a terminal rejection with potentially live inbound input"*. Entry is
gated on unread input remaining, not on which status code it is, so an ordinary successful
keep-alive response (whose body is already consumed) never enters the drain.

## Dispatching the state, for implementers

`DRAINING` is a state the connection drivers **return**, so every consumer of a returned
`KlHttpConnState` has to dispatch it. The rule:

> Dispatch a returned state semantically. Never collapse it into a two-way "the state I expect,
> else close" test.

This is not a style preference. The enum grows, and a two-way test silently mis-handles each new
member. When `DRAINING` was added, four completion-axis sites treating "not `READING`" as "close
now" closed connections on top of unread request bytes and destroyed responses that had already
been written, which is the exact failure this contract exists to prevent. A fifth site, on the async
resume path, matched no branch at all and left the connection registered for nothing, so the drain
could only limp forward on idle-sweep ticks.

Either switch over the states, or route anything a site does not specifically handle to a shared
dispatcher rather than to a close:

| axis | dispatcher |
|---|---|
| completion, after a dispatch | `comp_after_state()` |
| completion, after a response is retired | `comp_after_send_complete()` |
| readiness | the `transition:` block in `http_server.c` |

### How this is enforced

Not by convention. These four dispatchers are exhaustive `switch`es over `KlHttpConnState` with **no
`default:`**, and the compiler does the rest: `-Wall` enables `-Wswitch`, which under `-Werror` makes
an omitted member a build error. Adding a state to the enum therefore fails to compile until every
dispatcher has decided what to do with it.

A `default:` silences that check completely, which is the one thing review reliably fails to notice.
`make check-state-dispatch` keeps it out, and `make check-state-dispatch-selftest` proves the gate
still detects a planted `default:`. Both run in CI.

`-Wswitch-enum` was considered and not adopted. It differs from `-Wswitch` only in also warning on
switches that *do* have a `default:`, so for these dispatchers it adds nothing over the no-`default:`
rule already enforced above. Tree-wide it would flag 17 further switches whose `default:` is the
right design (for instance dispatching a couple of `KL_COMP_*` event kinds and ignoring the rest),
where enumerating every member would be noise and would force unrelated edits whenever any of those
enums grew.

A state that genuinely cannot occur at a site still gets a named arm, with a comment saying why and
what the site does anyway. That is the point: the decision is recorded per state, not left to an
`else`.

## Termination conditions

The drain ends on the **first** of these:

| condition | note |
|---|---|
| a read would block **and** the body framing is complete | the transport-level completion test |
| peer EOF | nothing more is coming |
| byte budget exhausted | `reject_drain_max_bytes`, default 64 KiB |
| deadline expired | `reject_drain_timeout_ms`, default 500 ms |
| socket error or reset | no point continuing |

Draining is asynchronous throughout: **one bounded read per loop progression**, never a blocking
read-until-empty. It must never stall the single-threaded loop, which is why both caps exist and
why a stalled client is resolved by the deadline rather than parked.

### The residual: a peer that resumes sending after an empty queue

One case is deliberately not protected, and this is the normative statement of it:

> After an early final response, Keel drains unread inbound data within the configured byte and time
> bounds. If HTTP framing is complete and the transport reports no currently readable data, Keel may
> close immediately. A peer that continues transmitting after that observation may therefore still
> cause an abortive TCP close. Keel does not delay every early rejection solely to guard against
> future protocol-invalid excess input.

The reason it stays this way is that the condition is not decidable from HTTP state:

```
framing complete
+ the current read would block
+ the peer may or may not send more
```

Nothing in the protocol distinguishes "the peer is finished" from "the peer is between writes", so the
choice is a policy one rather than a correctness one. Three policies were considered:

| policy | cost |
|---|---|
| close on would-block (what Keel does) | bounded and prompt; a rare abortive close remains possible if bytes arrive just after the empty-queue observation |
| linger to the deadline after framing completes | every ordinary early rejection pays the timeout, or extra loop state, even when the peer is plainly done |
| a short post-empty grace window | a better compromise, but it invents a new teardown semantic needing its own cross-platform definition and tests |

The first is chosen because the mechanism's whole purpose is BOUNDED effort, and the other two spend
unbounded-in-practice attention on a protocol-violating peer. Measured residual:
`integration.post_413`, whose client over-sends its declared Content-Length, fails about 2 runs in 16
in-suite on both axes and 0 in 20 in isolation, with the trace signature `wouldblock-complete`.

This is accepted behaviour for 3.x, not an open defect. It would be worth revisiting only if a real
workload shows the residual abortive-close rate mattering enough to justify a transport-level grace
mechanism, which would need the event axis rather than a blocking check on the drain path.

### Framing complete is not the same as receive queue empty

A subtle point worth stating, because the obvious rule is wrong:

```
framing COMPLETE   !=   socket receive queue empty
```

A peer that declares `Content-Length: 100000` and then pushes 122880 bytes has satisfied the
framing while leaving 22880 bytes queued. Closing there provokes the same abortive reset the drain
exists to avoid. So framing completion stops Keel **requiring** more input; the drain itself ends
when a read would block, i.e. when the receive queue is actually empty. Nothing waits for that:
the budget and the deadline still bound everything, and before framing completes a would-block
keeps the drain armed so the deadline resolves it.

## Body completion is the framing's verdict

Discarded bytes are fed through the **real** body framing, not counted. For chunked transfer
encoding, wire bytes and decoded body bytes differ, so a byte tally can never tell when the request
ended: such a drain could only ever finish on a cap, never on the terminal chunk.

| decoder state | meaning for the drain |
|---|---|
| finished | the framing oracle says the body ended |
| still active | keep parsing and draining |
| already errored | the oracle is **unavailable**; bounded raw drain, caps only |

That last row matters: a chunked rejection is frequently the decoder *itself* reporting the error,
so it can already be dead when the drain starts. Treating decoder failure as permission to close
immediately reintroduces the reset on exactly the connections this contract is about.

`request_body_received` is only an optimization: it lets a `Content-Length` drain stop early instead
of sitting out its deadline waiting for bytes the client already finished sending. It is never the
source of truth for completion.

## Diagnosing it

The drain is timing-sensitive, and an `fprintf` on its path is not a usable instrument: adding one
while investigating a failure made that failure disappear entirely (1 run in 10 failed without it, 0
in 10 with it, same binary). Build with `-DKEEL_INTERNAL_TRACE` instead, for example

```
make KEEL_EXTRA_CFLAGS=-DKEEL_INTERNAL_TRACE
```

and each drain records which terminal condition won, into a fixed ring formatted only at exit (see
`src/internal_trace.h`). The records name the reason and carry fd, declared length, bytes received,
remaining budget, milliseconds left on the deadline, and the framing flags, which is enough to tell
the bounds apart from each other and from framing completion. It is internal and compiled out of a
default build; it is not public API.

## Configuration

Both caps live on `KlHttpServerConfig`:

| field | default |
|---|---|
| `reject_drain_max_bytes` | `KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_BYTES` (64 KiB) |
| `reject_drain_timeout_ms` | `KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_MS` (500 ms) |

This section is the NORMATIVE description of the zero handling. The header comment and any other prose
summarise it and link here; if they ever disagree, this is the one that is right.

Each field is **independent**, and a zero value selects **that field's default**. That is the rule
`KlHttpServerConfig` states for every one of its members: "every member is optional and its zero/NULL
value selects the built-in default". So:

| `reject_drain_max_bytes` | `reject_drain_timeout_ms` | result |
|---|---|---|
| 0 | 0 | 64 KiB, 500 ms |
| 128 KiB | 0 | 128 KiB, 500 ms |
| 0 | 1000 | 64 KiB, 1000 ms |
| 4096 | 250 | 4096 bytes, 250 ms |

**Zeroing these fields cannot disable the drain.** To turn it off, set `reject_drain_disable = 1`. The
flag DOMINATES: the numeric fields are ignored when it is set, not merged, so

```c
cfg.reject_drain_disable   = 1;
cfg.reject_drain_max_bytes = 65536;   /* ignored */
cfg.reject_drain_timeout_ms = 500;    /* ignored */
```

is disabled. With the drain off, teardown after an early final response behaves as it did before the
drain existed: the connection closes immediately, which can reset away a response the peer has not yet
read. That is the whole point of the mechanism, so choose it only deliberately.

### This was wrong in 3.0.0

3.0.0 shipped documentation, here and in the header, saying that setting **both** fields to 0 disabled
the drain. It did the opposite: `kl_http_server_init` applied the defaults only when both were zero, so
zeroing both ENABLED the drain at 64 KiB / 500 ms, while zeroing exactly one disabled it as an
undocumented side effect. An embedder who read the header and chose to disable got the drain; an
embedder who set nothing got the right behaviour by accident. The fields are now normalised
independently and `reject_drain_disable` is the only off switch (#293).

## Transport requirement

The half-close goes through the socket seam as `KlSocketOps.shutdown(ctx, fd, KlShutdownHow)`, a
Keel enum (`KL_SHUT_RD` / `_WR` / `_RDWR`) rather than `SHUT_WR` or `SD_SEND`, so a provider with no
POSIX notion of a half-close (lwIP raw, EFI_TCP4) can interpret or refuse it on its own terms. The
op is best-effort: a NULL slot selects the built-in native shutdown, and a provider returning -1 is
tolerated. The drain still reduces the reset window without it; the half-close simply lets the peer
see an orderly end-of-stream while Keel keeps reading.
