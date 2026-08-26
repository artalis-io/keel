# Client-Identity Trio: Design

Status: **A + B + C done**.
Author: design agreed 2026-07-17.

Exposes *who is on the other end* of a connection to handlers, the TCP/TLS
analogs of the UNIX-socket peer-credential work shipped in 2.8.0. Three
independent features:

- **A. Remote address**: expose the client IP/port to handlers.
- **B. PROXY protocol (v1 + v2)**: recover the real client address behind an
  L4 load balancer, trust-gated by a CIDR allowlist.
- **C. TLS client certificate**: surface the verified mTLS client identity.

Decisions taken:
- PROXY trust: **CIDR allowlist** (nginx `set_real_ip_from` semantics), not a
  blanket per-listener boolean.
- PROXY versions: **v1 + v2** (v2 is what AWS NLB / modern HAProxy send).
- TLS client cert shape: **parsed fields + raw DER** (both).

---

## 0. Shared foundation

`KlConn` gains the effective peer address (from the socket, or overridden by a
trusted PROXY header):

```c
struct sockaddr_storage peer_addr;
socklen_t               peer_addr_len;
uint8_t                 peer_source;   /* KL_PEER_SOCKET | KL_PEER_PROXY (added with B) */
```

The accept loop captures it (`accept(fd, &peer, &len)` instead of `NULL, NULL`);
reset on pool reuse. WebSocket/H2 connections inherit it (same `KlConn`).

---

## A. Remote address  *(shipping first)*

```c
/* "203.0.113.7" / "2001:db8::1" + port. Returns -1 for AF_UNIX (use peer creds). */
int kl_request_peer_addr(const KlRequest *req, char *ip, size_t iplen, uint16_t *port);
/* Raw sockaddr for advanced use (family, IPv6 scope id). */
const struct sockaddr *kl_request_peer_sockaddr(const KlRequest *req, socklen_t *len);
```

`inet_ntop` over `conn->peer_addr`. The access-log callback already receives the
`req`, so client IP flows into structured logs once the accessor exists.

Tests: loopback client → `127.0.0.1` + port; IPv6; AF_UNIX → -1.

---

## B. PROXY protocol (v1 + v2, CIDR-gated)  *(done)*

Implementation notes: `src/proxy_protocol.c` holds the pure v1/v2 parser +
CIDR list; `KL_CONN_PROXY_HEADER` runs before TLS/HTTP and uses `MSG_PEEK` to
detect + size the header, then consumes exactly it. After the header, a 1-byte
`MSG_PEEK` decides whether to process already-buffered bytes this tick or wait
for the next event (edge-triggered epoll/kqueue don't re-deliver consumed
readiness). `peer_source` records SOCKET vs PROXY.


Config:
```c
const char *proxy_trusted_cidrs;  /* "10.0.0.0/8,192.168.0.0/16,::1/128"; NULL = off */
```
Parsed at `kl_server_init` into `{family, prefix[16], prefix_bits}[]`. Needs a
small CIDR utility (~100 LOC: parse IPv4/IPv6 CIDR, prefix-bit match).

Trust model (nginx real_ip semantics):
1. On accept, match the raw socket peer against the trusted list.
2. Trusted **and** leading bytes match a PROXY signature (`"PROXY "` v1, or the
   12-byte binary v2 sig) → parse it, set `peer_addr`, `peer_source = KL_PEER_PROXY`.
3. Otherwise → direct connection (socket addr). A spoofed header from an
   untrusted peer just becomes invalid HTTP → 400/close.

New state `KL_CONN_PROXY_HEADER` runs **before** TLS handshake / HTTP read (PROXY
wraps the raw stream; TLS is inside it). Handles split-across-reads with a
bounded buffer.
- v1: text, ≤107 B, `PROXY TCP4|TCP6|UNKNOWN <src> <dst> <sport> <dport>\r\n`;
  `UNKNOWN` → keep socket addr.
- v2: 12-byte sig + ver/cmd + family/proto + 2-byte len + address block; `LOCAL`
  cmd (health checks) → keep socket addr; ignore TLVs.
- Malformed / over-length / truncated-under-timeout → close.

Security: CIDR gate + signature check closes PROXY spoofing; hard length caps
bound DoS; malformed CIDR at init → `KL_ERR_INVALID_ARG`.

---

## C. TLS client certificate (mTLS)  *(done)*

Implementation notes: `KlTls` gains an optional `peer_cert` vtable method
(NULL-able like `alpn_protocol`); `kl_request_peer_cert()` delegates to it.
The mbedtls backend (`src/tls_mbedtls.c`) implements extraction via
`mbedtls_ssl_get_peer_cert` + `mbedtls_ssl_get_verify_result`, walking the
subject/issuer RDNs for CN, the subjectAltName sequence for DNS/IP SANs, a
SHA-256 over the raw DER for the fingerprint, and a TZ-free civil-time
conversion for the validity window. The mTLS auth mode is wired through the
existing `kl_tls_mbedtls_ctx_create(..., KlMtlsMode, ...)` config
(`KL_MTLS_NONE/OPTIONAL/REQUIRED` → `MBEDTLS_SSL_VERIFY_*`).

Testing: `tests/test_peer_cert.c` validates the handler→vtable plumbing with a
passthrough TLS mock (runs in the default/CI build). The real mbedtls
extraction was verified end-to-end against a live mTLS handshake and
cross-checked field-by-field against `openssl x509` output (fingerprint,
validity timestamps, SANs all matched), the mbedtls backend is not in the
default/CI build, so that check is run manually with `KEEL_TLS=mbedtls`.

Optional vtable method on `KlTls` (NULL-able like `alpn_protocol`):
```c
typedef struct {
    int     verified;                /* passed CA verification */
    char    subject_cn[256];
    char    san[512];                /* comma-separated DNS/IP SANs */
    char    issuer_cn[256];
    char    fingerprint_sha256[65];  /* hex */
    int64_t not_before, not_after;   /* unix time */
    const uint8_t *der; size_t der_len;  /* raw cert, valid until session reset/destroy */
} KlPeerCert;

/* struct KlTls: */  int (*peer_cert)(KlTls *self, KlPeerCert *out);  /* 0 filled, -1 none */
```
Backend does the X.509 extraction (mbedtls: `mbedtls_ssl_get_peer_cert` +
verify_result). mTLS config on the mbedtls ctx factory:
`client_cert_mode = none | optional | required` → wires
`MBEDTLS_SSL_VERIFY_NONE/OPTIONAL/REQUIRED`.

Handler API: `int kl_request_peer_cert(const KlRequest *req, KlPeerCert *out);`
(`verified` surfaced explicitly so "cert present" ≠ "cert valid").

---

## Sequencing & effort
1. **A, remote addr** (~½ day): standalone, no deps.
2. **B, PROXY protocol** (~1.5 days): CIDR util + v1/v2 parser + pre-TLS phase.
3. **C, mTLS cert** (~1.5 days): vtable slot + mbedtls extraction + config.

Each ships as its own commit, CI-green, mirroring the UNIX-socket rollout.
