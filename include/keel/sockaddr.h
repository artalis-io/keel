#ifndef KEEL_SOCKADDR_H
#define KEEL_SOCKADDR_H

#include <stdint.h>
#include <stddef.h>

/**
 * sockaddr.h — Keel's platform-neutral socket address (PAL: address axis).
 *
 * The symmetric partner of `KlSocketHandle` (handle.h). Keel neutralized the
 * native *handle* long ago; `KlSockAddr` finishes the same job for the *address*,
 * so no layer above the socket provider ever touches a platform `struct sockaddr`
 * — whose layout is compile-time and differs per stack (BSD/lwIP carry `sin_len`,
 * Linux does not). Providers are the ONLY code that marshals `KlSockAddr` <->
 * platform `sockaddr` (see src/sockaddr_native.h); everything else — the resolver,
 * proxy_protocol, udp, the protocol clients, the completion backends' delivered
 * results — speaks `KlSockAddr`. That makes the socket/address ABI runtime-
 * injectable: a stock libkeel + a foreign socket provider (lwIP, …) is enough,
 * with no library recompile. See docs/keel_sockaddr_design.md.
 *
 * Layout is fixed and identical on every platform. It is ~120 bytes — smaller
 * than the `struct sockaddr_storage` (128 B) it replaces in KlResolveResult.
 */

/** Keel-owned address families — providers map to/from platform AF_* values, so
 *  core never depends on system AF_* numbering. */
typedef enum {
    KL_AF_UNSPEC = 0,
    KL_AF_INET,
    KL_AF_INET6,
    KL_AF_UNIX,
} KlAddrFamily;

/** Max AF_UNIX path (matches the POSIX `sun_path` capacity). */
#define KL_UNIX_PATH_MAX 108

/** Buffer size that always fits kl_sockaddr_format() output (incl. NUL). */
#define KL_SOCKADDR_STRLEN 128

/**
 * @brief Canonical socket address. Fixed Keel-defined layout on all platforms.
 *
 * `port` is in HOST byte order (core compares/logs without ntohs); `u.ip` is in
 * NETWORK byte order — as it appears on the wire and out of inet_pton / DNS A/AAAA
 * records / PROXY headers — so the construct-from-wire helpers need no swapping.
 */
typedef struct {
    uint16_t family;      /**< KlAddrFamily */
    uint16_t port;        /**< host byte order; 0 for AF_UNIX */
    uint32_t scope_id;    /**< IPv6 sin6_scope_id (link-local); 0 otherwise */
    uint8_t  addr_len;    /**< 4 (v4) | 16 (v6) | strlen(path) (unix) */
    union {
        uint8_t ip[16];              /**< network byte order; ip[0..3] for v4 */
        char    path[KL_UNIX_PATH_MAX]; /**< AF_UNIX, NUL-terminated */
    } u;
} KlSockAddr;

/* ── construct-from-wire (raw bytes already in network order) ──────────────── */

/** Build an IPv4 address from 4 network-order bytes + host-order port. Returns 0. */
int kl_sockaddr_from_ipv4(KlSockAddr *out, const uint8_t ip4[4], uint16_t port);
/** Build an IPv6 address from 16 network-order bytes + host-order port + scope. */
int kl_sockaddr_from_ipv6(KlSockAddr *out, const uint8_t ip6[16], uint16_t port,
                          uint32_t scope_id);
/** Build an AF_UNIX address from a filesystem path. Returns -1 if too long. */
int kl_sockaddr_from_unix(KlSockAddr *out, const char *path);

/**
 * Parse a NUMERIC address literal (no DNS) into @p out with host-order @p port:
 * dotted-quad IPv4 ("127.0.0.1", "0.0.0.0") or IPv6 ("::1", "::", "2001:db8::1",
 * with RFC 4291 "::" compression and an optional embedded-IPv4 tail). Returns 0
 * on success, -1 if @p host is not a valid numeric literal. Pure — no getaddrinfo,
 * no platform headers (works identically on every platform, incl. lwIP).
 */
int kl_sockaddr_parse(KlSockAddr *out, const char *host, uint16_t port);

/* ── accessors (core reads these rather than struct fields directly) ───────── */

/** Family of @p a as a KlAddrFamily. */
KlAddrFamily kl_sockaddr_family(const KlSockAddr *a);
/** Port in host byte order (0 for AF_UNIX / unset). */
uint16_t     kl_sockaddr_port(const KlSockAddr *a);
/** Set the port (host byte order). */
void         kl_sockaddr_set_port(KlSockAddr *a, uint16_t port);

/* ── comparison ───────────────────────────────────────────────────────────── */

/** Full equality: family + address + port (+ scope for v6). */
int kl_sockaddr_equal(const KlSockAddr *a, const KlSockAddr *b);
/** Host-only equality: family + address (+ scope), ignoring port. */
int kl_sockaddr_equal_addr(const KlSockAddr *a, const KlSockAddr *b);
/** 1 if @p a is a loopback address (127.0.0.0/8 or ::1), else 0. */
int kl_sockaddr_is_loopback(const KlSockAddr *a);

/* ── presentation ─────────────────────────────────────────────────────────── */

/**
 * Format @p a into @p buf as "host:port" (IPv4), "[host]:port" (IPv6, RFC 5952
 * zero-compressed), or the raw path (AF_UNIX). Writes at most @p n bytes incl.
 * the NUL. Returns the length written (excl. NUL), or -1 on truncation / bad
 * family. Use KL_SOCKADDR_STRLEN for a never-truncating buffer.
 */
int kl_sockaddr_format(const KlSockAddr *a, char *buf, size_t n);

/**
 * Format only the host part of @p a (no port, no brackets): "192.168.0.1",
 * "2001:db8::1" (RFC 5952), or the AF_UNIX path. Writes at most @p n bytes incl.
 * NUL. Returns the length written (excl. NUL), or -1 on truncation / bad family.
 */
int kl_sockaddr_format_ip(const KlSockAddr *a, char *buf, size_t n);

#endif /* KEEL_SOCKADDR_H */
