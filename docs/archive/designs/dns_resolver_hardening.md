# DNS Resolver Hardening + Default Wiring: Design

Status: **done**; hardening (#2) and opt-out default wiring (#4) both shipped.
Decisions taken (2026-07-18):
- **#4 wiring:** *opt-out default*, the async client uses the built-in async
  resolver by default; a flag restores blocking `getaddrinfo`.
- **#2 hardening:** *core + DNS 0x20*; randomized transaction IDs, response
  question verification, and 0x20 case randomization.

Two focused commits (hardening first, then wiring on top), or one bundle.

---

## #2: Anti-spoofing hardening (`dns_resolver`)

Today the resolver matches responses on the 16-bit transaction ID alone, and
that ID is a predictable sequence (`next_id` from 1). The socket is `connect()`-ed
to the nameserver, so the kernel already drops any datagram not from that peer,
which defeats *off-path* spoofers who can't forge the nameserver's source IP.
The hardening below closes the remaining gaps (predictable ID, no question
binding) and adds defense-in-depth.

### 2a. Randomized transaction IDs

Replace the sequential counter with OS entropy:

- Draw IDs from a small per-resolver entropy pool refilled via `getentropy()`
  (portable across Linux/glibc, macOS, and Cosmopolitan; fall back to
  `arc4random_buf` where `getentropy` is unavailable, and to a `/dev/urandom`
  read as a last resort).
- On the rare 16-bit collision with an already-in-flight query, redraw.

### 2b. Response question verification

Bind each response to its query, not just the ID:

- The request stores the exact **question-section wire bytes** it transmitted
  (encoded QNAME + QTYPE + QCLASS, including 0x20 case, see 2c).
- On receipt, after the ID matches, the parser verifies the response's question
  section equals the stored bytes **exactly**. Mismatch → the response is
  ignored (not completed); the query keeps waiting until timeout.
- `kl_dns_parse_response` gains two optional params: `const uint8_t *expect_q,
  size_t expect_q_len`. When non-NULL it byte-compares the response question
  against them; NULL preserves the current answer-only behavior (tests/fuzz).
  The parser already walks the question section bounds-safely, so this is a
  compare, not new traversal. The fuzz harness exercises both modes.

### 2c. DNS 0x20 encoding

- When building a query, randomly upper/lower-case each ASCII letter of the
  QNAME (using entropy bits). Compliant recursive resolvers echo the question
  verbatim, so the exact-case compare in 2b enforces the echo, an off-path
  spoofer must reproduce the case pattern as well as the ID and source port.
- **Compatibility:** a minority of resolvers/middleboxes normalize case and
  would fail the exact-case check. Because our connected socket already blocks
  off-path packets, 0x20's marginal value here is modest, and the compat risk is
  real. Mitigations: (1) a `KlDnsResolverConfig.disable_0x20` escape hatch, and
  (2) documented behavior that a 0x20 mismatch is treated as a (safe) non-answer
  → the query times out and the caller can retry. We ship it on by default per
  the decision, with the escape hatch for misbehaving resolvers.

### Source port

Already ephemeral and OS-randomized (the socket isn't explicitly bound; the
local port is assigned at `connect()`). No change; noted for completeness.

### Tests (via the `KlUdpServer` mock nameserver)

- **Wrong question, right ID** → response rejected (query times out), proving
  ID-only matching no longer suffices.
- **0x20 case mismatch** → rejected; **exact echo** → accepted.
- **`disable_0x20`** → case-insensitive path still resolves.
- IDs are not the old `1,2,3…` sequence (sanity check on non-predictability).

---

## #4: Opt-out default wiring (`client`)

### Resolution selection (async client)

Precedence, highest first:

1. `KlClientConfig.resolver != NULL` → use it (explicit, shareable; unchanged).
2. `KlClientConfig.system_dns` set → **blocking `getaddrinfo`** (the previous
   NULL behavior; preserves `/etc/hosts` + search domains, at the cost of
   stalling the event loop during resolution).
3. **default** (resolver NULL, `system_dns` 0) → the async client **lazily
   creates and owns** a built-in `KlDnsResolver` from its `KlEventCtx`.

The sync client is unchanged: it always uses blocking `getaddrinfo` (an async
UDP resolver is meaningless without an event loop).

### Lifecycle

- The auto-created resolver is built on first resolve via
  `kl_dns_resolver_create(ev_ctx, NULL)` (nameserver from `/etc/resolv.conf`),
  stored on the `KlClient`, and freed on client teardown.
- **Ownership is per-client** (one UDP socket per client). For many concurrent
  clients, share one explicit `resolver` (precedence #1): documented, since the
  default trades a little efficiency for zero-config non-blocking DNS.

### Why this is still a net win

The previous NULL path did a **blocking** `getaddrinfo` *inside the async client*
, stalling the whole event loop during every lookup. The new default is
non-blocking end to end. The behavioral change is resolution *semantics*
(`/etc/hosts`, search domains), addressed below.

### Regression mitigations (opt-out makes built-in DNS live by default)

Our resolver doesn't yet do `/etc/hosts` or search domains (roadmap #5), so
opt-out could break names that `getaddrinfo` resolves. Mitigations shipped here:

- **`localhost` shortcut**, resolve `localhost` → `127.0.0.1` / `::1` directly
  (it's usually not in DNS). Covers the most common breakage.
- **Literal IPs**: already shortcut.
- **`system_dns` escape hatch**: anyone needing `/etc/hosts`/search *now* sets
  the flag and gets the old behavior.
- **Compatibility note in the changelog**: existing async-client users (incl.
  Hull) switch from inline blocking `getaddrinfo` to built-in UDP DNS. Strictly
  better for loop responsiveness; different resolution semantics.

Full `/etc/hosts` + `resolv.conf` `search`/`ndots` remain **roadmap #5**, after
which opt-out has no remaining semantic gap.

### Tests

- Async client resolves a name via the built-in resolver by default (mock
  nameserver + local `KlServer`, fetch by hostname).
- `system_dns` → uses `getaddrinfo` (resolves `localhost`).
- Explicit `resolver` takes precedence over both.
- `localhost` shortcut resolves without a DNS query.
- Auto-created resolver is freed on teardown (ASan, incl. teardown mid-resolve).

---

## Effort & sequencing

1. **#2 hardening** (~1 day): entropy IDs, question storage + verify, 0x20,
   parser signature evolution + fuzz-harness update, mock-nameserver tests.
2. **#4 wiring** (~1 day): `system_dns` flag, lazy create/own/free in the async
   client, `localhost` shortcut, precedence tests.

Each ships CI-green, mirroring the UDP arc. Full gauntlet (`make test`, poll,
ASan+UBSan, scan-build, cppcheck, `fuzz_dns`) per commit.

---

## Non-goals (this work)

- `/etc/hosts`, `resolv.conf` `search`/`ndots`, multiple-nameserver failover:
  **roadmap #5**.
- EDNS0, TCP fallback on truncation, DNSSEC, DoT/DoH; roadmap.
- DNS cookies (RFC 7873), a stronger anti-spoof than 0x20; roadmap if needed.
