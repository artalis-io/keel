# C Audit Report: KEEL

## Tenth pass — freestanding portability phase (2026-08-05)

**Scope:** the freestanding phase merged since the ninth pass (PRs #199–#210): the allocator split
(A1), `completion.h`→`KlSockAddr` (A2), freestanding public headers + `kl_ssize_t`/off_t (A3/F0),
`errno`→`KlIoStatus` (B1) + sync-proxy neutrality, the `completion_driver.c`→core/server/h2/ws and
`client.c`→common/sync/async **TU splits** (B2a/B2b) behind `KlEventCtx` hooks + `src/event_ctx.c`,
F-4 formatted-I/O elimination (`src/kl_cstr.c`) + the optional reference mem*/strlen
(`src/kl_cstr_builtin.c`), and the multi-arch + PE-link gates (B1+B2). The lwip-raw accept-window
UAF (M1, #208) was the ninth pass's M1 — now **fixed**.

**Method:** the TU splits are pure code movement (nm-proven client_async.o/completion_core.o have
no cross-stack deps; full suite + harness green) — not re-audited. A focused reviewer took the
genuinely-NEW hand-written logic — `kl_cstr.c` (bounded parsers/formatters on **untrusted** URL/
response input), `kl_cstr_builtin.c` (reference mem*), `event_ctx.c` (the split watcher API) — plus
mechanical sweeps and the automated gate in the Apple container.

**Automated tools + mechanical: CLEAN.** cppcheck **0 err/warn**; scan-build **"No bugs found"**;
**64 suites** under ASan+UBSan (incl the new `test_kl_cstr` 6/6 + `test_kl_cstr_builtin` 5/5); the
url parser (which now routes through `kl_strchr`/`kl_strstr`/`kl_parse_u16_decimal`) is fuzzed; no
unsafe libc funcs, no raw malloc/free, no VLAs in the new TUs; hardening unchanged.

### Findings

| # | File:Line | Sev | Issue | Fix |
|---|-----------|-----|-------|-----|
| L1 | `include/keel/client.h:42` | Low (cosmetic) | `KL_CLIENT_CHUNK_HDR_SIZE 16` commented "Fits `FFFFFFFFFFFFFFFF\r\n`" — that line is 18B+NUL, doesn't fit 16. **No overflow**: `chunk_buf`=4096 → real chunk sizes ≤3 hex digits, and `kl_buf_append_hex` is bounded (returns -1→error) even adversarially. **Fixed**: bumped to 24 + accurate comment. |

### Verdict (verified clean)
- **`kl_cstr.c`** — the append builders never compute `off+len` directly (guard order `o>cap` →
  `len>cap-o`), so no additive overflow and no NUL off-by-one; `kl_u64_to_dec/hex` bound the
  `tmp[20]`/`tmp[16]` reversal + reject `n>cap`; `kl_parse_u16_decimal` rejects at the exact u16
  boundary before any uint32 overflow; `kl_strstr`/`kl_strchr`/`kl_streq`/`kl_str_startswith` match
  libc (empty needle, NUL, short strings). Every caller (sockaddr/url/client_common/client_async)
  passes `sizeof(buf)` as cap and aborts on append failure — an adversarial oversized host/path/
  header yields a clean failure, not an overflow. `kl_u64_to_*` don't NUL-terminate; the one raw
  caller (`sockaddr.c` v6 formatter) memcpys exactly `r` + terminates itself; all others go through
  `kl_buf_append_*` which write the NUL. **Clean.**
- **`kl_cstr_builtin.c`** — memmove both directions + `d==s`/`n==0` early-out; unsigned memcmp
  ordering; the self-referential-lowering foot-gun is disarmed (`-fno-builtin` base +
  `-fno-tree-loop-distribute-patterns` on the self-contained target, probe-gated); out of CORE_SRC
  so no duplicate-def on hosted/EDK2. **Clean.**
- **`event_ctx.c`** — the split preserved the watcher-list invariants + the **H1 liveness guard**
  (which lives in the `event_ctx.h` inline, so both `kl_event_ctx_run` and the completion driver
  see it); add-failure unlinks+frees; `kl_event_ctx_run` heap path SIZE_MAX-guarded. **Clean.**

Overall: **Low** risk — one cosmetic comment (fixed), no Critical/High/Medium. The new
freestanding logic is correctly bounded, overflow-guarded, and NUL-termination-correct.

---

## Ninth pass — lwIP-raw client axis (LC-0..LC-5) + full re-sweep (2026-08-05)

**Scope:** the completion-native lwIP-raw **client** work merged since the eighth pass —
LC-0 completion-CONNECT contract (`KL_COMP_CONNECT` + `post_connect` across `completion.h`/
`completion_dispatch.c`/`completion_driver.c`/`event_pollcomp.c`/`event_iouring.c`/
`event_iocp.c`/`src/client.c`/`src/async.c`), LC-1/LC-2 plaintext + Happy-Eyeballs raw client,
LC-3a `KlUdp` over lwip-raw, LC-3 DNS via KEEL's `dns_resolver.c` on `KlUdp`-over-raw, LC-4
HTTPS (client socket-BIO through the raw provider + server memory-BIO completion-TLS leg, two
backend fixes), LC-5 caps/docs (PRs #191–#197). Files centered on `integrations/lwip/`
(`event_lwip_raw.c`, `lwip_raw_glue.c/.h`, `lwipopts_raw.h`) + the cross-backend LC-0 seam.

**Method:** two parallel deep-review agents (the lwip-raw backend/glue lifetime + the LC-0
cross-backend connect primitive), each tracing recv/send/connect/close-with-outstanding paths;
plus mechanical sweeps (no `strcpy`/`sprintf`/`atoi`, no raw `malloc`/`free` outside test
harnesses) and the automated gate in the Apple container: **cppcheck 0 errors/warnings,
scan-build "No bugs found," 60 suites passing under ASan+UBSan** (`make debug-test`,
`detect_leaks=1`), and the loopback-raw + raw-tls ASan runs. Build hardening confirmed
unchanged (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 -fstack-protector-strong
-fPIE -D_FORTIFY_SOURCE=3`; SIGPIPE fully handled).

**Automated tools: CLEAN.** cppcheck (0), scan-build (No bugs), no unsafe libc calls in
`src/`+`parsers/` (only `allocator.c`'s stdlib wrapper legitimately wraps `realloc`), no VLAs,
allocator discipline intact in the new backend (all Keel-owned memory via `KlAllocator`, no
hot-path allocation — TX window pre-allocated, only lwIP's own `pbuf_alloc` for a UDP send).

### High

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| H1 | `include/keel/event_ctx.h:140` (`kl_event_dispatch`) + `src/completion_driver.c` (`kl_comp_run` batch) + `src/client.c` (`he_close_attempts`/`client_drop_connect_fd`) | **Same-batch Happy-Eyeballs connect use-after-free.** `kl_comp_run` drains multiple completions into one batch and dispatches them sequentially; `kl_event_dispatch` derefs the tagged `KlWatcher*` with **no liveness check**. When two racing connect attempts (dual-stack A+AAAA) both complete in one drain, dispatching the winner's `KL_COMP_CONNECT` runs `he_win → he_close_attempts → client_drop_connect_fd → kl_watcher_del`, which **synchronously frees the loser's watcher node**; dispatching the loser's still-batched `KL_COMP_CONNECT` then reads freed memory. The default `connect_attempt_delay_ms=250` shields the loopback happy path (only one attempt in flight), so tests pass; a genuinely-racing dual-stack connect (both in flight, both completing in one wait) hits it. Confirmed by direct trace on pollcomp + io_uring. **Fix:** in the `KL_COMP_CONNECT`/`KL_COMP_WATCHER` dispatch, validate the tagged watcher is still linked in `ctx->watchers` before deref (small, backend-agnostic guard); skip if it was freed earlier in the same batch. |

### Medium

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| M1 | `integrations/lwip/lwip_raw_glue.c:766-804` (`lwr_srv_accept`/`lwr_srv_err`) | **Dangling-pcb UAF in the accept→post_recv window.** `lwr_srv_err` keys on `c->owner == arg`, but `owner` is set only later (first `post_recv`), and `lwr_srv_accept` never `tcp_arg`s the new pcb (it inherits the listener's arg). If lwIP aborts the accepted pcb (peer RST / OOM) before the driver's `post_recv`, the err is dropped, the slot keeps `dead=0` + a dangling `->pcb`, and the pending ACCEPT still surfaces → the driver adopts a freed pcb (UAF). Narrow timing (needs a stack-initiated abort in that window; rare on loopback). **Fix:** `tcp_arg(newpcb, slot)` at accept and resolve `lwr_srv_err` by the slot pointer, so a pre-owner abort still marks its slot `dead`. |

### Low

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| L1 | `src/redirect.c:77` (`is_cross_origin`) | **UBSan: NULL passed to `strncasecmp` (declared nonnull).** When both URLs have `host_len==0`, the equal-length guard falls through to `strncasecmp(a->host, b->host, 0)` with NULL hosts — defined-behavior-pedantic UB (0 length, no actual read), but it trips `UBSAN_OPTIONS=halt_on_error=1`. CI stays green only because its UBSan job runs recover-mode (prints, doesn't fail). **Fix:** `if (a->host_len == 0) return 0;` (or guard `a->host && b->host`) before the `strncasecmp`. |
| L2 | `integrations/lwip/event_lwip_raw.c:672-677` | **False EOF vs explicit overflow on a full non-TLS `read_buf`.** When `space==0` with `rx_queued>0`, the drain surfaces `bytes=0` → the driver reads it as peer-close (EOF), where the readiness backend (`event_pollcomp.c:248`) deliberately signals a header-overflow failure. Converges to a close either way — cosmetic (wrong reason). **Fix:** surface a failed READ when `space==0`. |
| L3 | `src/completion_driver.c:768`, `event_pollcomp.c:358`, `event_iouring.c:695` | **Stale comments** claim the client re-reads `SO_ERROR` for `KL_COMP_CONNECT`; the actual path (`he_on_connect_result`, `client.c`) trusts the mask-carried win/fail and does **not** read `SO_ERROR` (correct for io_uring, which drops it). Comment-only. **Fix:** correct the comments to reference `he_on_connect_result` + the mask. |

### Verdict
The new code is disciplined on overflow, allocation, backpressure, and the neutral seam
(`event_lwip_raw.c` stays lwIP-free; all `tcp_*`/`udp_*` in the glue), and the automated tools
are clean. Two real use-after-frees (H1 same-batch HE connect race; M1 accept-window pcb) are
edge-timing bugs a happy-path sanitizer run misses — both warrant fixes; H1 is the priority as
it sits on the outbound connect path for any completion backend. L1 is a trivial UBSan fix. The
underlying feature (raw client + HE + DNS + HTTPS) is functionally sound and end-to-end verified.

---

## Eighth pass — datagram data-plane folded onto the socket provider (2026-08-03)

**Scope:** the four-stage "datagram provider" refactor (#168–#172) that resolves
axis-audit **A2** — the UDP datagram data-plane moves from the compile/link
`udp_io_*` seam onto an optional `KlDatagramOps` vtable on `KlSocketProvider`, so
one runtime provider owns stream **and** datagram I/O. Files: `include/keel/datagram.h`
(the new vtable + `KlDgramRxSlot`/`KlDgramTxDesc`/`KlDgramRxMeta`), `src/socket_dgram_posix.c`
+ `src/socket_dgram_win.c` (the POSIX/Winsock datagram primitives, ~1000 lines),
`src/udp_cmsg.c` + `src/udp_cmsg_win.c` (shared cmsg parsers the completion
backends reuse), `src/udp.c` (the send-queue flush + recv drain **machine loops
moved up here**, provider dispatch, `udp_group_ok` mcast validation, batch
lifecycle, dgram-required init), the `socket.h`/`socket_posix.c`/`socket_winsock.c`
provider wiring + `kl_sockdef_dgram()`, the completion backends' `.dgram`
inheritance, and `integrations/lwip/socket_lwip.c`'s datagram ops. `udp_io_posix.c`,
`udp_io_win.c`, `udp_io.h` were deleted.

**Method:** three parallel deep-review agents (POSIX dgram + udp_cmsg; Winsock dgram
+ udp_cmsg_win; udp.c machine rework + lwIP + provider wiring) tracing cmsg
build/parse bounds, mmsg-batch alloc/index/free symmetry, the data-oriented
recv_batch/send_batch slot/descriptor arrays, address marshalling on untrusted
recv addresses, callback re-entrancy in the machine loops, and the dgram-default
resolution; mechanical sweeps (no `strcpy`/`sprintf`/`atoi`, no raw `malloc`/`free`
in the new TUs); and the Stage-4 gate — **cppcheck + scan-build clean, the full unit
suite under ASan+UBSan (891 tests, 0 failures, 0 sanitizer hits)**, gcc-14 +
cosmocc + MinGW (iocp/wsapoll), and the Apple container: epoll (0 fails, UDP
batching 5/5 via real `recvmmsg`/`sendmmsg`), io_uring (`smoke-iouring-asan`
UDP-over-completion), and the lwIP loopback + HTTPS.

**Verdict: clean.** No Critical or High findings. The memory-safety surface of the
refactor — cmsg bounds, the mmsg batch blocks, the data-oriented slot/descriptor
arrays, address marshalling, and machine-loop callback re-entrancy — is sound. One
**Low** correctness item was **fixed this pass**; the rest are Low/Informational,
pre-existing or unreachable via the public API.

### Low — fixed this pass

| # | File(s) | Issue | Fix |
|---|---------|-------|-----|
| L1 | `src/socket_dgram_posix.c` / `src/socket_dgram_win.c` (`pdg_send`/`wdg_send`) | On a source-pinned/TOS send with **no destination** (a connected socket), the TOS control-message's IP level was guessed `AF_INET`, so a TOS mark on a connected **IPv6** socket would build an `IP_TOS` (v4) cmsg → kernel ignores/rejects it. Unreachable via the public API today (`kl_udp_send_to_tos`/`_from` always pass a dest; `kl_udp_send` uses tos −1), so defense-in-depth. | Derive the family from the dest, else the **source-pin** address, else v4. |

### Reported, not changed (accepted / by design)

| # | Area | Note |
|---|------|------|
| R1 | `src/udp.c` completion-loop src-pin/TOS send | A source-pinned or per-packet-TOS send on a **completion** loop skips the overlapped `kl_comp_post_udp_send` branch (which is plain-send only) and takes the synchronous provider `send()`; on `EAGAIN` it `udp_enqueue`s, which arms a **readiness** WRITE watcher that a completion loop never drives → the datagram stalls. **Pre-existing** (the pre-refactor path did the same via `kl_udp_io_raw_send` + `udp_enqueue`); IOCP-only, and only under transient send-buffer pressure on a source-pinned/marked datagram. Fix would be an overlapped `WSASendMsg` path for src/TOS — deferred as its own change. |
| R2 | `src/udp.c` batch machine loops | `udp_recv_dgram`/`udp_flush_dgram` put a `KlDgramRxSlot[64]`/`KlDgramTxDesc[64]` (~17 KB) on the stack **inside the batch branch only** (mmsg batching is Linux + opt-in `mmsg_batch>1`); the per-datagram path (lwIP/embedded) never allocates them. Fine on host stacks; noted for tiny-stack targets. Optionally cap `UDP_MMSG_MAX` or heap the arrays. |
| R3 | `src/socket_dgram_posix.c` `configure` (macOS IPv4) | Sets `IP_PKTINFO`/`IP_RECVPKTINFO` and reports `KL_DGRAM_RX_PKTINFO`, but macOS delivers the IPv4 local address via `IP_RECVDSTADDR` (not `IP_PKTINFO`), so `meta.has_local` stays 0 there — graceful degradation, but the cap bit overstates. Pre-existing (matches the old `setup_recv_opts`). |
| R4 | Winsock length casts (`wdg_send`/`wdg_recv`) | `size_t len`/`buflen` cast to `ULONG`/`int` without a guard; unreachable for UDP (≤65507 B). |
| R5 | `cfg->tos == 0` in `configure` | `if (cfg->tos)` treats 0 as "OS default / unset" (documented in `udp.h`), so 0 can't be an explicit clear — by design. |

### Areas audited clean (no findings)

- **cmsg build/parse** (posix + win): every parse `memcpy` is length-gated
  (`CMSG_LEN`/`WSA_CMSG_LEN`), the Winsock parsers stop on a runt cmsg
  (`cmsg_len < sizeof(WSACMSGHDR)`), and `dgram_build_control` writes at most the
  `DGRAM_TX_CMSG_SPACE` the buffers are sized to.
- **mmsg batch** (`socket_dgram_posix.c`): `rx/tx_batch_new`/`free` use matching
  `(size_t)n * sizeof/bufsz/ctrl_sz` for every member, sizes set before the
  sub-allocs so a mid-alloc NULL frees cleanly (no partial leak); `recv_batch`/
  `send_batch` clamp to `min(b->n, max/n)`; all per-slot indexing is `< n <= b->n`.
- **data-oriented slots**: `KlDgramRxSlot.data` points into the batch payload and is
  delivered synchronously before the next `recv_batch` — lifetime honored by
  `udp_recv_dgram`.
- **machine loops** (`udp.c`): batch fill double-bounded (`cnt < mmsg_batch && cnt <
  UDP_MMSG_MAX`); `udp_drop_front(sent)` drops exactly what was sent; EAGAIN vs
  hard-error-drop correct; recv loops re-check `recv_active`/`kl_handle_valid` after
  every deliver (no callback-reentrancy UAF); no uninitialised `KlSockAddr` reaches
  `on_recv` (the `family != UNSPEC ? &src : NULL` guard).
- **dgram resolution + lifetime**: `udp_dg()` maps NULL sockets → `kl_sockdef_dgram()`;
  `kl_udp_init` rejects a provider without `.dgram` before opening the fd (no leak);
  the completion providers inherit the underlying `.dgram` for config/opts only (the
  data-plane stays on `kl_comp_post_udp_*`, gated by `KL_EVENT_CAP_COMPLETION`).
- **`udp_group_ok`**: `kl_sockaddr_parse` + first-octet multicast range check
  (IPv4 224/4, IPv6 ff00::/8), family cross-checked; a bad group string rejects
  cleanly (`KL_ERR_INVALID_ARG`).
- **lwIP datagram ops**: send/recv/gso/configure/set_tos/mcast bounds + family
  guards correct; batch NULL → per-datagram; no leak.
- scan-build: clean. cppcheck: clean. ASan+UBSan unit suite: 891 tests, 0 failures.

## Seventh pass — KlSockAddr address-ABI neutralization + lwIP platform (2026-08-02)

**Scope:** everything added/changed since the sixth pass — the runtime event-provider seam
(#150/#151), the **KlSockAddr address-ABI neutralization** series (#153–#159: canonical type +
pure helpers, socket-vtable currency, resolver, udp public API, accept + proxy_protocol + peer
addr, protocol-TU purge + grep-gate, lwIP payoff), and the **lwIP platform** (#152, #160–#165:
socket + event providers, client axis via `resolve_sync_lwip`, responsive stop via
`platform_wakeup_lwip`, `udp_io_lwip` + the `udp_io` seam→KlSockAddr flip, TLS-over-lwIP via the
provider-routed mbedTLS BIO, production `lwipopts.h`). Files: `sockaddr.{h,c}`,
`sockaddr_native.h`, `socket.h` + `socket_{posix,winsock}.c`, `udp.c` +
`udp_io_{posix,win}.c` + `udp_internal.h`, `resolver_cache.c`, `dns_resolver.c`,
`dns_sys_{posix,win}.c`, `completion_driver.c` + `event_{pollcomp,iouring,iocp}.c` (UDP
marshalling), `integrations/lwip/*`, and the `integrations/mbedtls` socket-provider routing.

**Method:** five parallel deep-review agents (sockaddr core; udp seam + I/O; lwIP integration;
completion-backend UDP marshalling; resolver + socket providers) tracing marshalling bounds,
op/buffer/node lifetime, integer math, and untrusted-input parsing; mechanical sweeps
(`src/`+`parsers/`: no `strcpy`/`sprintf`/`gets`, no `atoi`/`atol`/`atof`, raw `malloc`/`free`
only in the allocator wrapper); **scan-build — "No bugs found"**; **cppcheck — clean**; the
**full unit suite under ASan+UBSan (`make debug` + `make test`) — 891 tests, 0 failures, 0
sanitizer hits**; gcc-14 + cosmocc + MinGW (IOCP/WSAPoll) compile gates; and the Apple container
for Linux epoll/io_uring + the lwIP loopback/HTTPS runtime tests.

**Verdict: clean after fixes.** No Critical or High findings. Four **Medium** memory-safety
defects on untrusted-input (network) paths were found **and fixed this pass**, plus one Low
hardening fix and the cppcheck gate made version-robust. The KlSockAddr marshalling boundary is
the right place to have caught these — the neutralization concentrated all host↔neutral address
conversion into one reviewed seam.

### Medium — fixed this pass

| # | File(s) | Issue | Concrete risk | Fix |
|---|---------|-------|---------------|-----|
| M1 | `src/sockaddr_native.h` `kl_sockaddr_from_native` | AF_INET/AF_INET6 cases cast an **untrusted** `struct sockaddr` (from `accept`/`recvfrom`) to `sockaddr_in`/`sockaddr_in6` and `memcpy`'d the address **without a lower-bound `len` check** (only AF_UNIX checked `len`). | A short/garbage `socklen` from the kernel or a foreign provider → up to 16-byte out-of-bounds read past the caller's address buffer. | Added `if (len < sizeof(struct sockaddr_in{,6})) return -1;` before each cast/`memcpy`. |
| M2 | `src/udp_io_posix.c` (batched + single recv), `src/udp_io_win.c`, `src/completion_driver.c` (`KL_COMP_UDP_RECV`) | The four datagram-recv paths passed `&ksrc` to the `on_recv` callback **unconditionally**. When `kl_sockaddr_from_native` fails (unrecognised family, or `peer_len == 0` on a connected socket) it leaves the scratch **untouched** → an **uninitialised `KlSockAddr` (stack garbage) delivered as the datagram source** to application code. | Info-leak of stack contents into the app's source-address logic / reply targeting, reachable from network input. | Honor the return value: pass `NULL` (unknown source) instead of uninitialised stack; guard the local (pktinfo) address the same way. |
| M3 | `integrations/lwip/event_lwip.c` `lwev_mod` / `lwev_del` | Negative-fd guard used `!kl_handle_valid(fd)`, which only rejects `-1`; an `fd <= -2` passed and indexed `fd_to_idx[(int)fd]` — a **negative-index OOB** read (and an OOB write in `del`). `src/event_poll.c` guards the full negative range. | Memory corruption from a bogus/underflowed lwIP descriptor reaching mod/del. | `(int)fd < 0 || (int)fd >= cap`, matching `event_poll.c`. |

(M2 is one defect across four sites. Its readiness-posix instance predates this series but shares
the marshalling pattern introduced here and was fixed for completeness.)

### Low / tooling — fixed this pass

| # | File(s) | Issue | Fix |
|---|---------|-------|-----|
| L1 | `src/resolver_cache.c` `cache_insert` | Fixed `host[]` buffer filled via `memcpy(strlen+1)` with no self-check (callers bound it, but the function wasn't self-defending). | `strlen(host) >= KL_CLIENT_HOSTNAME_MAX` early return. |
| L2 | `Makefile` `cppcheck` | A newer cppcheck than CI's failed `make cppcheck` on `staticFunction` **false-positives** (public-API `kl_*` functions flagged should-be-static because cppcheck can't see the header consumers) and `normalCheckLevelMaxBranches` **informational** notes. | Added `--suppress=staticFunction --suppress=normalCheckLevelMaxBranches` — keeps the gate green across cppcheck versions without hiding real defects. |
| L3 | `src/async.c`, `src/dns_resolver.c`, `src/server.c` (×2), `src/body_reader_buffer.c` | Newer-cppcheck `constParameterPointer`/`constVariablePointer` on read-only pointers/params. | Added `const` (4 sites). The `kl_body_reader_buffer` factory param stays `void*` to match the `KlBodyReaderFactory` typedef — inline-suppressed with justification. |

### Reported, not changed (accepted / by design)

| # | File | Note |
|---|------|------|
| R1 | `src/socket_winsock.c` `kl_wsa_set_errno` | On Windows `EAGAIN != EWOULDBLOCK`; the mapping emits `EWOULDBLOCK`. Safe because every would-block test in-tree ORs **both** codes (verified across `dns_resolver.c` + the socket-seam callers). A contract, not a bug; keep the OR convention (a `KL_EWOULDBLOCK()` helper would make it self-enforcing). |
| R2 | `src/dns_resolver.c` `dns_resolve` | An over-long hostname (`> DNS_NAME_MAX`) is `snprintf`-truncated before `dns_build_candidates` rejects it, so a truncated name could be queried rather than hard-failed. No memory-safety impact. Optional: reject `strlen(host) >= DNS_NAME_MAX` up front. |
| R3 | `src/event_iocp.c` | A UDP send with an UNSPEC destination on an *unconnected* socket fails silently at completion (`ok=0`) rather than up-front; unreachable in practice because `kl_udp_send_to_from` validates `dest` at the public API. |
| R4 | `integrations/lwip/{socket_lwip,platform_wakeup_lwip}.c` | `set_reuseport`/`set_cork` return `-1` without setting `errno` (cosmetic diagnostic); the wakeup ignores `lwip_send`'s return (idempotent wakeup, recovered on the next poll tick). |

### Areas audited clean (no findings)

- **DNS response parser** (`kl_dns_parse_response`, `dns_skip_name`, `dns_extract_opt`,
  `dns_question_matches`): no compression-pointer following loop (names are terminated in place),
  label / RDATA / RR-header / address-size (`rdlen==4/16`) / multi-address-array bounds all tight.
  Also fuzzed (`fuzz_dns`).
- **`sockaddr.c`** parsers/formatters (`parse_ipv4`/`parse_ipv6`, `kl_sockaddr_format*`): bounded,
  no overflow; every builder `memset`s the union (no uninitialised-field leak in the round-trip).
- **UDP queue node** (flexible array): alloc-size overflow guard, matching free size on every
  path (send / EAGAIN-requeue / hard-error-drop / free); send-queue byte-cap underflow-guarded;
  GRO-split loop + cmsg build/parse bounds (runt-cmsg underflow guarded); recvmmsg/sendmmsg batch
  alloc/index/partial-free.
- **Completion backends** (pollcomp / io_uring / IOCP): op sockaddr-buffer sizes vs the socklen
  used in send/recv, recv name/control-buffer bounds, and op alloc/free lifetime — including the
  immediate-failure early-return paths and cancelled / zero-byte completions (no stale-buffer
  parse, no double-free/leak).
- **mbedTLS socket-provider routing**: NULL `sp` is byte-for-byte the prior behaviour
  (`kl_sockdef_*` fallback); per-session inheritance from the ctx; borrowed (not owned) provider
  pointer — no leak, no dangling copy; the completion memory-BIO path is unaffected.
- **socket_posix/winsock accept marshalling**: untrusted-peer `socklen` bounded before
  `kl_sockaddr_from_native`; `writev` iovcnt bounded before the stack `iovec[]`.

## Sixth pass — completion-axis feature work (PROXY / streaming / TransmitFile / TLS) (2026-08-01)

**Scope:** everything added/changed since the fifth pass — the completion-backend feature run
(PRs #128, #130, #133, #134, #135, #136): the completion driver's PROXY-header phase
(`comp_drive_proxy` + `kl_conn_ingest_proxy`) and overlapped streaming flush (`comp_stream_pump`,
8g-1); `event_iocp.c` (overlapped `WSARecvMsg` UDP local-addr, chunked `TransmitFile`, the
`post_recv` plaintext-during-PROXY guard); the three backends' `post_recv` guard; `drain.c`'s new
`kl_drain_data`/`kl_drain_consume`; `response.c` streaming-drain wiring; the TLS unit suites ported
to the shared completion-capable `tests/mock_tls.h`; and verification of the (already-landed)
platform-neutral mbedTLS backend on Windows.

**Method:** two parallel deep-review agents (completion axis; protocol/support + broad re-scan)
tracing op/buffer/connection lifetime, integer/offset math, and bounds; mechanical sweeps across
`src/`+`parsers/` (no `strcpy`/`sprintf`/`gets`, no `atoi`/`atol`/`atof`, raw `malloc`/`free` only
in the allocator wrapper, `kl_malloc` NULL-check + free-size discipline); `cppcheck` (clean on the
changed TUs); the **full 55-suite unit test under ASan+UBSan (`make debug-test`) — 0 failures, 0
sanitizer hits**; and a MinGW compile-gate on the touched Windows TUs.

**Verdict: clean.** No Critical/High/Medium findings. Two **Low** allocator-discipline defects
(free-size mismatch on a zero-length completion send, latent under a bring-your-own *sized*
allocator; harmless under the default stdlib allocator) were found **and fixed this pass**.

### Low — fixed this pass

| # | File | Issue | Fix |
|---|------|-------|-----|
| L1 | `src/event_pollcomp.c` `pc_op_free` | A zero-length WRITE/SENDFILE/UDP op allocs `sendbuf` as `total ? total : 1` (1 byte) but `pc_op_free` freed it as `send_total ? send_total : KL_PC_CIPHER_SIZE` → frees a 1-byte block as **17408 bytes**. Reachable via a 0-length TLS-ciphertext send. A size-classed/arena `KlAllocator` mis-buckets the free. | Fall back to `1`, not `KL_PC_CIPHER_SIZE` (mirrors the alloc; `send_total` is set to the real size on every alloc path). |
| L2 | `src/event_iocp.c` `iocp_op_free` | Same shape: a zero-length WRITE frees a 1-byte `sendbuf` with `send_total == 0`. | Free `send_total ? send_total : 1`. |

### Informational (no action required)

- **I1** `event_iocp.c` `KL_IOCP_SENDFILE` completion advances `file_done` by the *requested*
  `file_chunk`, not bytes actually transferred. Correct because overlapped `TransmitFile` is
  all-or-nothing per chunk; a short success (never observed) would garble (not OOB) the body.
- **I2** `event_iouring.c` `kl_comp_cancel` leaves an op in-flight if the SQ is full at cancel
  time (liveness edge under SQ pressure, not a safety bug).
- **I3** `dns_resolver.c:976` write-buffer growth omits the project's `SIZE_MAX/2` doubling idiom;
  safe today (values bounded to a few KB) — add the guard for uniformity.

### Verified correct (explicitly not findings)

Drain peek/consume is safe because `kl_comp_post_send` **copies** the buffer synchronously in all
three backends (so `kl_drain_consume` after posting is not a UAF); the PROXY `memmove` never
underflows (`kl_proxy_parse`'s `consumed ∈ [0,len]`, and `read_cap` 8192 ≫ `KL_PROXY_HEADER_MAX`
536, bounded by the `-1` guard); the ≤1-in-flight invariant (recv XOR send per conn, posted only
from a completion) means `kl_comp_cancel` yields exactly one aborting completion → one release, no
double-free; every `kl_comp_post_*` frees its op on all error paths; the Windows cmsg walks
(`kl_udp_win_parse_local`/`udp_parse_tos`) stop on a runt cmsg; `response.c` fixed stack buffers
(`cl_buf[48]`, `hdr[24]`) are correctly sized; CRLF header-injection + CL/TE smuggling (llhttp)
guards intact.

---

## Fifth pass — io_uring completion backend + provider auto-wire + stop-wakeup (2026-07-30)

**Scope:** the code added/changed across the io_uring-completion migration (PRs #101–#110):
`src/event_iouring.c` (the new ~750-line completion backend — hand-written op/registered-
buffer/splice/watcher lifecycle, the highest-risk new C), the 5a provider auto-wire
(`kl_event_native_provider` in every event backend + `kl_server_init`/client), the
`kl_server_stop` self-pipe wakeup (`src/server.c`), and the `iouringcomp`→`iouring` rename.

**Method:** mechanical sweeps (unsafe string/parse funcs, raw `malloc`/`free`, VLAs, `kl_malloc`
NULL-check discipline, integer-overflow guards) across `src/`; `cppcheck` on the new TUs; and —
the decisive step — an **ASan + UBSan + LeakSanitizer** run of the io_uring smokes on Linux
(Apple `container` VM, kernel 6.18), which the plain CI smokes don't provide.

**Issues found: 1** (Critical: 0, **High: 1**, Medium: 0, Low: 0) — **fixed + verified.**

### High

| # | File | Issue | Fix |
|---|------|-------|-----|
| H1 | `src/event_iouring.c` (`kl_event_del`) | **Memory leak** — a readiness watch (`KlIouWatch`) removed while its `IORING_OP_POLL_ADD` was in flight was *unlinked* from `st->watches` with its free *deferred* to the poll-cancel CQE. At shutdown `kl_event_close`→`io_uring_queue_exit` drops that CQE, so the unlinked watch was never freed (and `kl_event_close`, freeing `st->watches`, no longer saw it). Surfaced as the `kl_server_stop` self-pipe watch leaking 48 B per server (2× in the async smoke). LeakSanitizer-confirmed; missed by CI because the io_uring backend was never run under LSan (only pollcomp was). | Keep the removed watch **linked** (skipped by `add`/`mod` via `!w->removed`); free it in the drain CQE (unlink+free) on the normal path, or in `kl_event_close` if the CQE never arrives (shutdown). Re-verified: both io_uring smokes pass under ASan+UBSan+LSan with **zero leaks**. |

**Systemic fix:** added `make smoke-iouring-asan` + a CI step in the *Completion (io_uring)*
job, so the io_uring op/buffer/splice/watcher lifecycle is now under LeakSanitizer in CI (the
gap that let H1 reach `main`).

### Clean (verified)

- **No unsafe functions** (`strcpy`/`strcat`/`sprintf`/`gets`/`atoi`/`atol`/`atof`) anywhere in
  `src/`+`parsers/`; raw `malloc`/`free` only in the default allocator wrapper (`allocator.c`).
- **`event_iouring.c` allocations** — all `kl_malloc` sites NULL-checked (registered pool is
  best-effort with malloc+SEND fallback; the rest fail cleanly via `iou_op_free`+`-1`).
- **Integer overflow** — `kl_comp_post_sendfile` guards `count`/`head_total` against `SIZE_MAX/2`;
  `sendcap` tracks the exact allocation for a correctly-sized free.
- **`cppcheck`** clean on the new TUs. **Hardening** intact (`-Werror -Wall -Wextra -Wpedantic
  -Wshadow -Wformat=2 -fstack-protector-strong -D_FORTIFY_SOURCE=3`; ASan/UBSan debug build).
- `LIBURING_UDATA_TIMEOUT` / cancel sentinels handled in the drain (no `(void*)-1` deref);
  single in-flight op per conn (driver invariant); idle-timeout cancel via `ASYNC_CANCEL` + abort.

## Fourth pass — mbedTLS backend + test shim + parser re-audit (2026-07-26)

**Scope:** the surface that changed since the third pass — `src/tls_mbedtls.c`
(significantly refactored: BIO callbacks routed through the socket seam, fds
retyped to `KlSocketHandle`, a shared `server_ctx_from_mem` + new
`kl_tls_mbedtls_ctx_create_from_buf`), the new test-network shim
(`tests/net_compat.{h,c}` posix/win + `tests/smoke_tls.c`), and a regression
re-audit of the untrusted-input parsers after the Winsock seam sweep + socket-
handle retype.
**Method:** three parallel source-level auditors — (1) deep `tls_mbedtls.c`
memory-safety (peer-cert extraction on untrusted mTLS input, error-path free
discipline, allocator/key-material handling), (2) the `net_compat` shim +
`smoke_tls` resource safety, (3) a regression re-check of `dns_resolver`,
`proxy_protocol`, `url`, `websocket`, `chunked`, `connection` — plus a tooling
sweep (dangerous functions, VLAs, raw alloc, cppcheck, and an ASan+UBSan real-
handshake run of the mbedTLS backend).

**Issues found: 3** (Critical: 0, High: 0, Medium: 0, **Low: 2, Doc: 1**) —
**all fixed.**

The TLS backend came through clean. The auditors confirmed the hard parts sound:
the peer-cert CN/SAN extraction (`x509_extract_cn`/`x509_extract_san`) is
bounds-safe on attacker-controlled certs — every `memcpy` into the fixed `subject_cn`/
`issuer_cn`(256)/`san`(512) buffers is length-clamped before the copy, the
comma-separated SAN accumulator checks `off + ilen (+1) >= outlen` *before* every
write and always NUL-terminates, and the SHA-256 fingerprint fits its `char[65]`
exactly; the refactored ctx-creation error paths free each mbedTLS structure
exactly once and scrub key material (`kl_secure_zero`) on every path that owns the
key buffer (success and every early return); `read_file`'s length handling
(negative `ftell`, 1 MB cap, `len+1`) is safe; the BIO `kl_sockdef_send`/`recv`
errno→mbedTLS mapping is correct on both platforms; allocator create/destroy is
paired with the right sizes and the shared ctx is never double-freed. The
untrusted-input parsers showed **no regression** from the seam/handle changes —
the seam preserves the POSIX `ssize_t` contract and every `fd < 0` check migrated
to `kl_handle_valid`. Tooling: no dangerous functions, no VLAs, no unsanctioned
allocation, cppcheck clean, and the mbedTLS backend runs a real loopback handshake
clean under ASan+UBSan.

---

### Low

**L1 — `x509_extract_cn` / `x509_extract_san`: defensive `outlen == 0` guard**
**`src/tls_mbedtls.c`** — *fixed.* Both peer-cert extractors wrote `out[0] = '\0'`
unconditionally and (CN) computed `outlen - 1`; safe with today's callers (fixed
256/512 buffers), but a future `outlen == 0` caller would write OOB / underflow.
Added `if (outlen == 0) return;` at the top of each — defense-in-depth on
untrusted-input functions.

**L2 — `tests/smoke_tls.c`: client URL hard-coded the port instead of `SMOKE_PORT`**
**`tests/smoke_tls.c`** — *fixed.* The HTTPS URL literal duplicated `18443`; if
`SMOKE_PORT` changed, the client would silently target the old port and the test
would fail confusingly. Now built with `snprintf` from `SMOKE_PORT`.

### Documentation

**D1 — `tests/net_compat_win.c`: undocumented WSAStartup precondition**
**`tests/net_compat_win.c`** — *fixed.* The loopback-pair helper needs Winsock
initialized; the library's load-time `WSAStartup` (socket_winsock.c) covers every
test, but that was implicit. Added a precondition note to the file comment.

### Areas audited clean (no findings)
- **`tls_mbedtls.c` peer-cert extraction** — bounds-safe on untrusted mTLS certs
  (clamped CN/SAN copies, pre-write accumulator bound, exact-fit fingerprint).
- **`tls_mbedtls.c` ctx error paths** — each mbedTLS struct freed once; key
  material scrubbed on all owning paths; no double-free/leak/UAF; correct free
  sizes; shared ctx not freed per-connection.
- **`tls_mbedtls.c` BIO + fd** — seam-routed I/O, correct errno mapping,
  `KlSocketHandle`/`KL_INVALID_SOCKET` throughout, transport owns the fd.
- **`net_compat_win.c` `kl_test_socketpair`** — each of listener/client/server
  closed exactly once on every `goto fail`, no double-close on success, no SOCKET
  leak, `addrlen` initialized before `getsockname`.
- **`smoke_tls.c` lifetimes** — server ctx freed on all paths (direct on init
  fail; via `kl_server_free` otherwise), per-iteration client ctx destroyed once,
  server stopped+joined before ctx destroy.
- **Untrusted-input parsers** (`dns_resolver`, `proxy_protocol`, `url`,
  `websocket`, `chunked`, `connection`) — no regression; all bounds checks,
  length math, anti-spoof, and smuggling guards intact after the seam/handle sweep.
- **Tooling** — no `strcpy`/`sprintf`/`atoi`/`alloca`/…; no VLAs; no libc alloc
  outside `allocator.c`; `make cppcheck` clean; mbedTLS backend real-handshake
  ASan+UBSan green; full ASan+UBSan gauntlet green in CI on `main`.

## Recommendation
All three findings fixed. The mbedTLS backend — including the untrusted-input
peer-cert parsers and the refactored error paths — is well-bounded and now
platform-neutral. No further action.

---

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
