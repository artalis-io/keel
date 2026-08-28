# F2-D decision record: KlEventLoop.fd

Status: DECISION for review (F2-D). Docs-only. No header, struct, backend, consumer, test, or
integration changed by this document. Grounded in the live tree at base commit `4e667e1` on branch
`main`.

Scope: decide the v3 fate of the public field `KlEventLoop.fd` (event.h). Per the F2 plan this is its
own reviewed v3 API increment, kept separate from the broader KlEventCtx/KlEventLoop layout question
(F2-B) and sequenced before it, because KlEventLoop is embedded by value in KlEventCtx. This record
decides the field; implementation is a separate reviewed step.

## 0. The field

```
typedef struct {
    int fd;             /**< epoll_fd or kqueue_fd, -1 for io_uring */
    void *_backend;     /**< reserved for backend-specific state */
    KlAllocator *alloc;
    const KlEventOps *ops;
} KlEventLoop;   /* event.h:26-31, embedded by value as the first member of KlEventCtx */
```

`fd` is a public, concrete, inline field whose documented meaning is a backend descriptor. It is the
single most backend-leaky field on the public event surface.

## 1. Complete reader/writer inventory (whole tree)

Every access to a KlEventLoop's `fd` across `src/`, `include/`, `integrations/`, `examples/`, and
`tests/` (grep for `loop->fd`, `loop.fd`, `.loop.fd`):

| location | accesses | nature |
|---|---|---|
| src/event_epoll.c | 8 meaningful + 1 close-reset | READS/WRITES: `fd = epoll_create1(0)`, `epoll_ctl(fd,...)` x3, `epoll_wait(fd,...)`, close-guard + `close(fd)` + `fd = -1` |
| src/event_kqueue.c | 8 meaningful + 1 close-reset | READS/WRITES: `fd = kqueue()`, `kevent(fd,...)` x4, close-guard + `close(fd)` + `fd = -1` |
| src/event_poll.c | 2 | WRITES `fd = -1` only (init + close) |
| src/event_pollcomp.c | 2 | WRITES `fd = -1` only |
| src/event_iouring.c | 2 | WRITES `fd = -1` only |
| src/event_iocp.c | 2 | WRITES `fd = -1` only |
| src/event_wsapoll.c | 2 | WRITES `fd = -1` only |
| integrations/platform/lwip/event_lwip.c | 2 | WRITES `fd = -1` only |
| integrations/platform/lwip/event_lwip_raw.c | 2 | WRITES `fd = -1` only |
| examples/lwip/event_lwip.c | 2 | WRITES `fd = -1` only |

Totals: `include/` 0, `tests/` 0, generic dispatch / `event_ctx.c` / `completion_*` 0, protocol code 0,
UEFI `event_efi.c` 0. The `fd` tokens that appear in `event_ctx.c` and `completion_dispatch.c` are the
WATCHED SOCKET descriptor (`w->fd`, a function parameter), passed alongside `&ctx->loop`; they are not
reads of `KlEventLoop.fd`.

The inventory is unambiguous: `KlEventLoop.fd` is read as a value by exactly two backends (epoll,
kqueue), and only inside their own translation units. Every other backend writes `-1` and never reads
it. No public header, no generic/protocol code, no test, no example (beyond the `-1` writes), and no
UEFI/Windows/io_uring/lwIP backend reads it.

## 2. Backend-by-backend meaning and _backend ownership

- epoll: `fd` IS the epoll instance (`epoll_create1`), used for every `epoll_ctl`/`epoll_wait`, closed
  at teardown. epoll uses NO `_backend` (0 references): its only per-loop state is this descriptor.
- kqueue: `fd` IS the kqueue instance (`kqueue()`), used for every `kevent`, closed at teardown. kqueue
  uses NO `_backend` (0 references): same shape as epoll.
- poll (7 `_backend` refs), pollcomp (20), io_uring (20), iocp (20), wsapoll (7): all real per-loop
  state lives in an allocated `_backend` block (pollfd array / completion double / ring / completion
  port); `fd` is set to `-1` and never read.
- lwIP (BSD and raw NO_SYS) and UEFI: `_backend` or backend-private state holds everything; lwIP sets
  `fd = -1`, UEFI does not touch `fd` at all.

So epoll and kqueue are the only backends that use the inline `fd`, and they are precisely the two
backends that do NOT use `_backend`. The reserved `_backend` slot is free on exactly the backends that
currently need somewhere to keep a descriptor.

## 3. Allocation, embedding, copies, initialization

- KlEventLoop is embedded BY VALUE as `KlEventLoop loop;` in KlEventCtx (event_ctx.h:53); KlEventCtx is
  stack- or embed-allocated by consumers (a stack `KlEventCtx`, or `KlHttpServer.ev`). There is no heap
  allocation of KlEventLoop itself.
- KlEventLoop is NEVER copied or returned by value anywhere in the tree; it is always operated on
  through a `KlEventLoop *`. So there is no value-copy that could truncate or duplicate a live
  descriptor.
- Initialization: `kl_event_ctx_init` sets `ctx->loop.ops = NULL` then calls
  `kl_event_init[_provider]`, which sets `fd` (epoll/kqueue) or `-1` (all others). The field is written
  by the backend, never left indeterminate.

## 4. Storage mechanism and its allocation/failure consequences

The epoll/kqueue descriptor is relocated into a small private backend state held in the already-reserved
`_backend` slot and allocated through `loop->alloc`, matching the existing poll/pollcomp/io_uring/
IOCP/WSAPoll ownership model:

```
typedef struct { int fd; } KlEpollState;    /* and the equivalent KlKqueueState */
```

Pointer-encoding the descriptor as `(void *)(intptr_t)fd` is REJECTED. It has two correctness problems
that make it unacceptable representation debt in a v3 cleanup:

- A valid descriptor can be `0` (for example when standard input is closed before the loop is created).
  `0` encodes as `NULL`, colliding with the zero-initialized / closed `_backend` state, so the loop
  could not distinguish "fd 0" from "no backend".
- Integer-to-pointer conversion is implementation-defined and yields a non-object pointer value. Even
  where it happens to work on current POSIX targets, it is unnecessary representation debt.

Removal therefore DOES change allocation and failure behavior for epoll/kqueue, in a small and
well-understood way that matches the other backends:

- One initialization-time allocation: `kl_event_init` for epoll/kqueue allocates a `KlEpollState`/
  `KlKqueueState` via `loop->alloc` and stores it in `loop->_backend`. This is outside the hot path
  (once per loop), exactly like the pollfd/ring/completion-port state the other backends already
  allocate.
- One new allocator-failure path: if the state allocation fails AFTER the kernel descriptor was
  created, the newly created `epoll_create1`/`kqueue` fd MUST be closed and `kl_event_init` returns
  `-1`, leaking neither the descriptor nor memory.
- `_backend == NULL` is the zero-initialized and post-close state. Backend ops treat a NULL `_backend`
  as "no live descriptor"; `kl_event_close` frees the state through `loop->alloc`, closes the fd, and
  leaves `_backend == NULL` so close is idempotent and the resource is released exactly once.
- Close ordering: close the kernel descriptor, then free the state, then clear `_backend` (or free then
  clear with the fd captured first); the order is fixed so there is exactly one `close` and one free per
  successful init, and none on a failed init beyond the descriptor cleanup above.

Removing `fd` shrinks KlEventLoop by one `int`; because it is only ever stack/embedded and never
value-copied, no consumer's layout math or value-copy changes. The only new runtime surface is the
single per-loop epoll/kqueue init allocation and its failure path described here.

## 5. Freestanding, lwIP, UEFI, Windows consequences

- Freestanding (UEFI): `event_efi.c` never reads `fd`; removal is a no-op for UEFI. No hosted `fd`
  concept is needed on bare firmware.
- lwIP (BSD and raw NO_SYS, in-tree integration and example): today they write `fd = -1`; after removal
  they simply stop writing it. No behavior change, no cost.
- Windows (wsapoll, iocp): today they write `fd = -1`; after removal they stop. Their real state is in
  `_backend`. No cost.
- io_uring: same, `fd = -1` today, its ring lives in `_backend`. No cost.

No freestanding or cross-platform cost is incurred by removal; several backends lose a meaningless
`fd = -1` line.

## 6. Interaction with F2-B (KlEventCtx / KlEventLoop opacity)

Removing `fd` is beneficial and independent of F2-B, and doing it first makes F2-B cleaner:

- If F2-B later makes KlEventLoop opaque (layout moved behind a `*_detail.h`), `fd` would be off the
  public surface regardless; removing it beforehand means F2-B reasons about a smaller KlEventLoop
  (`_backend`, `alloc`, `ops`), all of which are either the opaque reserved slot or infrastructure.
- If F2-D removal lands first and F2-B opacity never happens, KlEventLoop still no longer leaks a
  descriptor by name.
- The two decisions compose; neither blocks the other. F2-D's decision (remove) holds whether or not
  F2-B makes the struct opaque.

## 7. Options

- Remove. Delete `fd`; epoll/kqueue store their descriptor in a small `loop->alloc`-allocated backend
  state (`KlEpollState`/`KlKqueueState`) held in `_backend` (section 4). Cost: a one-time v3 break of a
  field no legitimate consumer reads, one per-loop init allocation with an allocator-failure path for
  epoll/kqueue, and a small edit to event_epoll.c/event_kqueue.c. A resurrection gate keeps it gone.
- Reserve as explicit legacy. Keep `fd`, re-document it as a legacy/internal slot, and gate its readers
  to the two named backend TUs. Cost: the leaky field stays on the public surface forever, and a gate
  is still required (to bound its readers) with strictly more surface than the removal gate.

## 8. Decision

Remove `KlEventLoop.fd` in v3. The inventory proves there is no legitimate public consumer (zero reads
in headers, generic code, protocols, tests, examples, or any backend other than epoll/kqueue, which
read it only inside their own TUs), and removal carries no freestanding, Windows, lwIP, or UEFI cost.
The epoll and kqueue descriptors move into a small `loop->alloc`-allocated backend state
(`KlEpollState`/`KlKqueueState`) held in `_backend` (section 4), NOT into a pointer-encoded integer.
Removal adds one per-loop init allocation and its allocator-failure path for epoll/kqueue, matching the
ownership model the other five backends already use. This is a justified, cleanly bounded v3 break: it
deletes the most backend-leaky field on the public event surface with no realistic source impact.

Reserve is rejected: it permanently keeps a descriptor-shaped field on the public surface for no
benefit, and still needs a gate.

Implementation (a separate reviewed increment, held until F2-B establishes the final KlEventLoop/
KlEventCtx representation): delete the field; in event_epoll.c/event_kqueue.c allocate
`KlEpollState`/`KlKqueueState` through `loop->alloc` at init, store it in `_backend`, load the fd from
it, and free it at close with the ordering and exactly-once guarantees of section 4; drop the
`fd = -1` lines from the other backends and the lwIP integrations/example; add the gate below; and add
the tests in section 10.

## 9. Gate design (prevent reintroduction and new consumers)

`check-no-eventloop-fd` (a resurrection gate in the style of `check-no-kludp`): default-deny,
self-canaried, `file:line` diagnostics, comment/splice-aware, BSD+GNU-portable, run over tracked
files. Because removal means NO backend keeps a named `fd` (epoll/kqueue use `_backend`), the gate
permits ZERO occurrences of either:

- a `KlEventLoop` struct definition containing an `fd` member; and
- any read/write of `loop->fd` / `.loop.fd` (a KlEventLoop's `fd`), which also blocks a new generic or
  protocol consumer from reaching for it.

Self-canary: a temporary file that adds `loop->fd` must fail the gate; the clean tree must pass. (If
the reserve option were taken instead, the gate would keep an allowlist of exactly
`src/event_epoll.c` and `src/event_kqueue.c` as the only authorized readers; removal makes the
allowlist empty, which is the stronger and simpler form.)

## 10. Migration

- In-tree readers updated in the implementing commit: event_epoll.c and event_kqueue.c (descriptor to
  a `loop->alloc`-allocated `KlEpollState`/`KlKqueueState` in `_backend`).
- In-tree `-1` writers cleaned up: event_poll.c, event_pollcomp.c, event_iouring.c, event_iocp.c,
  event_wsapoll.c, and the lwIP integration/example backends (drop the `fd = -1` line).
- External consumers: none expected; the field was documented as a backend descriptor and no public
  or protocol path reads it. A 3.0.0 consumer that erroneously read `ctx.loop.fd` would fail to
  compile and must stop; that is the intended, surfaced break, not a silent behavior change.
- No test or example reads the field, so no test/example migrates beyond the lwIP `-1` cleanup.
- New tests required in the implementing commit:
  - allocator-failure at epoll/kqueue init: a failing `loop->alloc` after the kernel descriptor is
    created must return `-1`, close the descriptor, and leak neither the fd nor memory (verified under
    ASan/LSan);
  - valid backend descriptor equal to zero: with the loop's kernel fd forced to `0` (for example by
    closing stdin before init), the loop must operate and tear down correctly, proving the state uses
    `_backend != NULL` rather than a pointer-encoded fd that would alias `0` with `NULL`.

## 11. Validation

Docs-only. `git diff --check`; `make check-doc-refs`, `make check-old-layout`, `make
check-no-milestones`, `make check-no-em-dash`; pure ASCII; no build, header, backend, test, or
integration file changed. Nothing pushed; no remote CI. The field deletion, backend edits, and the new
gate are held for a separate reviewed implementation increment.
