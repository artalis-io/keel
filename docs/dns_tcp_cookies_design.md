# DNS TCP fallback (RFC 7766) + Cookies (RFC 7873) — Design

Status: **Phase 1 (persistent TCP fallback) implemented (2026-07-19); Phase 2
(cookies) pending.**
Decisions: TCP fallback uses **persistent/pooled** per-nameserver connections with
pipelining + idle close; DNS cookies are **on by default** with client-cookie
verification and a **BADCOOKIE retry**.

Closes the two remaining DNS robustness gaps on the built-in resolver: a
truncated UDP response currently can't be recovered (no TCP fallback), and there
is no cookie-based off-path spoof resistance beyond txn-id + 0x20 + source check.

Delivered as **two CI-green commits**: Phase 1 (TCP fallback), Phase 2 (cookies).

---

## Background — current resolver

UDP-only, leg-based: each resolve fires an A leg and an AAAA leg over one shared
unconnected `KlUdp sock`, rotating nameservers (`ns[DNS_MAX_NS]`) with per-leg
timeout/retransmit. Queries carry an EDNS0 OPT RR (empty options, 1232 UDP
payload). `dns_on_recv` matches a response to a leg by txn-id, checks
source-is-nameserver + QR + rcode + 0x20 question-echo, and settles the leg. The
**TC (truncation) bit is not inspected**, and there is no cookie state.

## Phase 1 — persistent TCP fallback (RFC 7766) — **implemented**

Landed in `src/dns_resolver.c` as a self-contained `KlDnsTcp tcp[DNS_MAX_NS]`
connection manager. All transport I/O goes through `dns_tcp_write`/`dns_tcp_read`,
which branch on `t->tls` (NULL today — the DoT hook). Tests:
`dns.tcp_fallback` (TC → recover over TCP), `dns.tcp_persistent_reuse` (two A+AAAA
legs pipeline over one connection; a second resolve reuses the idle-cached
connection — single accept), `dns.tcp_drop` (connection drop → pending legs settle
empty, resolve completes rather than hanging).

### Trigger
In `dns_on_recv`, after matching the leg, test `pkt[2] & 0x02` (TC). If set,
don't settle — route the leg to the TCP path for the nameserver it used.

### Per-nameserver persistent connection
A `KlDnsTcp tcp[DNS_MAX_NS]` on the resolver — one reusable connection per NS:

```c
typedef struct {
    int       fd;            /* -1 = closed */
    int       state;         /* CLOSED / CONNECTING / READY */
    unsigned char *wbuf; size_t wlen, wsent;  /* queued framed queries (2-byte len + msg) */
    size_t    wcap;
    unsigned char *rbuf; size_t rcap, rlen;   /* accumulated inbound bytes */
    size_t    rneed;         /* current message length from the 2-byte prefix (0 = need prefix) */
    int64_t   idle_timer;    /* close after inactivity when outstanding == 0 */
    int       outstanding;   /* in-flight queries pipelined on this connection */
} KlDnsTcp;
```

Flow (driven by a `KlWatcher` on `fd`):
- **Open on demand:** first TC fallback to an NS opens a nonblocking `socket` +
  `connect` (EINPROGRESS → CONNECTING). Subsequent fallbacks reuse a READY conn.
- **Enqueue:** append the leg's framed query (`[u16 len][message]`) to `wbuf`,
  `outstanding++`, arm WRITE interest, cancel the leg's UDP timer, arm a TCP
  response timer, and flag `leg->tcp_pending`.
- **Writable:** on CONNECTING, check `SO_ERROR` → READY (or fail all pending
  legs). Flush `wbuf`; drop WRITE interest when drained.
- **Readable:** append to `rbuf`; frame by the 2-byte length prefix — for each
  complete message, `kl_dns_parse_response`, `dns_find_leg(id)`, settle it,
  `outstanding--`. Multiple pipelined responses (out of order) are handled by
  txn-id matching.
- **Idle:** when `outstanding == 0`, arm `idle_timer` (a few seconds); on fire,
  close. Any late byte re-opens on the next fallback.
- **Drop/error:** on read EOF or a socket error with pending legs, settle those
  legs empty (best effort — the other family's leg may still have answered) and
  close. `rbuf`/`wbuf` freed on close (lazily allocated on first use).

### Leg additions
`KlDnsLeg` gains `int tcp_pending`. It reuses its existing `id`/`question`; the
TCP query is rebuilt (fresh 0x20 casing is fine — the response echoes what we
send, and `leg->question` is updated to match). The per-leg timer bounds the TCP
exchange; TCP settle reuses the existing `dns_leg_settle` path.

## Phase 2 — DNS cookies (RFC 7873)

### Per-NS cookie state
```c
typedef struct {
    uint8_t client[8];       /* random, generated once per NS */
    uint8_t server[32];      /* learned from responses */
    uint8_t server_len;      /* 0 = none yet */
    uint8_t have_client;
} KlDnsCookie;               /* cookie[DNS_MAX_NS] on the resolver */
```
Client cookie: 8 random bytes from the existing entropy pool, per nameserver
(RFC-compliant; we do not use the §B.2 keyed-hash derivation — noted non-goal).

### Query build
`dns_build_query` takes the NS index and adds a COOKIE option to the OPT rdata:
option-code `10`, option-data = client(8) [+ server(server_len)]; OPT `rdlen`
becomes `4 + optlen` (was 0). Non-cookie servers ignore the option.

### Response handling
A bounds-safe `dns_extract_cookie` walks the additional section to the OPT RR,
finds the COOKIE option, and returns the echoed client cookie + server cookie.
`dns_on_recv`: verify the echoed client cookie equals ours (a strong off-path
anti-spoof signal layered on txn-id/0x20/source); store the server cookie for
that NS. A missing cookie (server without support) is accepted (backward compat);
a mismatched client cookie is treated as a spoof and ignored.

### BADCOOKIE
Full rcode = `(OPT extended-rcode << 4) | header rcode`; `23` = BADCOOKIE. On
BADCOOKIE the response carries a fresh server cookie — store it and re-transmit
the query once with the updated cookie (bounded so it can't loop).

## Orthogonality & long-term fit (DoT / DoH)

Reviewed before implementing so the TCP transport composes cleanly and doesn't
have to be rewritten for encrypted DNS.

**Orthogonal to the UDP path.** TCP is a *separate transport* attached to the
existing leg state machine at exactly one seam — the settle point in
`dns_on_recv`, where a `TC`-bit response switches the leg from "UDP settle" to
"TCP settle". The UDP transmit/retransmit/rotation logic is untouched; the
`KlDnsTcp` connection manager is self-contained; a leg gains a single
`tcp_pending` flag. Cookies are likewise orthogonal — an EDNS0 OPT option +
per-NS state, invisible to the transport.

**Builds on the existing connection/transport primitives — not a parallel stack.**
- The DNS-over-TCP connection carries `int fd` + `KlTls *tls` and does *all* I/O
  through a `(fd, KlTls*)` read/write helper with the same shape as the HTTP
  client's `io_read`/`io_write` (plain `send`/`recv` when `tls == NULL`, else the
  `KlTls` vtable). Idle timeout uses `KlTimer`, events use `KlWatcher`, timing
  uses the monotonic clock — the same shared primitives, no reinvention.
- It reuses the pluggable **`KlTls`** transport vtable that TLS server/client
  already use, so encryption is a wrapping concern, not a fork.

**DoT = a wrapping increment, not a rewrite.** Because the connection already
speaks through the `(fd, KlTls*)` abstraction, DNS-over-TLS (RFC 7858, TCP+TLS
port 853) is: add a `KlTlsConfig` hook to `KlDnsResolverConfig`, create a `KlTls`
via its factory on connect, and add a handshake step before the first framed
write. Phase 1 leaves `tls == NULL` (plain Do53/TCP) but routes through the same
helpers so the hook drops in. Same 2-byte framing, same pipelining, same idle
pool.

**DoH stays a separate resolver (correct layering).** DNS-over-HTTPS is *not*
part of the built-in UDP/TCP resolver — per the roadmap it "rides the existing
HTTPS client" as its own `KlResolver` implementation. That is precisely how it
reuses `KlClientPool`: a DoH resolver issues pooled HTTPS requests
(`kl_client_start_pooled`), inheriting HTTP keep-alive connection reuse for free.
Forcing the built-in resolver's pipelined, length-framed DNS-over-TCP into the
host-keyed HTTP idle-cache (`KlClientPool`) would break orthogonality for no gain;
the per-NS persistent connection here is the RFC-standard "pool" for DNS
transports. So: **HTTP pool reuse lands where it belongs (the DoH resolver);
the TCP transport reuses the shared TLS/timer/watcher primitives.**

Net: Phase 1 introduces one self-contained, TLS-ready DNS transport;
DoT bolts a `KlTls` onto it; DoH arrives as an independent resolver over the
already-pooled HTTP client. All three compose without touching each other.

## Non-goals

DoT/DoH transports (separate roadmap items — this design only ensures they slot
in), TCP pipelining fairness/limits beyond basic id-multiplexing, and RFC 7873
§B.2 keyed client-cookie derivation.

## Tests (`tests/test_dns_resolver.c` additions)

Driven on the event loop with mock nameservers:
- **TCP fallback:** a mock TCP DNS server (accept → read framed query → reply
  framed). A UDP response with TC set triggers the resolver to retry over TCP and
  resolve. Plus: persistent reuse (two queries → one connection), idle close, and
  connection-drop → leg settles empty.
- **Cookies:** a mock UDP NS that echoes the client cookie + adds a server
  cookie → assert the resolver stores it and sends it on the next query; a
  BADCOOKIE response → assert the retry with the returned server cookie; a
  mismatched client cookie → assert rejection.

Static analysis (the new TCP + OPT parsing is Linux/untrusted-adjacent) runs the
exact CI `scan-build make clean all` + cppcheck 2.13 in containers before push.

## Delivery

Phase 1 (persistent TCP) ≈ larger; Phase 2 (cookies) ≈ medium. Each ships
CI-green, verified on macOS + a Linux container.
