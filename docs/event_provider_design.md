# Runtime event-backend provider (KlEventProvider)

Makes the **readiness event backend runtime-pluggable**, parallel to the socket
seam (`KlSocketProvider`). The motivation is a bring-your-own event backend —
lwIP (`lwip_poll`) — that can be linked as a separate `.a` and installed at
runtime, without recompiling the Keel core. Companion to
`docs/lwip_platform_design.md` and `docs/capability_matrix.md`.

## The seam

`KlEventOps` (in `keel/event.h`) is a vtable over the eight event-loop
operations the core calls: `init / add / mod / del / wait / close / caps /
native_provider`. A `KlEventProvider` is `{ const KlEventOps *ops; const char
*name; }`. `KlEventLoop` gains an optional `const KlEventOps *ops`.

The public `kl_event_*` API is implemented once in `event_dispatch.c`:

```c
int kl_event_add(loop, …) {
    return loop->ops ? loop->ops->add(loop, …) : kl_event_add_builtin(loop, …);
}
```

Each compiled backend (epoll/kqueue/poll/WSAPoll/io_uring/IOCP/pollcomp) renamed
its eight functions to `*_builtin`; the selected one (Makefile `BACKEND=`) is
still linked in and called directly. So:

- **Default path (all shipped backends): `loop->ops == NULL`** → one
  perfectly-predicted branch, then a direct call to the same `*_builtin` code as
  before. No vtable indirection, no behavior change, no measurable overhead.
- **Runtime provider (lwIP): `loop->ops` set** → the same calls route through the
  provider. Installed via `KlEventCtx.event_provider` →
  `kl_event_init_provider()`; `kl_event_init()` itself always takes the builtin
  path and zeroes `ops` (so a stack-allocated loop is safe).

## Scope: readiness only

`KlEventOps` is the **readiness** event API. That is exactly what lwIP needs
(its sockets layer is `lwip_poll`). The **completion** axis (io_uring / IOCP /
pollcomp) drives its loop through a *separate* API — `kl_comp_run` / `kl_comp_*`
(`io_engine.h`) — which is **not** part of this vtable and is **untouched**.
Completion backends still implement the eight `kl_event_*` functions (for watcher
fds + `native_provider` auto-wiring); those go through the dispatcher like any
other, but their submit/drain path is unaffected. `loop->ops` stays NULL on a
completion loop.

**Completion stays build-time by deliberate policy**, not oversight: there is no
bring-your-own completion backend today (io_uring is Linux-native, IOCP is
Windows-native, mutually exclusive; the only hypothetical is lwIP-raw). By the
same two-consumer rule that justifies the readiness seam (default + lwIP = two),
a completion seam waits for a real second consumer. The identical pattern — a
`KlCompOps` vtable + dispatcher — drops in later with no rework if one appears.

## Matched provider/backend pairing

A socket provider and event backend must agree on what a "pollable handle" is: a
provider watching non-OS handles (lwIP socket indices) pairs with an event
backend that can poll them (`event_lwip`). Today that pairing is a matched set
selected together (the lwIP integration ships both). `KlEventOps.caps` /
`native_provider` + `kl_event_ctx_sockets_compatible` (async.c) already carry the
capability bits for a future negotiated pairing.

## Compatibility

`KlEventLoop` gains a trailing `ops` field and `keel/event.h` gains
`KlEventOps` / `KlEventProvider` / `kl_event_init_provider` — additive,
source + static-relink compatible (see `docs/compatibility.md`). Callers that
allocate a `KlEventLoop` on the stack and call `kl_event_init()` are unaffected
(it zeroes `ops`).
