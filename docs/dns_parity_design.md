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

`KlResolveResult` gains an address **list** while keeping `addr`/`addrlen`/
`ai_family` as the primary (first) entry for source compatibility:

```c
#define KL_RESOLVE_MAX_ADDRS 8
typedef struct {
    struct sockaddr_storage addr;      /* = addrs[0]; existing consumers keep working */
    socklen_t               addrlen;
    int ai_family, ai_socktype, ai_protocol;
    struct sockaddr_storage addrs[KL_RESOLVE_MAX_ADDRS];  /* full list */
    socklen_t               addrlens[KL_RESOLVE_MAX_ADDRS];
    int                     naddrs;
} KlResolveResult;
```

- The resolver collects *all* A and AAAA records from the response(s) and
  interleaves families (AAAA-first when `prefer_ipv6`, else A-first), RFC 8305
  §4 ordering.
- `resolver_cache` caches the whole list. The client keeps using `addr` (first)
  until Phase 3. Existing single-address consumers are unaffected.

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
