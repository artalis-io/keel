# DNS Parity with getaddrinfo — Design

Status: **planned (phased).**
Decisions taken (2026-07-19): deliver all four resolver-internal features
(`/etc/hosts`, `search`/`ndots`, multi-nameserver failover, EDNS0) **and**
multiple-address return + Happy Eyeballs (RFC 8305).

This is a large body of work touching the resolver, the `KlResolver` vtable, and
the client connect path. It ships as a **phased sequence of CI-green commits**,
each independently testable — not one mega-commit.

---

## Phase 1a — `/etc/hosts` + EDNS0  *(DONE 2026-07-19)*

- **`/etc/hosts`** — before issuing a query, scan `/etc/hosts` for the name
  (case-insensitive), honoring `prefer_ipv6`. Generalizes the existing
  `localhost` shortcut (kept as a fallback for hosts files that omit it).
  Read per-resolve (the file is tiny and OS-cached; matches getaddrinfo).
  Completes via the existing deferred-literal path (0 ms timer → async).
- **EDNS0** — append an `OPT` pseudo-RR to each query advertising a larger UDP
  payload (1232 bytes, the DNS-flag-day value), `arcount = 1`. Reduces
  truncation. Response `OPT` in the additional section is ignored.

## Phase 1b — `search`/`ndots` + multi-nameserver failover  *(state-machine rework)*

*Split into two commits: **1b-i multi-nameserver failover (DONE 2026-07-19)** —
unconnected socket + per-transmit nameserver rotation + source-address
verification + resolv.conf `nameserver` list (with `IP#port` extension);
**1b-ii search/ndots (DONE 2026-07-19)** — candidate-name expansion from
`resolv.conf` `search`/`domain` + `options ndots:`, with the 3-level request
state machine (candidate → family → nameserver rotation).*


The request state machine currently has one dimension: **family** (A → AAAA).
These add two more:

- **Candidate names** (`search`/`ndots`) — parse `resolv.conf`
  `search`/`domain` + `options ndots:N` (default 1). For a name with fewer than
  `ndots` dots and no trailing `.`, build a candidate list: search-appended
  variants + the bare name (order per ndots). Try each candidate until one
  resolves; NXDOMAIN on one advances to the next.
- **Nameservers** — parse *all* `nameserver` lines (cap ~3). On timeout
  exhausting attempts for the current NS, fail over to the next (re-`connect()`
  the UDP socket for peer filtering). Both dimensions compose with the existing
  family fallback and retransmit/timeout logic.

The request grows: `{candidate[], nservers[], qtype, tries_left}` with a small
nested cursor. Careful ordering so one lookup issues a bounded number of queries.

## Phase 2 — multiple-address return  *(vtable change)*

Decisions (2026-07-19): **dual-family** — every resolve queries A *and* AAAA and
returns a merged, RFC 8305 §4-interleaved list; completion uses a **Resolution
Delay** cap (RFC 8305 §3) so a slow/absent family never stalls the result.

Split into two commits: **2a** (struct + list-parser + single-family collection)
and **2b** (dual-family concurrency + resolution delay + interleave).

### Struct (`KlResolveResult`) — clean list (no back-compat constraint)

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
resolver is unreleased, so there's no compatibility cost — the consumers move to
`addrs[0]` in 2a:

- `client.c` `dns_resolved`: `result->addr` → `result->addrs[0]`,
  `result->ai_family` → `result->addrs[0].ss_family`, etc. (small edit; the
  client uses only `addrs[0]` until Phase 3).
- `resolver_cache.c`: still copies the struct by value — no logic change.
- `test_dns_resolver.c` assertions: `g_res.addr`/`g_res.ai_family` →
  `g_res.addrs[0]`/`g_res.addrs[0].ss_family` (mechanical, ~15 sites).

Address sets larger than 8 are truncated to the first 8 (preferred-first).

### Parser

Add `kl_dns_parse_all(pkt, len, expect_id, want_qtype, expect_q, expect_q_len,
out_addrs[], out_lens[], max) -> count` — collects *all* records of `want_qtype`
(bounds-safe, fuzzed alongside `kl_dns_parse_response`, which stays as the
single-address form). The new list path gets its own `fuzz_dns` coverage.

### Phase 2a — struct + list, single-family collection  *(DONE 2026-07-19)*

- Introduce the struct + `kl_dns_parse_all`.
- On a successful response, collect *all* records of the answering family into
  the list (keep the existing A→AAAA fallback — one family per resolve for now).
- `resolver_cache` and the client are unchanged (client still uses `addr[0]`).
- Tests: a 3-A-record response fills `naddrs == 3` in order.

### Phase 2b — dual-family concurrency + resolution delay

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

## Phase 3 — Happy Eyeballs (RFC 8305)  *(client connect racing)*

The async client's connect path stages the address list:

- Start connecting to the first address; after a **Connection Attempt Delay**
  (~250 ms, one `KlTimer`) start the next if not yet connected; interleave
  families. First socket to finish the handshake wins; the rest are closed.
- Bounded parallelism (e.g. ≤ `naddrs`, cap 4 in flight). On all-fail, return
  the last error. Sequential fallback (delay = ∞) remains the degenerate case.

This is the riskiest phase (touches the client state machine) and lands last, on
top of a proven multi-address resolver.

---

## Sequencing

1. **1a** `/etc/hosts` + EDNS0 — small, no state-machine change.
2. **1b** `search`/`ndots` + multi-NS failover — resolver state-machine rework.
3. **2** multiple-address vtable + resolver list collection.
4. **3** client Happy Eyeballs.

Each phase: full gauntlet (`make test`, poll, ASan+UBSan, scan-build, cppcheck,
`fuzz_dns`) and CI-green before the next. Tests use the `KlUdpServer` mock
nameserver; `/etc/hosts` and `resolv.conf` parsing get unit tests over crafted
fixtures (a temp file path is injectable for testing).

## Non-goals (still)

TCP fallback on the `TC` bit, DNSSEC, DoT/DoH, DNS cookies — separate roadmap
items. EDNS0 here is buffer-size advertisement only (no extensions).
