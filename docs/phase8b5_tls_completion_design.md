# Phase 8b-5 — TLS over the completion loop (feed/drain BIO) — Design

**Status:** **implemented (8b-5a + 8b-5b).** The deepest 8b increment — the design's
flagged crux. Builds on 8b-0..4 (the platform-independent completion axis + driver).

- **8b-5a** — `KlTls` optional `feed_input`/`drain_output` ops + the mbedTLS memory-BIO
  mode. Validated by `tests/smoke_tls_completion.c` (a full mbedTLS handshake +
  bidirectional app data driven purely through the two ops, no socket) on POSIX/local
  mbedTLS. Only `include/keel/tls.h` changed — additive optional ops.
- **8b-5b** — the completion driver's TLS ciphertext leg. `kl_comp_post_recv` reads
  ciphertext into a transient op buffer for TLS conns (`read_buf` stays plaintext);
  `kl_comp_drain` hands it to the engine via the public `feed_input` op (no mbedTLS
  knowledge in the backend) and surfaces a plain READ. The driver (`comp_tls_drive`)
  reuses `kl_conn_on_handshake` / `comp_try_reading` / `kl_conn_ingest_body` verbatim,
  looping on `pending()` for coalesced records; responses are encrypted via the memory
  BIO (`comp_tls_send_response`) and flushed synchronously (same head-of-line caveat as
  streaming). `comp_on_accept` enters the handshake state + enables memory-BIO mode.
  Buffered HTTP/1.1 over TLS only; TLS file/stream bodies and ALPN-h2 are out of this
  subset (they close rather than mis-serve). **Validation:** the Windows IOCP build
  compiles clean under MinGW (`-Werror`); POSIX `make test` is byte-identical (the two
  completion TUs are not compiled there); the TLS-over-IOCP *runtime* is a local/hull
  gate per the BYO/out-of-CI mbedTLS decision below. Zero `include/keel/` change in
  8b-5b — the driver drives the abstract `KlTls` vtable, never mbedTLS.

**The problem.** `KlTls` does its socket I/O *internally*: `tls->read/write/handshake
(self, fd)` drive a BIO whose `recv`/`send` call synchronous `kl_sockdef_recv/send`
on the fd. On a completion loop there is no synchronous socket data — the driver owns
the I/O (`WSARecv`/`WSASend`). So the TLS engine must be fed the ciphertext the driver
received, and its outgoing ciphertext drained for the driver to send.

**Decision (with the user): the "feed me bytes" approach — true completion.** The
transport is *inverted*: the caller feeds received ciphertext and drains outgoing
ciphertext; the engine's BIO reads/writes those buffers instead of the socket.

---

## 1. Keeping it non-intrusive and orthogonal

Three axes, as for every 8b increment:

**Axis 1 — API: minimal, additive, optional (not a breaking change).** `read`,
`write`, `handshake`, `pending`, `shutdown` are **reused unchanged**. Only two
**optional, nullable** ops are added to the `KlTls` vtable — exactly the pattern
`alpn_protocol` and `peer_cert` already use (nullable ops a backend may omit):

```c
/* include/keel/tls.h — KlTls vtable, appended. Optional: NULL on a backend that only
 * supports the readiness/socket BIO. When both are present the transport is
 * caller-driven (completion mode); read/write/handshake then operate on these
 * buffers instead of the fd. */
int     (*feed_input)(KlTls *self, const void *cipher, size_t len);  /* ciphertext in  → engine */
ssize_t (*drain_output)(KlTls *self, void *cipher, size_t cap);      /* ciphertext out ← engine */
```

Existing behavior is **byte-identical**: a readiness loop never calls feed/drain
(the socket BIO path is untouched), and a backend that leaves them NULL works exactly
as today. This is the one deliberate, minimal API addition — additive and
non-breaking — that true-completion TLS requires; it is not a new required op, a
changed signature, or a struct-layout change to any existing type users allocate.

**Axis 2 — platform: IOCP is one implementation.** feed/drain and the memory-BIO
mode are **platform-independent** (in the mbedTLS backend + the completion driver);
`WSARecv`/`WSASend`/`OVERLAPPED` stay only in `event_iocp.c`. A future
io_uring/AIO backend reuses the same driver TLS path.

**Axis 3 — event axis: readiness untouched.** The socket-BIO path (readiness) is
unchanged; feed/drain are used **only** on a completion loop. The completion TLS
handling is contained to the completion driver + the backend's buffered BIO — never
the router or connection state machine (which stay model- and transport-blind: they
see plaintext either way).

**Axis 4 — TLS backend: any backend, via the vtable.** The completion driver uses
the **vtable ops** (`feed_input`/`drain_output`/`read`/`write`), never a backend's
internals — so it is not coupled to mbedTLS. mbedTLS is simply the first backend to
implement the optional ops (a memory BIO); a BYO backend that implements them works
over IOCP too.

---

## 2. mbedTLS: a buffered (memory) BIO mode

`tls_mbedtls.c` gains a completion mode selected the first time `feed_input` is
called (or via a flag set at handshake): its BIO callbacks read from a per-`KlTls`
**input ring** (fed ciphertext) and append to an **output ring** (ciphertext to
send), instead of `kl_sockdef_recv/send(fd)`:

- `feed_input(cipher, len)` — append to the input ring; a later `read`/`handshake`
  consumes it. `bio_recv` returns `MBEDTLS_ERR_SSL_WANT_READ` when the ring is empty
  (so `read`/`handshake` yield WANT_READ → the driver posts another `WSARecv`).
- `drain_output(buf, cap)` — copy pending output-ring bytes out (what `bio_send`
  buffered during `handshake`/`write`); the driver `WSASend`s them.

`handshake`/`read`/`write` are the **existing** functions — only the BIO's byte
source/sink changes. The readiness path keeps the socket BIO (`fd`), unchanged.

---

## 3. The completion driver's TLS path (platform-independent)

For a TLS connection (`c->tls != NULL`) the driver adds a ciphertext leg around the
existing plaintext core (all via vtable ops — no Win32, no mbedTLS symbol):

```
WSARecv ciphertext ─▶ tls->feed_input(ct)
   handshaking? ─▶ tls->handshake ─┬─ WANT_READ  ─▶ drain_output→WSASend? ─▶ post WSARecv
                                   ├─ WANT_WRITE ─▶ drain_output→WSASend  ─▶ post WSARecv
                                   └─ OK         ─▶ state=READING ─▶ post WSARecv
   established?  ─▶ tls->read(pt) ─▶ kl_conn_ingest… (the SHARED model-blind core)
response ready ─▶ tls->write(pt) ─▶ tls->drain_output(ct) ─▶ WSASend
```

The plaintext side is the *same* `kl_conn_dispatch_request` / `kl_conn_ingest_body`
core the plaintext and readiness paths use. TLS adds only the encrypt/decrypt leg,
driven through the vtable.

---

## 4. Staging

| Increment | What | Validated |
|---|---|---|
| **8b-5a** | `KlTls` optional `feed_input`/`drain_output` ops + the mbedTLS memory-BIO mode | POSIX + local mbedTLS: a buffered-BIO handshake/echo unit test (no IOCP) |
| **8b-5b** | the completion driver's TLS ciphertext leg + server default-provider TLS wiring + a TLS-over-IOCP smoke | local mbedTLS build; compile-gate on CI |

8b-5a is platform-neutral (the ops + BIO mode are testable on POSIX with mbedTLS).
8b-5b is the Windows-only driver leg.

---

## 5. The honest validation gap (and how it's handled)

**mbedTLS is bring-your-own and deliberately out of CI** (Phase 6 decision — the
mock TLS suites are what CI runs; the real mbedTLS backend is validated locally +
documented). So the **Windows-IOCP TLS runtime cannot be CI-gated** — there is no
mbedTLS on the Windows CI runner, exactly as there is none on the Linux ones.

Consequence, stated plainly: 8b-5's Windows runtime (a real TLS handshake over IOCP)
is **not** validated by CI. It is validated by (a) the platform-neutral 8b-5a
buffered-BIO unit test under a local mbedTLS build, (b) a MinGW `-Werror` compile-gate
of the driver TLS leg, and (c) a local/hull `make KEEL_TLS=mbedtls` TLS-over-IOCP
smoke run — the same out-of-CI validation posture the mbedTLS backend itself already
carries. This gap is inherent to TLS being BYO, not to this design; wiring mbedTLS
into CI (Windows + Linux) is a separate, larger decision.

---

## 6. Risks & stop conditions

- If true-completion TLS cannot be added without a **breaking** `KlTls` change (a
  changed existing signature or a required op) — as opposed to the additive optional
  ops here — **stop**: it becomes a separate completion-aware-TLS-transport phase.
- **Buffered-BIO record boundaries / backpressure** — the input ring must hold at
  least a full TLS record before `read` can progress; the output ring drains as the
  driver `WSASend`s. Bound the rings; treat overflow as a connection error.
- **HTTP/2-over-TLS-over-IOCP** (ALPN → h2) stays out of scope here (h2 rides the
  readiness path today); 8b-5 targets HTTP/1.1-over-TLS.
- **No CI runtime** (§5) — the accepted, documented cost of TLS being BYO.

## 7. Out of scope

- Wiring mbedTLS into CI (a separate decision that would let 8b-5 be CI-gated).
- HTTP/2 / WebSocket over TLS over IOCP.
- A non-mbedTLS backend's completion ops (the vtable supports them; providing one is
  future work).
