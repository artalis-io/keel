# DNS Parity with getaddrinfo: Design

Status: **Complete: Phases 1a–3 done (2026-07-19).**
Decisions taken (2026-07-19): deliver all four resolver-internal features
(`/etc/hosts`, `search`/`ndots`, multi-nameserver failover, EDNS0) **and**
multiple-address return + Happy Eyeballs (RFC 8305).

This is a large body of work touching the resolver, the `KlResolver` vtable, and
the client connect path. It ships as a **phased sequence of CI-green commits**,
each independently testable, not one mega-commit.

---

## Phase 1a: `/etc/hosts` + EDNS0  *(DONE 2026-07-19)*

- **`/etc/hosts`**: before issuing a query, scan `/etc/hosts` for the name
  (case-insensitive), honoring `prefer_ipv6`. Generalizes the existing
  `localhost` shortcut (kept as a fallback for hosts files that omit it).
  Read per-resolve (the file is tiny and OS-cached; matches getaddrinfo).
  Completes via the existing deferred-literal path (0 ms timer → async).
- **EDNS0**: append an `OPT` pseudo-RR to each query advertising a larger UDP
  payload (1232 bytes, the DNS-flag-day value), `arcount = 1`. Reduces
  truncation. Response `OPT` in the additional section is ignored.

## Phase 1b: `search`/`ndots` + multi-nameserver failover  *(state-machine rework)*

*Split into two commits: **1b-i multi-nameserver failover (DONE 2026-07-19)**:
unconnected socket + per-transmit nameserver rotation + source-address
verification + resolv.conf `nameserver` list (with `IP#port` extension);
**1b-ii search/ndots (DONE 2026-07-19)**: candidate-name expansion from
`resolv.conf` `search`/`domain` + `options ndots:`, with the 3-level request
state machine (candidate → family → nameserver rotation).*


The request state machine currently has one dimension: **family** (A → AAAA).
These add two more:

- **Candidate names** (`search`/`ndots`): parse `resolv.conf`
  `search`/`domain` + `options ndots:N` (default 1). For a name with fewer than
  `ndots` dots and no trailing `.`, build a candidate list: search-appended
  variants + the bare name (order per ndots). Try each candidate until one
  resolves; NXDOMAIN on one advances to the next.
- **Nameservers**: parse *all* `nameserver` lines (cap ~3). On timeout
  exhausting attempts for the current NS, fail over to the next (re-`connect()`
  the UDP socket for peer filtering). Both dimensions compose with the existing
  family fallback and retransmit/timeout logic.

The request grows: `{candidate[], nservers[], qtype, tries_left}` with a small
nested cursor. Careful ordering so one lookup issues a bounded number of queries.

## Phase 2: multiple-address return  *(vtable change)*

Decisions (2026-07-19): **dual-family**: every resolve queries A *and* AAAA and
returns a merged, RFC 8305 §4-interleaved list; completion uses a **Resolution
Delay** cap (RFC 8305 §3) so a slow/absent family never stalls the result.

Split into two commits: **2a** (struct + list-parser + single-family collection)
and **2b** (dual-family concurrency + resolution delay + interleave).

### Struct (`KlResolveResult`): clean list (no back-compat constraint)

```c
#define KL_RESOLVE_MAX_ADDRS 8
typedef struct {
    struct sockaddr_storage addrs[KL_RESOLVE_MAX_ADDRS]; /* preferred-first, family-interleaved */
    socklen_t               addrlens[KL_RESOLVE_MAX_ADDRS];
    int                     naddrs;
    int                     ai_socktype;   /* shared: SOCK_STREAM */
    int                     ai_protocol;   /* shared: 0 */
} KlResolveResult;
```

The old single `addr`/`addrlen`/`ai_family` triple is **dropped**, not kept as
`addrs[0]`: a lone `ai_family` can't honestly describe a mixed v4/v6 list, so a
per-address family (`addrs[i].ss_family`) is both cleaner and more correct. The
resolver is unreleased, so there's no compatibility cost; the consumers move to
`addrs[0]` in 2a:

- `client.c` `dns_resolved`: `result->addr` → `result->addrs[0]`,
  `result->ai_family` → `result->addrs[0].ss_family`, etc. (small edit; the
  client uses only `addrs[0]` until Phase 3).
- `resolver_cache.c`: still copies the struct by value; no logic change.
- `test_dns_resolver.c` assertions: `g_res.addr`/`g_res.ai_family` →
  `g_res.addrs[0]`/`g_res.addrs[0].ss_family` (mechanical, ~15 sites).

Address sets larger than 8 are truncated to the first 8 (preferred-first).

### Parser

*Planned:* a separate `kl_dns_parse_all(...)` collecting *all* records of a type.
**As implemented:** the list collection was folded directly into
`kl_dns_parse_response` (it fills `out->addrs[]` up to `KL_RESOLVE_MAX_ADDRS`), so
no separate function was added; `kl_dns_parse_response` is the single fuzzed
(`fuzz_dns`) bounds-safe parser.

### Phase 2a: struct + list, single-family collection  *(DONE 2026-07-19)*

- Introduce the struct + `kl_dns_parse_all`.
- On a successful response, collect *all* records of the answering family into
  the list (keep the existing A→AAAA fallback; one family per resolve for now).
- `resolver_cache` and the client are unchanged (client still uses `addr[0]`).
- Tests: a 3-A-record response fills `naddrs == 3` in order.

### Phase 2b: dual-family concurrency + resolution delay  *(DONE 2026-07-19)*

The request gains **two legs** (A and AAAA), each with its own transaction id,
retry/nameserver-rotation state, timeout timer, question bytes, and `done` flag:

```c
typedef struct { uint16_t id; int tries_left, ns_idx; int64_t timer_id;
                 int done; uint8_t question[...]; size_t question_len; } KlDnsLeg;
```

- **resolve / next candidate**: fire *both* legs concurrently.
- **recv**: match the response id to a leg; collect that leg's records
  (inserted family-interleaved into `result`); mark the leg `done`; on the first
  addresses, arm the Resolution-Delay timer (~50 ms).
- **complete** when: both legs are `done`, **or** the resolution-delay fires with
  ≥1 address collected. Merge is already interleaved (§4: preferred family
  first, then alternate).
- **both legs empty** (NXDOMAIN / no-record / timeout on both) → advance to the
  next candidate (fire both legs again); fail when candidates exhaust.
- **cancel / destroy**: cancel both leg timers + the resolution-delay timer.

State machine after 2b: candidate → {A-leg, AAAA-leg concurrent} → per-leg
nameserver rotation × attempts. The single-active-query model of 1b becomes a
two-leg model; `dns_find_by_id` matches a leg within a request.

### Effort

2a ≈ 1 day (struct + parser + collection + tests); 2b ≈ 2 days (two-leg state
machine + resolution delay + interleave + tests). Each ships CI-green, gcc-14
verified before handoff.

## Phase 3: Happy Eyeballs (RFC 8305)  *(client connect racing)*  *(DONE 2026-07-19)*

Decisions (2026-07-19): **full racing** (not sequential-only); Connection Attempt
Delay is a **configurable** `KlClientConfig` field (0 = 250 ms default); and this
phase **adds the async client's first overall deadline timer** (today the async
path enforces no timeout at all).

### The gap today

`dns_resolved()` uses `addrs[0]` only → `start_connect()` → a single `c->fd` in
`KL_HCLIENT_CONNECTING`. `async_handle_connecting()` maps any `SO_ERROR` straight
to `async_complete_error()`, so `addrs[1..]` are never tried. The async client
therefore (a) ignores all but the first address and (b) gives up on the first
connect failure, no fallback, let alone racing. Phase 2b delivers a
family-interleaved, preferred-first list; Phase 3 makes the client consume it.

### Structural change: one connecting fd → a set of racing attempts

`KL_HCLIENT_CONNECTING` generalizes from a single fd to a small set. New
`KlClient` fields:

```c
typedef struct { int fd; int active; } KlConnAttempt;   /* ≤ KL_RESOLVE_MAX_ADDRS */
KlResolveResult conn_addrs;      int conn_next;          /* full list + cursor */
KlConnAttempt   conn_attempts[KL_RESOLVE_MAX_ADDRS];
int             conn_pending;    int64_t conn_delay_timer;   /* -1 sentinel */
int64_t         deadline_timer;  KlError conn_last_err;      /* -1; all-fail err */
```

The watcher callback keeps `user_data = client`; the firing attempt is found by a
linear scan of `conn_attempts` (≤ 8, matches the codebase's O(n)-over-tiny-array
philosophy); no per-attempt allocation.

### Flow

1. **dns_resolved** copies the whole `*result` into `conn_addrs`, arms the overall
   `deadline_timer` (`timeout_ms`), then `he_start_next()` + arms the Connection
   Attempt Delay timer.
2. **he_start_next**: nonblocking `connect()` on `addrs[conn_next++]`: `rc==0` →
   immediate win; `EINPROGRESS` → WRITE watcher + record fd, `conn_pending++`;
   hard error / `socket()` fail → recurse to the next address (no delay consumed).
   Re-arm the delay timer.
3. **he_on_delay**: if a next address exists and there's no winner yet, start it
   (which re-arms). Staggered starts bound in-flight sockets (list ≤ 8; no
   separate in-flight cap needed).
4. **watcher (CONNECTING)**: find attempt by fd; `SO_ERROR==0` → **winner**;
   nonzero → close that attempt, `conn_pending--`, stash `conn_last_err`, and if
   another address remains start it *immediately* (a failure must not wait out the
   delay, §5); when `conn_pending==0 && list exhausted` → `KL_ERR_CONNECT`.
5. **he_win(fd)**: cancel the delay timer, close + `kl_watcher_del` **all
   losers**, set `c->fd = fd`, then fall into the *existing* proxy/TLS/SENDING
   transition unchanged.
6. **deadline_timer fires** at any point before completion → tear down (all
   attempts or the live fd) → `KL_ERR_TIMEOUT`. Cancelled on completion/win-to-
   done. This covers connect racing **and** the previously-untimed TLS handshake /
   send / recv states.

### Config + constant

```c
#define KL_CLIENT_CONNECT_ATTEMPT_DELAY_MS 250   /* RFC 8305 §5 default; min 10, max 2000 */
/* KlClientConfig gains: int connect_attempt_delay_ms;  (0 = default) */
```

A configurable delay also makes the timing test deterministic (small value forces
a fast race; a huge value degenerates to sequential).

### Orthogonality

- No client-side sorting; the resolver already delivers the §4 interleave; the
  client walks the list in order.
- Everything downstream of connect (TLS, proxy, streaming, pool, decompress) is
  untouched; the sync path stays single-address getaddrinfo+connect.
- `naddrs==1` degenerates to today's single connect (the delay timer never has a
  next to start).
- Proxy is address-list-agnostic; it races whatever list `dns_resolved` received.
- Teardown / `kl_client_cancel` additionally cancels both timers and closes every
  `active` attempt (not just `c->fd`).

### Tests  *(mock resolver returns two addrs → local listeners)*

- **he_first_wins**: addr[0] live → wins; addr[1] never needed.
- **he_fallback_on_refused**: addr[0] = closed port (ECONNREFUSED) → *fast*
  failover (no delay wait) to a live addr[1]; request succeeds.
- **he_second_wins_on_slow_first**: addr[0] a stalled/black-hole connect, addr[1]
  live, small configured delay → addr[1] dialed after the delay and wins while
  addr[0] still pending; total ≈ delay, not the OS connect timeout.
- **he_all_fail**: both refused → `KL_ERR_CONNECT`.
- **he_single_address**: `naddrs==1` unchanged.
- **deadline_fires**: all addresses black-hole with a short `timeout_ms` →
  `KL_ERR_TIMEOUT` (not an indefinite hang).
- **ASan**: no fd/watcher/timer leak when a winner closes losers, and on
  mid-race cancel.

This is the riskiest phase (multi-fd in a previously single-fd client) and lands
last, on the proven multi-address resolver. ~2 days.

---

## Sequencing

1. **1a** `/etc/hosts` + EDNS0: small, no state-machine change.
2. **1b** `search`/`ndots` + multi-NS failover: resolver state-machine rework.
3. **2** multiple-address vtable + resolver list collection.
4. **3** client Happy Eyeballs.

Each phase: full gauntlet (`make test`, poll, ASan+UBSan, scan-build, cppcheck,
`fuzz_dns`) and CI-green before the next. Tests use the `KlUdpServer` mock
nameserver; `/etc/hosts` and `resolv.conf` parsing get unit tests over crafted
fixtures (a temp file path is injectable for testing).

## Non-goals (still)

TCP fallback on the `TC` bit, DNSSEC, DoT/DoH, DNS cookies: separate roadmap
items. EDNS0 here is buffer-size advertisement only (no extensions).
