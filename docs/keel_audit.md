# C Audit Report: KEEL

## Third pass — Windows/Winsock PAL surface (2026-07-25)

**Scope:** the Windows platform TUs that landed with PAL Phase 6 (the Winsock
port) — `src/socket_winsock.c`, `src/dns_sys_win.c`, `src/udp_io_win.c`,
`src/event_wsapoll.c`, `src/platform_win.c`, `src/server_plat_win.c`, and the
compatibility-boundary headers `src/sockcompat.h` / `src/socket.h` /
`src/dns_sys.h` / `src/platform.h` (Windows branches).
**Method:** three parallel source-level auditors (these TUs are Windows-only and
cannot be compiled on the macOS host — no MinGW), each cross-checking the Windows
implementation against its POSIX sibling's contract: (1) `udp_io_win.c` cmsg /
WSARecvMsg / batching; (2) `event_wsapoll.c` + `platform_win.c` +
`server_plat_win.c`; (3) the header shims + re-verification of the two fixes that
were already in the working tree.

**Issues found: 3** (Critical: 0, High: 0, **Medium: 3**, Low: several/Doc) —
**all three Medium fixed.** The two fixes already present in the working tree
(`socket_winsock.c` errno translation on the seam ops + writev fail-loud;
`dns_sys_win.c` hosts-path truncation) were re-verified **correct and complete.**

> ⚠️ These fixes touch Windows-only TUs and were **not compiled locally** (no
> MinGW on the audit host). They must go green on the Windows CI job before merge.
> The POSIX build + full test suite remain green (the only shared header edit,
> `sockcompat.h`, is inside the `#if defined(_WIN32)` branch).

### M1 — `event_wsapoll.c`: `kl_event_wait` returned `-1` without setting `errno`
**`src/event_wsapoll.c` (`kl_event_wait`)** — *fixed.*

On `WSAPoll` → `SOCKET_ERROR` the backend returned `-1` without touching `errno`
(Winsock reports via `WSAGetLastError()` and never sets the CRT `errno`). The
server accept loop (`server.c:582`) branches on `errno == EINTR` to decide
retry-vs-abort and then feeds `errno` to `kl_log_errno`; on a genuine poll error
it would read a **stale** `errno` — spuriously `continue`-looping if the stale
value happened to be `EINTR`, or logging a bogus reason otherwise. **Fix:**
translate via the now-shared `kl_wsa_set_errno()` before returning `-1`.

### M2 — `platform_win.c`: `kl_plat_poll1` returned `-1` without setting `errno`
**`src/platform_win.c` (`kl_plat_poll1`)** — *fixed.*

Same root cause as M1 on the sync-client single-fd poll wrapper: returned
`WSAPoll`'s `-1` verbatim with no `errno`. **Fix:** `kl_wsa_set_errno()` on
`SOCKET_ERROR`, return `-1` (success value passed through unchanged).

*Shared-helper refactor for M1/M2:* `wsa_set_errno()` was file-static in
`socket_winsock.c`; promoted to a non-static `kl_wsa_set_errno()` declared in
`sockcompat.h` (Windows branch) so the event backend and `poll1` translate
identically to the socket seam ops. No behavior change on the existing 14 seam
call sites.

### M3 — `udp_io_win.c`: truncated-datagram path parsed an indeterminate cmsg buffer
**`src/udp_io_win.c` (`kl_udp_io_recv_drain`)** — *fixed.*

On the `WSAEMSGSIZE` (truncated) return of `WSARecvMsg`, `msg.Control.buf/len`
are **not** reliably populated, yet the code fell through to
`udp_parse_local`/`udp_parse_tos` with `Control.len` still at the full buffer
size and the control buffer never zero-initialized. The additive
`cmsg_len >= WSA_CMSG_LEN(...)` checks prevent any out-of-bounds read, but the
delivered `recv_local` / `recv_tos_val` could be derived from indeterminate stack
bytes for a truncated datagram. **Fix:** a `have_control` flag set only on the
successful recv path now gates the cmsg parse, so truncated datagrams report no
local-addr/TOS (correct — that control data is unreliable).

### Low / Doc (noted, not fixed)
- **`event_wsapoll.c`** — empty pollset with an *infinite* timeout (`count==0 &&
  timeout_ms<0`) returns 0 immediately rather than blocking (POSIX `poll(...,-1)`
  blocks). Not reachable on the server path (the listen socket is always
  registered and the server passes a finite computed timeout). Low.
- **`platform_win.c`** — `kl_plat_random` casts `len` to `ULONG` for
  `BCryptGenRandom`; a `len > 4 GiB` would truncate. Callers use tiny buffers
  (mask keys, DNS txn-ids); not exploitable. The RNG return **is** checked and
  the buffer is never left uninitialized. Doc/Low.
- **Best-effort non-block failures** — `ioctlsocket(FIONBIO)` on the wakeup
  socket-pair read end ignores failure, matching the POSIX sibling's lenient
  `fcntl(O_NONBLOCK)` contract. Low, symmetry only.
- **`sockcompat.h`** — the `WSAE* → E*` mapping depends on the socket `E*`
  constants existing in `<errno.h>`; MinGW-w64 (the Makefile target) provides
  them but MSVC's `<errno.h>` historically does not. Worth a one-line "MinGW-w64
  required" note. Doc.
- **`kl_sockdef_close`** — returns `closesocket()`'s value without
  `kl_wsa_set_errno()`; no caller branches on errno after close. Doc.

### Verified sound (Windows PAL)
- **`socket_winsock.c`** — every seam op a caller tests for
  `EWOULDBLOCK`/`EINPROGRESS`/`EINTR` sets `errno` on `-1`; `connect` overrides
  `WSAEWOULDBLOCK→EINPROGRESS`; the tuning knobs that intentionally skip errno
  have no errno-inspecting caller; the writev `EINVAL`-on-`iovcnt>16` guard fails
  loud and can never overrun `stackbufs[16]` (real callers use `iov[7]`); the
  translation switch covers all tested codes.
- **`dns_sys_win.c`** — the hosts-path fix's `memcpy`s are bounds-safe (full
  default is 40 B into a `MAX_PATH`=260 buffer); build-once `static` buffer
  lifetime sound under the single-threaded loop.
- **`udp_io_win.c`** — TX/RX cmsg buffer sizing exact (`WSA_CMSG_SPACE`
  additive, no subtraction/underflow); all received-cmsg lengths additively
  checked; no dynamic alloc in the TU; matched `kl_free` size on the send queue;
  `kl_handle_valid` (not `<0`) re-check; `WSAEMSGSIZE` translated. The GSO-unsup
  `EIO` return correctly trips `udp.c:434`'s `!= EAGAIN/EWOULDBLOCK` predicate
  (no retry loop).
- **`platform_win.c`** — `kl_monotonic_ms` sec/rem decomposition avoids
  `ctr*1000` overflow, divide-by-zero guarded; socket-pair emulation closes all
  fds on every error path with no double-close; `kl_plat_file_pread` clamps
  `count > INT_MAX`.
- **`server_plat_win.c`** — `kl_srv_bind_unix` bounds `sun_path` before the
  `+1`-NUL `memcpy`; bind-failure closes + resets the fd; atomic console-ctrl
  handler registration sound.
- **`event_wsapoll.c`** — `grow_arrays` `INT_MAX/2` overflow guard + `size_t`
  math + fds-shrink-back on udata realloc failure; full unwind in init; matched
  free sizes in close.
- **Header shims** (`sockcompat.h`/`socket.h`/`dns_sys.h`/`platform.h`) — `ssize_t`
  = `intptr_t` (pointer-width); all handle-carrying ops use `KlSocketHandle`
  (intptr_t) with `KL_INVALID_SOCKET` — no `SOCKET→int` truncation; no
  identifier-hijacking macros; every inline wrapper NULL-guards + falls back to
  `kl_sockdef_*`.

---

## Second pass — UDP feature surface (2026-07-19)

**Date:** 2026-07-19 (second pass — UDP feature surface)
**Scope:** the UDP stack that landed since the first pass — `src/udp.c` (roughly
doubled: multicast, `recvmmsg`/`sendmmsg` batching, GSO/GRO offload, ECN/TOS/DSCP
marking, and a send-path cmsg refactor), `src/udp_server.c`, a re-audit of the
`src/dns_resolver.c` untrusted-input parsers, and a full tooling/hardening sweep.
**Method:** three parallel auditors — (1) deep `udp.c` memory-safety/bounds,
(2) `udp_server.c` + DNS parser re-check, (3) automated tooling.

**Issues found: 2** (Critical: 0, High: 0, Medium: 0, Low: 1, Doc: 1) — **both fixed.**

The UDP surface came through clean. The auditors confirmed the hard parts are
sound: RX/TX control-message buffer sizing (`UDP_RX_CMSG_SPACE` fits pktinfo +
UDP_GRO + TOS simultaneously; `UDP_TX_CMSG_SPACE` fits pktinfo + TOS exactly),
the `udp_build_control` CMSG_NXTHDR construction walk, batch alloc/free integer
safety (n∈[1,64], bufsz∈[1,65535] — products well under SIZE_MAX; matched
free-sizes; partial-alloc cleanup), the recvmmsg/sendmmsg lifecycle and the
mid-batch use-after-free rechecks, `udp_drop_front`'s self-bounding dequeue (the
prior scan-build fix is genuinely sound), GSO/GRO split bounds, per-packet
`node->tos` initialization, and fd-leak/double-free paths. Tooling: no dangerous
functions, no VLAs, no unsanctioned allocation, `-Werror`-clean production build,
ASan+UBSan green, cppcheck only benign false positives.

---

## Low

### L1 — `udp_parse_tos` computed a length by subtraction (size_t underflow risk)
**`src/udp.c` (`udp_parse_tos`)** — *fixed.*

```c
size_t dl = cm->cmsg_len - CMSG_LEN(0);   /* wraps if cmsg_len < CMSG_LEN(0) */
if (dl >= sizeof(int)) { ... }
```

A control message matching the TOS level/type but with `cmsg_len < CMSG_LEN(0)`
would underflow `dl` to a huge value and read `CMSG_DATA(cm)` past a runt cmsg.
Not reachable in practice (these cmsgs originate from the kernel, which always
sets a valid `cmsg_len`, and `CMSG_NXTHDR` won't advance into a runt trailer),
but it was the only one of the three cmsg parsers to subtract rather than use the
safe additive comparison. **Fix:** mirror `udp_parse_local`/`udp_parse_gro` —
`if (cm->cmsg_len >= CMSG_LEN(sizeof(int)))` / `>= CMSG_LEN(1)`. No subtraction,
matches the codebase idiom.

## Documentation

### D1 — `kl_dns_parse_all` referenced but never implemented
**`CLAUDE.md`, `docs/dns_parity_design.md`** — *fixed.*

The Phase 2a design proposed a separate `kl_dns_parse_all()`; the implementation
instead folded multi-address collection directly into `kl_dns_parse_response`
(which fills `out->addrs[]` up to `KL_RESOLVE_MAX_ADDRS`). No such function
exists, yet CLAUDE.md claimed it was an implemented, fuzzed parser. **Fix:**
corrected CLAUDE.md to reference only `kl_dns_parse_response`, and noted the
plan-vs-implementation deviation in the design doc.

---

## Areas audited clean (no findings)

- **`udp.c` cmsg buffer sizing** — RX (64B) and TX (48B) buffers exactly cover the
  worst-case simultaneous cmsg sets; `udp_build_control` sets `msg_controllen`
  before the CMSG_NXTHDR walk, NULL-guards every write, and `used ≤ bufsz` always.
- **`udp.c` batch lifecycle** — no integer overflow (clamped n/bufsz), matched
  alloc/free sizes, correct partial-failure cleanup, mid-batch UAF rechecks after
  every callback (recv), self-bounding `udp_drop_front` (send).
- **`udp.c` GSO/GRO** — arg validation + fallback loop bounds; GRO split rechecks
  `recv_active` between segments.
- **`udp.c` public API** — NULL/`fd<0` guards on every entry; idempotent
  `kl_udp_free`; no double-close; fd freed on all `kl_udp_init` error paths.
- **`udp_server.c`** — all public entries NULL-checked; reply source-pinning
  bounds-checked and reset per datagram; all config fields forwarded with correct
  types; no fd leak on partial init.
- **`dns_resolver.c` response parser** — `dns_skip_name` /
  `kl_dns_parse_response` / `dns_question_matches`: every packet read length-checked
  before dereference; compression pointers skipped in place (no forward-follow, no
  loop); RDATA length validated before the fixed 4/16-byte memcpy; additive
  bounds math (no underflow); `naddrs` capped; anti-spoof (txn-id + source +
  question echo) intact. No regression.
- **Tooling** — no `strcpy`/`sprintf`/`atoi`/`alloca`/… ; no VLAs; no libc
  allocation outside `allocator.c`; `make` `-Werror` clean; `make debug-test`
  ASan+UBSan green; `make cppcheck` only benign `staticFunction`/const-suggestion
  false positives; production hardening flags all present.

## Recommendation

Both findings are fixed. The UDP feature surface — including the untrusted-input
cmsg parsers and the batched/offload paths — is well-bounded. No further action.
