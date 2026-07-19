# C Audit Report: KEEL

**Date:** 2026-07-19
**Scope:** full `src/` + `parsers/` sweep, with deep focus on the recently landed
DNS-parity work (built-in resolver, dual-family state machine) and the async
client's Happy Eyeballs connect racing (RFC 8305).
**Method:** four parallel auditors — (1) Happy Eyeballs fd/timer lifecycle,
(2) DNS/UDP untrusted-input parsers + two-leg state machine, (3) core-module
memory-safety sweep, (4) automated tooling (scan-build, cppcheck, ASan/UBSan,
dangerous-function + VLA + direct-allocator grep).

**Issues found: 3** (Critical: 0, High: 1 *latent*, Medium: 0, Low: 2)
**Status: all 3 fixed (2026-07-19)** — see "Resolution" at the end. Fixing L2
additionally uncovered and fixed a latent NULL+0 UB in the response parser's
`accum_append` (UBSan), now covered by a regression test.

The library is in strong shape: production build is `-Werror` clean, scan-build
reports no bugs, cppcheck yields only cosmetic `style` findings, and the full
ASan+UBSan suite passes **797/797** tests with zero sanitizer output. No
dangerous libc functions (`strcpy`/`sprintf`/`atoi`/…), no VLAs, and no direct
libc allocation outside the sanctioned `KlAllocator` default wrapper.

---

## High (latent — not reachable by any shipped code path)

### H1 — UAF when a user-supplied resolver completes synchronously *and* returns NULL
**`src/client.c:2420-2434`** (interacts with `dns_resolved`)

The `KlResolver` contract (`include/keel/resolver.h:55-60`) permits `resolve()`
to call `done_fn` **synchronously** and to return **NULL** on error.
`kl_client_start_s` conflates "resolve failed to start" with "resolve already
completed synchronously":

```c
c->resolve_req = resolver->resolve(...);   /* may run dns_resolved synchronously */
if (!c->resolve_req) {                      /* also NULL after sync completion */
    ...
    kl_free(alloc, c, sizeof(KlClient));    /* frees c that on_done already received */
    return NULL;
}
```

If a conformant resolver resolves synchronously (literal IP / cache hit /
`/etc/hosts`) and the request then completes inside the call, `dns_resolved` sets
`c->resolve_req = NULL` and fires `on_done(c)` with `state = DONE`. Should that
resolver then return NULL, `c` — already handed to `on_done` — is freed, and the
caller (seeing NULL) believes start failed and never calls `kl_client_free`:
use-after-free for `on_done`, and the request buffer/parser are only saved from a
double-free because `async_complete_*` NULLs them first.

**Not reachable today:** the shipped built-in resolver defers even literal IPs
through a 0 ms timer (`dns_resolver.c`, `dns_on_literal`) so it *never* completes
synchronously, and `resolver_cache.c` uses the documented `in_resolve`/`completed`
sentinel. Only a user-supplied sync-completing resolver that returns NULL trips
it. Pre-dates Phase 3 (the resolver path is older); surfaced by this audit.

**Fix:** detect sync completion via state and return the live handle instead of
freeing:
```c
c->state = KL_HCLIENT_RESOLVING;
KlResolveReq *rq = resolver->resolve(resolver, ev_ctx, resolve_host,
                                     resolve_port, dns_resolved, c);
if (c->state == KL_HCLIENT_DONE)   /* dns_resolved already ran; on_done fired */
    return c;                       /* caller owns c, frees via kl_client_free */
if (!rq) { /* genuine start failure */ ... free c ... return NULL; }
c->resolve_req = rq;
return c;
```

---

## Low

### L1 — `udp_parse_local` copies from a control message without validating `cmsg_len`
**`src/udp.c:260-268` and `:270-278`**

```c
if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
    struct in_pktinfo pi;
    memcpy(&pi, CMSG_DATA(cm), sizeof(pi));   /* no cmsg_len check */
```

Matches on level/type only, then copies the full struct. The source is the local
kernel (not a network attacker) and `control.buf` is sized
`CMSG_SPACE(sizeof(struct in6_pktinfo))` (the larger of the two), so any over-read
stays within the stack buffer — hence Low, not a memory-safety defect in
practice. Harden by gating each branch on
`cm->cmsg_len >= CMSG_LEN(sizeof(pi))`.

### L2 — Empty-valued response headers are merged into the next header's name
**`parsers/response_parser_llhttp.c:162-176`** (`resp_on_header_field`)

The client-side response parser detects header boundaries with the heuristic
"previous value was non-empty" (`if (p->hdr_value_len > 0) flush_header(...)`)
instead of wiring llhttp's `on_header_value_complete` callback (the request-side
parser does this correctly, `parser_llhttp.c:104`). For a response header with an
empty value (`X-Empty:\r\nX-Next: v\r\n`), no flush happens when `X-Next` arrives,
so its bytes are appended onto the still-open `X-Empty` name accumulator →
corrupt header `X-EmptyX-Next: v`, and the empty header is dropped. Not a
memory-safety issue (`accum_append` is overflow-guarded and NUL-terminates); a
functional correctness bug on the HTTP client response path. Fix: wire
`on_header_value_complete` as the boundary signal.

---

## Tooling results

| Check | Result |
|-------|--------|
| `make` (production, `-Werror`) | **PASS** — warning-free |
| `make analyze` (scan-build) | **PASS** — "No bugs found" |
| `make cppcheck` | style-only (const-param, staticFunction false-positives on public `kl_` API) — no error/warning |
| `make debug-test` (ASan+UBSan) | **PASS** — 797/797 tests, zero sanitizer output |
| dangerous funcs (`strcpy`/`sprintf`/`atoi`/`gets`/`alloca`/…) | **none** |
| VLAs | **none** |
| direct libc alloc outside `allocator.c` wrapper | **none** |
| production hardening | `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE` |

## Areas audited clean (no findings)

- **Happy Eyeballs (`client.c`):** no fd leaks (every `socket()` closed on all
  paths), no double-close (winner marked inactive before `he_close_attempts`;
  `c->fd == -1` guard on the racing all-fail path), no timer leak/stale-id misuse
  (monotonic ids, cancel-before-rearm, callbacks null their own id, both timers
  cancelled on every completion/cancel path), bounds safe (`naddrs` clamped to
  `KL_RESOLVE_MAX_ADDRS`, `conn_next < naddrs`), `conn_pending` accounting exact,
  correct KL_ERR_CONNECT vs KL_ERR_TIMEOUT, and safe re-entrancy on immediate
  connect (state-guarded loop).
- **DNS response parser (`dns_resolver.c`):** genuinely bounds-safe — every packet
  read length-checked before dereference; compression pointers skipped in place
  (never followed → no pointer-chase loop); 128-label cap; RDATA length validated
  before fixed 4/16-byte `memcpy`; `naddrs` capped in every write loop. Strong
  anti-spoof (txn-id + QR + rcode + source-is-nameserver + full 0x20 question
  echo). Two-leg state machine: no timer double-cancel, no request UAF across
  legs, `in_done`/`cancelled` reentrancy guard sound.
- **UDP (`udp.c`):** `msg_controllen` set before `CMSG_FIRSTHDR`; send-queue cap
  underflow/overflow-guarded; symmetric `q_bytes` accounting; correct fd lifecycle.
  Only L1 above.
- **Core parsers/modules:** `chunked.c`, `body_reader_multipart.c`,
  `parser_llhttp.c`, `body_reader_buffer.c`, `response.c`, `router.c`,
  `connection.c`, `server.c`, `request.h` — all clean; overflow guards present on
  every size computation (`SIZE_MAX/2`, `INT_MAX/2`, `SIZE_MAX/size`), header
  injection blocked (`contains_crlf` / `mp_has_ctl`), CL-vs-TE smuggling handled,
  alloc/free sizes matched, fds closed on all paths.

## Recommendations

1. Fix **H1** (small, mirrors the documented `resolver_cache.c` sentinel pattern)
   — closes a contract-conformance UAF even though no shipped path reaches it.
2. Harden **L1** (one length check per branch) and fix **L2** (wire the real
   llhttp completion callback; add a regression test for an empty response-header
   value).
3. No other action needed — the untrusted-network attack surface (DNS + HTTP
   parsers) is well-bounded and fuzzed.

## Resolution (2026-07-19)

All three findings fixed:

- **H1** — `kl_client_start_s` now detects synchronous resolver completion via
  `state != KL_HCLIENT_RESOLVING` after `resolve()` returns and hands back the
  live handle instead of freeing it; a NULL return is only treated as a start
  failure when the resolve genuinely deferred. Regression test
  `client.async_resolver_sync_complete_null_return`.
- **L1** — `udp_parse_local` now gates each pktinfo branch on
  `cmsg_len >= CMSG_LEN(sizeof(...))` before copying.
- **L2** — the response parser wires `on_header_value_complete` as the header
  boundary (matching the request parser) instead of the "prev value non-empty"
  heuristic, so empty-valued headers are preserved. Regression test
  `response_parser.empty_valued_header`. This exposed a latent NULL+0 pointer UB
  in `accum_append` (zero-length append to an unallocated buffer), fixed with an
  early return for `data_len == 0`.

Verification: full suite (813 tests), poll backend, ASan+UBSan (clean, incl. the
new empty-header path), scan-build (no bugs), cppcheck (style-only), gcc-14, and
`fuzz_response_parser` (60k runs, no crashes).
