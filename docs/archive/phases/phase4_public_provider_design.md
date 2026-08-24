# Phase 4: Public socket-provider selection, Design

Status: **ready to implement (2026-07-26).** Phase 6 (Winsock) is complete: a real
non-POSIX provider (`kl_socket_provider_winsock`) drives Keel's server and client
over WSAPoll, the mbedTLS backend runs through the same seam, and 47/55 test
suites run on the Windows runner. That was the "proven by a real alternative
provider" bar Phase 4 was gated on (`pal_transformation_design.md`,
`phase6_winsock_design.md` Part E). The vtable shape is now settled, so the public
surface can be **frozen once, correctly.** This revises the 2026-07-20 design
(which deferred implementation until Phase 6).

Phase 4 makes the socket provider **user-selectable and user-authorable**; today
`KlSocketProvider`, `KlSocketOps`, `kl_socket_provider_posix()`, and the capability
helpers live in the internal `src/socket.h` (not installed), so an embedder can
inject a provider only from inside the tree.

---

## 1. Freeze risks from the 2026-07-20 design: all resolved by Phase 6

The prior design deferred going public to avoid a breaking re-freeze on three
fronts. Phase 6 resolved all three; the vtable is implemented by **three** real
providers (POSIX, Winsock) and exercised by a real transport (mbedTLS through the
seam):

1. **Handle type: RESOLVED.** `KlSocketHandle` (`intptr_t`, pointer-width) shipped
   in Phase 5 and is used across the whole vtable. It holds a POSIX `int`, a
   Winsock `SOCKET`, and a future pointer handle (lwIP raw `tcp_pcb *`). No `int fd`
   in the ops. `KL_INVALID_SOCKET` + `kl_handle_valid()` are the validity contract.
2. **writev / sendfile ops: RESOLVED.** Both are real vtable ops
   (`ops->writev`/`ops->sendfile`), capability-gated (`KL_SOCK_CAP_WRITEV/SENDFILE`);
   Winsock implements them via `WSASend`/`TransmitFile`, POSIX via the syscalls, and
   a provider lacking the cap gets the serialize / pread-send fallback.
3. **accept/connect shape: RESOLVED.** Readiness-style `connect`/`accept` ops
   proved sufficient for both POSIX and Winsock (WSAPoll readiness + `getsockopt(SO_ERROR)`
   completion). `AcceptEx`/`ConnectEx`/IOCP is a **completion model** = the separate
   Phase 8 event axis, explicitly out of scope here (the event/socket decoupling
   invariant in `pal_review.md` F3 keeps it addable later without touching this API).

**Interim already shipped:** `KlEventCtx.sockets` is a public *opaque* field
(`const struct KlSocketProvider *`), and the server already routes **every** socket
op, including the listen-socket create/bind, through `s->ev.sockets`. So the
internal plumbing is complete; Phase 4 only exposes the type and adds the config
entry points.

---

## 2. Decision (locked): no internal POSIX types on the public authoring API

**Directive:** the installed provider API must not leak Keel's internally-shimmed
POSIX-only types. Those are the types that don't exist natively on all targets and
that `src/sockcompat.h` papers over today; `struct iovec`, `ssize_t`, `off_t`.
Exposing them in an installed header would bake POSIX-shaped types into the public
ABI and force a public `iovec`/`ssize_t` fallback on Winsock (which has neither).

**Resolution: Keel-owned types in the public vtable:**

| Internal (today) | Public vtable type | Notes |
|---|---|---|
| `struct iovec` | `KlIoVec { void *base; size_t len; }` | Keel-owned; providers translate → `iovec` (POSIX) / `WSABUF` (Winsock) *inside their TU*. |
| `ssize_t` (return) | `kl_ssize_t` = `intptr_t` | Same width as `KlSocketHandle`; signed, pointer-width. |
| `off_t` (sendfile offset) | `uint64_t` | Fixed-width, platform-neutral file offset. |
| `KlSocketHandle` | `KlSocketHandle` (already public) | Opaque `intptr_t`; validity via `kl_handle_valid()`, never `< 0`. |

**`struct sockaddr` / `socklen_t` are kept**: deliberately, and they are *not*
"internal POSIX types": they are the standard BSD-sockets address ABI provided by
the OS on **both** POSIX and Winsock, resolved through the sanctioned public
`keel/net.h` boundary (Keel does not shim them). A full Keel-owned address type
(`KlSockAddr`) is the deferred **F4** item (`pal_review.md`), explicitly out of
scope here. So the public authoring API speaks `KlSocketHandle` + `KlIoVec` +
`kl_ssize_t` + `uint64_t` offsets, with `sockaddr`/`socklen_t` via `net.h` as the
one standard-ABI dependency, documented and F4-bounded.

**Consequence for the seam (implementation):** `KlIoVec` becomes the seam currency
end to end; `response.c` builds `KlIoVec[]` (not `struct iovec[]`), the seam op is
`writev(ctx, fd, const KlIoVec *, int)`, and `struct iovec` / `WSABUF` never appear
outside `socket_posix.c` / `socket_winsock.c`. This *tightens* the existing
architecture (no platform I/O-vector type outside a provider TU), not just the
public surface.

---

## 3. Public surface

### 3.1 New installed header `include/keel/socket.h`
The stable authoring subset of `src/socket.h`:
- `KlSocketHandle` contract (re-export from `keel/handle.h`, already installed).
- `KlSocketOps`; the ops table (final shape, per §2): lifecycle
  (`socket`/`connect`/`bind`/`listen`/`accept`/`close`), I/O (`send`/`recv`/
  `recv_peek`/`writev`/`sendfile`), options (`set_nonblocking`/`set_blocking`/
  `set_cloexec`/`set_nosigpipe`/`set_reuseaddr`/`set_reuseport`/`set_ipv6only`/
  `set_tcp_nodelay`/`set_cork`), introspection (`get_local_addr`/`get_so_error`),
  `destroy`, and `name`.
- `KlSocketProvider { const KlSocketOps *ops; void *context; uint64_t capabilities; }`.
- Capability flags: `KL_SOCK_CAP_NATIVE_FD`, `KL_SOCK_CAP_WRITEV`,
  `KL_SOCK_CAP_SENDFILE`; now a **public contract** (a provider advertises what it
  supports; Keel falls back otherwise).
- Built-in factory: `const KlSocketProvider *kl_socket_provider_posix(void);`
  (and `kl_socket_provider_winsock(void)` on Windows).
- Query helpers: `kl_socket_provider_has_cap()`, `kl_sock_native_fd()`,
  `kl_socket_provider_destroy()`.
- `KlError kl_sock_errno_to_error(int)`; the coarse errno→KlError mapping (§5).

**Stays internal** (`src/socket.h`, not installed): the inline `kl_sock_send/recv/
connect/…` wrappers and the `kl_sockdef_*` platform defaults. Those are Keel's
*consumers* of a provider, not part of the authoring API; a custom provider
implements `KlSocketOps`; it never calls the wrappers.

### 3.2 `event_ctx.h`: de-opaque the field
`KlEventCtx.sockets` already exists as `const struct KlSocketProvider *`. Once
`keel/socket.h` is installed and included by `event_ctx.h`, the type is nameable
and users can assign `ctx.sockets = kl_socket_provider_posix()` (or a custom one).

## 4. Selection wiring

### 4.1 Server: `KlConfig.sockets`
Add `const KlSocketProvider *sockets;` to `KlConfig` (default NULL = POSIX
default). `kl_server_init` copies it to `s->ev.sockets` **before** `bind` (already
the code path; the listen socket is created through `s->ev.sockets`). Add the
**native-fd guard**, since the event loop can only poll a real OS descriptor:

```c
if (cfg->sockets && !kl_socket_provider_has_cap(cfg->sockets, KL_SOCK_CAP_NATIVE_FD)) {
    s->last_error = KL_ERR_SOCKET;   /* readiness loop needs a native fd */
    return -1;                        /* explicit unsupported-combination error */
}
```
(A non-native provider, e.g. a future raw-lwIP handle, is a Phase 8/9 completion-
axis concern, correctly rejected by the readiness server here.)

### 4.2 Client
The async client takes a user-owned `KlEventCtx`; the user sets `ctx.sockets`
before creating the client; no new field strictly needed. **Add a convenience
`KlClientConfig.sockets`** (default NULL) that the client copies into its ctx, for
symmetry with the server and the sync client (which owns its ctx internally).
Document both paths in the client guide.

## 5. Error taxonomy (coarse, public)
`kl_sock_errno_to_error` stays mapped to the existing `KlError` network codes
(`SOCKET`/`BIND`/`LISTEN`/`CONNECT`/`IO`/`TIMEOUT`/`INVALID_ARG`/`ALLOC`). No new
public enum values in Phase 4: `KlError` is already rich, and it is the portable
error surface (the Winsock seam already maps WSA codes → errno → `KlError`, and
`KlPlatformCap`/`kl_platform_caps()` from the PAL review is the precedent for
capability discovery). Finer would-block/reset/unreachable categories are an
additive follow-up if a consumer needs them.

## 6. Compile-time / linking model
Providers are plain structs the embedder supplies. "Link only one provider" is
automatic; POSIX/Winsock are the built-ins (Makefile-selected), custom ones are
user code. Compile-time default is NULL (built-in). **No registry, no global
mutable state, no runtime plugin loading**; out of scope.

## 7. Deliverables
- `include/keel/socket.h` (installed) with the frozen vtable per §2/§3, defining
  `KlIoVec` + `kl_ssize_t` and using `uint64_t` sendfile offsets; `event_ctx.h`
  includes it (de-opaque the field).
- **Seam retype to `KlIoVec`**: the `writev` op signature, `response.c`'s
  scatter-gather assembly, and the `kl_sockdef_writev`/provider impls move from
  `struct iovec` to `KlIoVec`; `struct iovec`/`WSABUF` are confined to
  `socket_posix.c`/`socket_winsock.c`. Verify the response fast-path bench is flat.
- `KlConfig.sockets` + pre-bind native-fd guard; `KlClientConfig.sockets`.
- Internal `src/socket.h` reduced to the consumer wrappers + `kl_sockdef_*`,
  including the public header for the shared types.
- **Example** `examples/custom_socket_provider.c`; a decorator over POSIX that
  counts bytes / logs (mirrors `custom_allocator`), proving the authoring API end
  to end.
- **Docs:** a "selecting a socket provider" section in the server + client guides,
  the capability contract, the native-fd requirement, and the KlIoVec/handle types.
- **Tests:** server + client conformance over `kl_socket_provider_posix()` selected
  via the public config; the native-fd guard rejection; the decorator example
  exercised; install-tree compile test (a program using only installed headers).
- **Install:** add `keel/socket.h` to `make install` + the pkg-config header set.

## 8. Validation target (post-Phase-4)
The first external validation of the *public* API should be the **lwIP socket-API
provider** (a descriptor-based, readiness-model provider); it exercises the public
authoring surface without needing the Phase 8/9 completion axis, and confirms the
frozen vtable is sufficient for a genuinely different stack. If lwIP-socket needs a
vtable change, better to learn it against the fresh public API than after adoption.

## 9. Stop conditions / risks
- **Public-type leakage** (§2) → **decided: no internal POSIX types on the public
  API.** The installed header ships `KlIoVec`/`kl_ssize_t`/`uint64_t` offsets, never
  `struct iovec`/`ssize_t`/`off_t`. Freeze the header once with these.
- **Address abstraction** (F4) → `struct sockaddr`/`socklen_t` stay via `net.h` (the
  standard cross-platform sockets ABI, not an internal shim); a full `KlSockAddr`
  and a non-BSD provider (UEFI) would reopen this. Documented, deferred.
- **Scope creep** → no provider registry, no runtime loading, no finer error enum,
  no completion-model ops. All additive future work.

## 10. Out of scope
Portable address types (F4/Phase 5-extension); IOCP/completion ops (Phase 8); the
lwIP raw provider (Phase 9); UEFI (Phase 10); finer public error codes.
