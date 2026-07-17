#ifndef KEEL_PROXY_PROTOCOL_H
#define KEEL_PROXY_PROTOCOL_H

/*
 * PROXY protocol (v1 + v2) header parser and CIDR trust matching.
 *
 * Used when KEEL sits behind an L4 load balancer (HAProxy, AWS NLB, nginx
 * stream) that prepends the real client address to each connection. Only
 * honored from sources in a configured CIDR allowlist (see KlConfig.
 * proxy_trusted_cidrs) — a header from an untrusted peer could spoof any IP.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef enum {
    KL_PROXY_NONE = 0,   /**< Leading bytes are not a PROXY header — direct conn */
    KL_PROXY_NEED_MORE,  /**< Looks like a PROXY header but need more bytes */
    KL_PROXY_OK,         /**< Parsed; *consumed set; peer filled unless LOCAL/UNKNOWN */
    KL_PROXY_INVALID     /**< Malformed / oversized → close the connection */
} KlProxyResult;

/**
 * @brief Parse a PROXY protocol v1 or v2 header from the front of a buffer.
 *
 * @param buf       Bytes read from the connection (not consumed).
 * @param len       Number of bytes in @p buf.
 * @param consumed  On KL_PROXY_OK, receives the header length to consume.
 * @param peer      On KL_PROXY_OK with a real source address, receives it
 *                  (AF_INET / AF_INET6). Untouched for LOCAL/UNKNOWN.
 * @param peer_len  Set to the address length, or 0 for LOCAL/UNKNOWN (caller
 *                  keeps the real socket address).
 * @return A KlProxyResult.
 */
KlProxyResult kl_proxy_parse(const uint8_t *buf, size_t len, size_t *consumed,
                             struct sockaddr_storage *peer, socklen_t *peer_len);

/** @brief Maximum bytes a PROXY header parser may need to buffer (v2 cap). */
#define KL_PROXY_HEADER_MAX 536

/* ── CIDR trust list ─────────────────────────────────────────────────── */

typedef struct {
    int     family;      /**< AF_INET or AF_INET6 */
    uint8_t addr[16];    /**< network bytes (4 used for v4) */
    int     bits;        /**< prefix length */
} KlCidr;

/**
 * @brief Parse a comma-separated CIDR list ("10.0.0.0/8,::1/128").
 *
 * @param s    The list string.
 * @param out  Output array.
 * @param cap  Capacity of @p out.
 * @return Number of CIDRs parsed, or -1 on a malformed entry / overflow.
 */
int kl_cidr_parse_list(const char *s, KlCidr *out, int cap);

/**
 * @brief Test whether a socket address falls within any CIDR in the list.
 * @return 1 if matched, 0 otherwise.
 */
int kl_cidr_match(const KlCidr *list, int count, const struct sockaddr *sa);

#endif /* KEEL_PROXY_PROTOCOL_H */
