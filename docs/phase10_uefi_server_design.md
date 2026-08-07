# Phase 10 — UEFI HTTP(S) **server** on bare firmware (scoping plan)

**Status: S-1..S-6 done — serves `GET / → 200` (plaintext, S-4) and **HTTPS** (S-6) over
EFI_TCP4 on bare firmware (QEMU/OVMF). S-7 (teardown/audit/ship) remains.** The Phase 10
client (U-0..U-8) serves a hardened `KlClient`
(plaintext + HTTPS + DNS) on bare UEFI firmware. This doc scopes the *inbound* direction — a
`KlServer` that `bind`/`listen`/`accept`s over EFI_TCP4 and answers `GET / → 200` —
deliberately structured to preserve the three-axis separation that `/axis-audit` enforces.

Progress: S-1 (freestanding server archive) and S-2/S-3/S-5 (EFI_TCP4 passive open +
completion accept + mock-EFI lifetime suite) are merged. S-4 (this build) carved a
freestanding `kl_server_init` into `server_core.c`, added the self-contained server
archive + `s4_selftest.c`/`s4_link_stubs.c`/`build_s4.sh`/`run_s4.sh`, and the EFI image
links + **boots on real UEFI firmware** (QEMU/OVMF): `efi_main` runs, `kl_server_init`
executes freestanding and negotiates the socket provider. On a network-less OVMF it
degrades gracefully (`native_provider()` → NULL → `KL_ERR_SOCKET`, the documented path);
the boot-to-200 needs a network-enabled OVMF (Ubuntu's, via `run_s4.sh` in the container).
Remaining: S-6 (HTTPS), S-7 (teardown / EBS / audit / ship).

Companion: `docs/phase10_uefi_feasibility_design.md` (client), `docs/keel_axis_audit.md` (the axis
contract this plan must not violate).

---

## 1. Why the architecture already does most of the work

The axis-audit north star is: **event model, socket/platform implementation, and protocol layer are
orthogonal; protocols sit above both axes and never touch platform socket/event internals.** The
server path was built model-blind for io_uring / IOCP / pollcomp / lwip-raw, and that pays off here:

- **Protocol axis — UNCHANGED.** `server.c` / `connection.c` (`conn_internal.h` model-blind core) /
  router / `response.c` already drive a server over *any* completion backend. `KL_COMP_ACCEPT`
  (target `KlConn*`) is a first-class completion event (`src/completion.h`), and
  `completion_server.c:comp_on_accept` is the generic accept→`KlConn` path (pool acquire, convert the
  backend's **neutral** `KlSockAddr` peer, refill the accept). `completion_core.c:kl_comp_run` routes
  ACCEPT/READ/WRITE there **without a static reference**, so a client-only build links none of it.
- **Socket axis — a hole to fill.** The EFI_TCP4 provider is intentionally client-only today:
  `efi_sock_bind`/`listen`/`accept` return failure / `KL_INVALID_SOCKET` with `EFI_UNSUPPORTED`
  (`socket_efi_tcp4.c:920-930`). EFI_TCP4 itself supports passive open (`ActiveFlag=FALSE`) +
  `Accept(ListenToken)` (`spikes/uefi/efi_tcp4.h`), so the firmware is capable — the vtable ops are
  just stubs.
- **Completion axis — one op to add.** `event_efi.c` implements `post_connect`/drain/cancel for the
  outbound path; it needs the **inbound counterpart `post_accept`** (the completion contract already
  names it) that arms EFI Accept tokens and surfaces each accepted child as `KL_COMP_ACCEPT`.
- **Build surface — a new archive.** `libkeel_freestanding.a` is a **client-only, completion-only**
  source set by construction (Makefile §"Freestanding client archive"; `server.h` deliberately
  excluded). A server needs a sibling **freestanding *server* archive**.

**Design invariant (the acceptance bar for every phase below): the protocol TUs stay byte-identical
to the POSIX build.** No `#include` of `efi_*.h`, no EFI calls, no token logic above the socket/
completion seam. `grep` of the server protocol TUs for EFI/event symbols must return nothing — the
same mechanical check the client passed (axis-audit Goal 4).

---

## 2. What's net-new, mapped to the three axes

| Axis | Net-new work | Reused unchanged |
|------|--------------|------------------|
| **Protocol** | *(nothing)* | `server.c`, `connection.c`, router, `response.c`, body readers, `completion_server.c` |
| **Socket** (`socket_efi_tcp4.c`) | `bind`/`listen`/`accept` = passive `Configure` + an Accept-**token pool** + child-per-connection; neutral peer `KlSockAddr` | `send`/`recv` (U-6 non-blocking), `close` (quarantine + generation guard), status mapping |
| **Completion** (`event_efi.c`) | `post_accept` (arm Accept tokens) + drain emitting `KL_COMP_ACCEPT`; accept-token teardown | drain loop, `KlCompletionOps` wiring, `native_provider`, EBS fail-closed |
| **Build** | `libkeel_freestanding_server.a` (+ symbol-closure gate); a `KlUdpServer`? (no — HTTP only) | the client archive machinery |
| **TLS** (HTTPS server) | server-side completion-TLS: memory-BIO `feed_input`/`drain_output` + a sync send on the accepted socket | the mbedTLS adapter, `entropy_uefi`, `clock_snapshot` (U-8) |

---

## 3. Phased plan (mirrors the client's U-0..U-8 cadence)

Each phase is independently QEMU/OVMF-verifiable and **must keep the protocol TUs unchanged**.

### S-0 — Feasibility spike (raw EFI_TCP4 passive open)
Prove, in `spikes/uefi/`, that a raw EFI_TCP4 child with `ActiveFlag=FALSE` + `Configure(passive)` +
`Accept(&listenToken)` yields a *new child protocol handle* per inbound connection, that we can
`Receive`/`Transmit` on it, and close it. Deliverable: go/no-go + the six lifecycle findings (mirror
of U-0), esp. **the accepted child is a distinct EFI_HANDLE with its own token set** (drives the
socket-axis object model).
*Axis:* socket only. *Goal 2* (completion semantics: Accept = construct→submit→completion→new handle).

### S-1 — Freestanding **server** archive
Add `libkeel_freestanding_server.a`: the client sources **plus** `server.c` / `connection.c` /
`completion_server.c` / router / `response.c` / body readers, still completion-only, no OS sockets.
Extend `make freestanding-headers` + the undefined-symbol whitelist gate to the server surface
(`server.h` freestanding-clean: no `off_t`/errno/rawio leakage — audit the same way the client
headers were). Multi-arch (x86_64 + aarch64) PE-link gate.
*Axis:* build/protocol packaging. *Goal 4* (protocols above axes — proven by the symbol gate).

### S-2 — Socket axis: `bind`/`listen`/`accept`
Implement the three vtable ops on `socket_efi_tcp4.c`:
- `bind` → record the local addr/port on the conn (Configure input).
- `listen` → `Configure(passive, StationPort)` + prime an **Accept-token pool** (N outstanding
  `Accept` tokens, like a listen backlog; N = backlog clamped to a cap).
- `accept` → non-blocking: return the next completed child as a fresh `KlUefiConn` (generation++,
  its own token set), or `KL_INVALID_SOCKET`+would-block when none ready.
Reuse the **exact token-lifetime discipline** from the client: stable provider-owned slots, generation
guard on the child handle, and **quarantine on a failed Accept-token cancel** (never free a token the
firmware may still write). Peer address returned as a **neutral `KlSockAddr`** (Goal 5).
*Axis:* socket. *Goals 5, 6* (neutral accept addr; op/handle ownership + lifetime).

### S-3 — Completion axis: `post_accept` + `KL_COMP_ACCEPT`
Add `post_accept` to the EFI `KlCompletionOps`: arm/refill the Accept-token pool; the drain
pumps them and emits one `KL_COMP_ACCEPT` per accepted child (`accepted_fd` = the new conn handle,
peer already neutralized). `completion_server.c:comp_on_accept` then does the rest **unchanged**
(pool acquire, arm first read, refill). Wire `native_provider` so a passive socket is created on
demand. **Backpressure:** when the `KlConn` pool is full, stop re-priming Accept tokens (don't accept
into a drop) — the Keel-level equivalent of reducing readiness interest (Goal 8).
*Axis:* completion. *Goals 2, 3, 8* (honest completion accept; no model leak upward; backpressure).

### S-4 — **Plaintext** HTTP server GO — *COMPLETE (serves GET / → 200 on QEMU/OVMF)*
`s4_selftest.c`: stock freestanding `KlServer` on the EFI completion backend answers
`GET / → 200` to a host client (QEMU SLIRP `hostfwd`), over real EFI_TCP4. Acceptance = the
model-blind server core runs with **zero** protocol edits (the whole point). Prove accept →
serve → keep-alive → close, and accept-burst (pool refill).
*Axis:* all three, integrated. *Required trace* (axis-audit): accept path end-to-end + close-with-
outstanding-accept.

**Done in this build:** the S-4 prerequisite was carving a freestanding `kl_server_init`
into `server_core.c` (the archive had the completion run loop but init was hosted-only —
it pulled the ws/h2/proxy hook installers, the PROXY CIDR allowlist, `kl_file_io_create`,
the stop self-pipe, and the stdlib default allocator; all now `#ifndef KEEL_FREESTANDING`).
Behavior-preserving on hosted (65 test suites, gcc-14 clean); the self-contained server
archive gates for x86_64 + aarch64 PE **with init in it**. `s4_selftest.c` constructs the
server from the archive alone (`cfg.event_provider` set, `cfg.sockets` NULL → adopts the
native EFI provider), routes `GET /`, drives the socket seam for `bind`/`listen` (the hosted
`kl_server_bind_listener` is archive-excluded), then loops `kl_server_run_completion_loop`
(auto-primes accepts → drains `KL_COMP_ACCEPT` → `comp_on_accept`, all model-blind).
`s4_link_stubs.c` supplies the server-only link residuals (bind/listen/reuseaddr/writev/
sendfile/cork/nodelay/accept fallbacks, file-response platform hooks, `_fltused`). The EFI
image links (198 KB PE32+) and **boots on QEMU/OVMF**: `efi_main` runs, `kl_server_init`
executes freestanding + negotiates the provider — validating the carve on bare firmware.
**Network-OVMF boot result (Ubuntu 24.04 container, `run_s4.sh`, `OVMF_CODE_4M.fd`):**
`kl_server_init` **succeeds** and `bind`/`listen` **succeed** ("server up" -> "listening on
:80") — the freestanding `KlServer` construction + EFI_TCP4 passive open both work on real
network firmware. The first boot hit a firmware `#UD` (RIP -> `0xB0000`) on serve. **Root cause:** the EFI
`KlCompletionOps` (`event_efi.c` `EFI_COMP_OPS`) left `.post_recv`/`.post_send` **NULL** — the
*client* rides the `KL_COMP_WATCHER` drain-relay + sync socket provider and never calls them,
but the *server* completion driver (`completion_server.c`) calls `kl_comp_post_recv`/`post_send`
directly, so post-accept it called a NULL op. **Fixed** by implementing EFI `post_recv`/`post_send`
mirroring `event_pollcomp.c`: the op is queued; `el_drain`, on the U-6 non-blocking read-readiness
probe, does `efi_sock_recv` into `conn->read_buf` and emits `KL_COMP_READ`; sends flatten the
response iovec into a private buffer and `efi_sock_send` it in bounded Transmit fragments, emitting
one `KL_COMP_WRITE` (generation-stale-guarded; freed on completion/cancel). **Result: PASS** —
`server up → listening on :80 → GO`, `curl` returns **200** with body
`hello from KEEL on UEFI (EFI_TCP4 server)`. `post_sendfile` stays NULL (file responses are S-6).

(This run's homebrew OVMF on the macOS host has no EFI_TCP4 stack -> `kl_server_init` returns
`KL_ERR_SOCKET`, the documented graceful-degradation path; the network boot needs Ubuntu's
OVMF.) `kl_server_free`/teardown stays hosted-only — S-7.

### S-5 — Lifetime hardening + mock-EFI accept tests
Extend the F7b host mock (`mock_efi_test.c`) with the **server** lifetime scenarios the QEMU happy
path can't expose — the load-bearing acceptance, mirroring the client work:
- Accept-token hang → cancel succeeds / **fails → quarantine** (never free a live listen token).
- Close a listener with N outstanding Accept tokens → all drained/quarantined before teardown.
- Accepted-child close-while-recv-outstanding (already covered for client conns; re-assert per-child).
- Post-EBS: `accept`/`listen` refuse (fail-closed), zero firmware calls.
- Pool-full backpressure: Accept tokens stop being re-primed; no accept-into-drop.
- Stale generation on a reused accepted-child slot.
*Axis:* socket + completion lifetime. *Goals 6, 8, 10, 13.* ASan+UBSan; `detect_leaks` per-OS.

### S-6 — **HTTPS** server (completion-mode TLS) — *COMPLETE (serves HTTPS GET / → 200 on QEMU/OVMF)*
The hard TLS lift landed with a **remarkably small** backend delta — the completion-mode TLS
server is entirely in the model-blind core (`completion_server.c`: `comp_tls_drive` /
`kl_comp_tls_flush` / `comp_tls_send_response`), so the EFI backend supplied only the two
[[keel-completion-tls-backend-contract]] obligations:
1. **post_recv feeds ciphertext** (`event_efi.c`): a TLS conn's socket carries ciphertext, so the
   recv reads it into a transient scratch buffer and calls `tls->feed_input`; `comp_tls_drive` then
   handshakes/decrypts into `read_buf`. (The plaintext S-4 recv path is unchanged — one `if (c->tls)`.)
2. **synchronous send** on the accepted socket for `kl_comp_tls_flush` — already satisfied by
   `efi_sock_send`; the response ciphertext rides the same S-4 `post_send` (content-agnostic).

The one config fix: **`MBEDTLS_SSL_SRV_C`** was missing from `mbedtls_config_uefi.h` (the U-4 client
config), so the server ctx parsed but its handshake never progressed — enabling it (additive; the
client is unaffected) fixed the stall. `s6_selftest.c` builds the server ctx from an embedded PEM
cert+key (`kl_tls_mbedtls_ctx_create_from_buf`), binds `:443`; `build_s6.sh`/`run_s6.sh` mirror the
U-4 mbedTLS recipe against the server archive. **Result: PASS** — `server up → listening on :443 →
GO`, host `curl -k` returns **200** with body `hello from KEEL on UEFI (HTTPS over EFI_TCP4)`.
(Spike entropy = the insecure weak fallback, no virtio-rng; a production build requires real EFI_RNG
fail-closed, per U-4. `post_sendfile` still NULL — file responses are a later step.)
*Axis:* completion + TLS-vtable — no vtable change needed; the client's `feed_input`/`drain_output`
memory-BIO ops served the server verbatim.

### S-7 — Docs, audit passes, EBS lifecycle, ship
`kl_uefi_shutdown()` also tears down the listener + Accept-token pool (order: children → listener →
providers → platform). Fresh `/c-audit` + `/axis-audit` passes (the axis pass should record that the
server **validates the audit's "future provider: UEFI SNP + polling" claim** — Goal 14). Compatibility
matrix row: `EFI_TCP4 server + EFI completion backend`.

---

## 4. Token-lifetime discipline (carry the client's hard-won rules forward)

The server adds two new token classes; both obey the client's proven rules verbatim:
- **Accept tokens** (listener-owned pool): every armed token reaches exactly one terminal state
  (completion → new child, or Cancel→drain-to-`EFI_ABORTED`) before its storage is reused. On a
  failed cancel → **quarantine** the listener slot (leak to EBS), never `CloseEvent`/`DestroyChild`.
- **Per-child tokens**: identical to client conns (`conn_posted`/`rx_posted`/`tx_posted`/`close_posted`,
  generation guard, quarantine, `KL_EFI_RXBUF` bound). The accepted child IS a `KlUefiConn`, so it
  reuses `efi_sock_send`/`recv`/`close` unchanged.
Stable provider-owned storage for the Accept-token pool (a `static` listener struct), so a firmware
write into a hung Accept token after teardown lands in valid memory (the DNS-token lesson).

---

## 5. Risks / open questions (resolve during S-0..S-2)

1. **Accepted-child object model.** EFI_TCP4 `Accept` returns a *new child handle* on the listen
   token — confirm it's a full `EFI_TCP4_PROTOCOL` we `OpenProtocol` per connection (S-0), and that
   closing a child doesn't disturb the listener.
2. **Backlog semantics.** How many concurrent Accept tokens to keep armed vs the `KlConn` pool size;
   how to throttle without dropping (Goal 8).
3. **Single-listener vs SO_REUSEPORT.** UEFI is single-threaded pre-boot — one listener, one loop
   (fine; horizontal scaling is not a pre-boot concern).
4. **Freestanding server surface.** Does `server.c`/`connection.c` pull any hosted type
   (`off_t` in `response.h`/`file_io.h`, sendfile/splice) that must be neutralized first? (The client
   phase already flagged `off_t` as "the next hosted type" — S-1 must close it.)
5. **Completion-mode server TLS** is the biggest unknown (S-6) — the sync-send-on-accepted-socket
   obligation; de-risk by reproducing natively on pollcomp first.
6. **No `KlUdpServer`/QUIC** in scope — HTTP/1.1 over EFI_TCP4 only.

---

## 6. Acceptance (the axis contract, restated as gates)

A phase merges only if:
- Protocol TUs are byte-identical to POSIX (`grep` for `efi_*`/event symbols in the server core = ∅).
- The completion accept path is **traced** end-to-end (readiness has no analogue here — EFI is
  completion-only — so the pollcomp server is the cross-backend equivalence check).
- New token classes pass the **host mock-EFI** lifetime suite (hang/cancel-fail→quarantine/post-EBS/
  backpressure) under ASan+UBSan — QEMU happy-path GO is necessary but **not** sufficient (the
  standing lesson from the client hardening).
- QEMU/OVMF serves the real request (`GET / → 200`, then HTTPS at S-6).

**Bottom line:** the server is a socket-axis + completion-axis extension on an already-model-blind
protocol + server-driver core. If any phase finds itself editing `server.c`/`connection.c` to make
EFI work, that is an axis violation and the design is wrong — the fix belongs below the seam.
