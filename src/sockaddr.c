/*
 * sockaddr.c — pure operations on KlSockAddr.
 *
 * NO system socket headers: this TU is platform-neutral by construction (it
 * compiles unchanged on a freestanding / lwIP target). All platform sockaddr
 * marshalling lives in src/sockaddr_native.h, included only by socket providers.
 */
#include <keel/sockaddr.h>

#include <string.h>
#include <stdio.h>

int kl_sockaddr_from_ipv4(KlSockAddr *out, const uint8_t ip4[4], uint16_t port) {
    if (!out || !ip4) return -1;
    memset(out, 0, sizeof *out);
    out->family = KL_AF_INET;
    out->port = port;
    out->addr_len = 4;
    memcpy(out->u.ip, ip4, 4);
    return 0;
}

int kl_sockaddr_from_ipv6(KlSockAddr *out, const uint8_t ip6[16], uint16_t port,
                          uint32_t scope_id) {
    if (!out || !ip6) return -1;
    memset(out, 0, sizeof *out);
    out->family = KL_AF_INET6;
    out->port = port;
    out->scope_id = scope_id;
    out->addr_len = 16;
    memcpy(out->u.ip, ip6, 16);
    return 0;
}

int kl_sockaddr_from_unix(KlSockAddr *out, const char *path) {
    if (!out || !path) return -1;
    size_t len = strlen(path);
    if (len == 0 || len >= KL_UNIX_PATH_MAX) return -1;
    memset(out, 0, sizeof *out);
    out->family = KL_AF_UNIX;
    out->addr_len = (uint8_t)len;
    memcpy(out->u.path, path, len + 1);   /* include NUL */
    return 0;
}

/* ── numeric literal parsing (pure — no inet_pton / getaddrinfo) ──────────── */

/* Parse dotted-quad IPv4 into out[4]. Returns 0 on success, -1 otherwise. */
static int parse_ipv4(const char *s, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return -1;
            s++;
        }
        out[i] = (uint8_t)v;
        if (i < 3) { if (*s != '.') return -1; s++; }
    }
    return (*s == '\0') ? 0 : -1;
}

/* Parse an IPv6 literal (RFC 4291: hextets, one "::" run, optional embedded IPv4
 * tail) into out[16]. Returns 0 on success, -1 otherwise. */
static int parse_ipv6(const char *s, uint8_t out[16]) {
    uint8_t buf[16] = {0};
    int gap = -1;      /* byte index where "::" was seen, -1 = none */
    int n = 0;         /* bytes filled so far */

    if (s[0] == ':') { if (s[1] != ':') return -1; s += 2; gap = 0; if (*s == '\0') { memset(out, 0, 16); return 0; } }

    for (;;) {
        /* embedded IPv4 tail (contains a '.') */
        const char *dot = s;
        while (*dot && *dot != ':' && *dot != '.') dot++;
        if (*dot == '.') {
            if (n > 12) return -1;
            if (parse_ipv4(s, buf + n) != 0) return -1;
            n += 4;
            s = "";  /* consumed to end */
            break;
        }
        /* a hextet: 1-4 hex digits */
        int v = 0, digits = 0;
        while (1) {
            char c = *s; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | d;
            if (++digits > 4) return -1;
            s++;
        }
        if (digits == 0) return -1;
        if (n > 14) return -1;
        buf[n++] = (uint8_t)(v >> 8);
        buf[n++] = (uint8_t)(v & 0xff);
        if (*s == '\0') break;
        if (*s != ':') return -1;
        s++;
        if (*s == ':') {                 /* start of "::" run */
            if (gap >= 0) return -1;      /* only one "::" allowed */
            gap = n;
            s++;
            if (*s == '\0') break;        /* trailing "::" */
        }
    }
    if (*s != '\0') return -1;

    if (gap >= 0) {
        if (n >= 16) return -1;          /* "::" must stand for >=1 zero group */
        int tail = n - gap;
        memmove(buf + (16 - tail), buf + gap, (size_t)tail);
        memset(buf + gap, 0, (size_t)(16 - tail - gap));
    } else if (n != 16) {
        return -1;
    }
    memcpy(out, buf, 16);
    return 0;
}

int kl_sockaddr_parse(KlSockAddr *out, const char *host, uint16_t port) {
    if (!out || !host) return -1;
    uint8_t v4[4];
    if (parse_ipv4(host, v4) == 0)
        return kl_sockaddr_from_ipv4(out, v4, port);
    uint8_t v6[16];
    if (parse_ipv6(host, v6) == 0)
        return kl_sockaddr_from_ipv6(out, v6, port, 0);
    return -1;
}

KlAddrFamily kl_sockaddr_family(const KlSockAddr *a) {
    return a ? (KlAddrFamily)a->family : KL_AF_UNSPEC;
}

uint16_t kl_sockaddr_port(const KlSockAddr *a) {
    return a ? a->port : 0;
}

void kl_sockaddr_set_port(KlSockAddr *a, uint16_t port) {
    if (a) a->port = port;
}

int kl_sockaddr_equal_addr(const KlSockAddr *a, const KlSockAddr *b) {
    if (!a || !b) return 0;
    if (a->family != b->family) return 0;
    switch (a->family) {
    case KL_AF_INET:
        return memcmp(a->u.ip, b->u.ip, 4) == 0;
    case KL_AF_INET6:
        return a->scope_id == b->scope_id && memcmp(a->u.ip, b->u.ip, 16) == 0;
    case KL_AF_UNIX:
        return a->addr_len == b->addr_len &&
               memcmp(a->u.path, b->u.path, a->addr_len) == 0;
    default:
        return 0;
    }
}

int kl_sockaddr_equal(const KlSockAddr *a, const KlSockAddr *b) {
    if (!kl_sockaddr_equal_addr(a, b)) return 0;
    return a->port == b->port;
}

int kl_sockaddr_is_loopback(const KlSockAddr *a) {
    if (!a) return 0;
    if (a->family == KL_AF_INET)
        return a->u.ip[0] == 127;                     /* 127.0.0.0/8 */
    if (a->family == KL_AF_INET6) {
        static const uint8_t v6_loop[16] = { [15] = 1 };  /* ::1 */
        return memcmp(a->u.ip, v6_loop, 16) == 0;
    }
    return 0;
}

/* ── IPv6 presentation (RFC 5952): longest zero-run -> "::", lowercase hex ─── */
static int fmt_ipv6(const uint8_t ip[16], char *buf, size_t n) {
    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (uint16_t)((ip[2 * i] << 8) | ip[2 * i + 1]);

    /* longest run of consecutive zero groups (>= 2 to be compressed) */
    int best = -1, best_len = 0, cur = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (cur < 0) { cur = i; cur_len = 1; } else { cur_len++; }
            if (cur_len > best_len) { best_len = cur_len; best = cur; }
        } else {
            cur = -1; cur_len = 0;
        }
    }
    if (best_len < 2) best = -1;

    /* Colon accounting per RFC 5952 (musl inet_ntop6 shape): a compressed run
     * emits a single ':' at its start; the separator before the group that
     * follows supplies the second colon — except a trailing run, which needs an
     * explicit closing ':' after the loop. */
    size_t off = 0;
    for (int i = 0; i < 8; i++) {
        if (best != -1 && i >= best && i < best + best_len) {
            if (i == best) {
                if (off >= n) return -1;
                buf[off++] = ':';
            }
            continue;
        }
        if (i != 0) {
            if (off >= n) return -1;
            buf[off++] = ':';
        }
        int r = snprintf(buf + off, n - off, "%x", g[i]);
        if (r < 0 || (size_t)r >= n - off) return -1;
        off += (size_t)r;
    }
    if (best != -1 && best + best_len == 8) {
        if (off >= n) return -1;
        buf[off++] = ':';
    }
    if (off >= n) return -1;
    buf[off] = '\0';
    return (int)off;
}

int kl_sockaddr_format_ip(const KlSockAddr *a, char *buf, size_t n) {
    if (!a || !buf || n == 0) return -1;

    if (a->family == KL_AF_INET) {
        int r = snprintf(buf, n, "%u.%u.%u.%u",
                         a->u.ip[0], a->u.ip[1], a->u.ip[2], a->u.ip[3]);
        return (r < 0 || (size_t)r >= n) ? -1 : r;
    }
    if (a->family == KL_AF_INET6)
        return fmt_ipv6(a->u.ip, buf, n);
    if (a->family == KL_AF_UNIX) {
        int r = snprintf(buf, n, "%.*s", (int)a->addr_len, a->u.path);
        return (r < 0 || (size_t)r >= n) ? -1 : r;
    }
    return -1;
}

int kl_sockaddr_format(const KlSockAddr *a, char *buf, size_t n) {
    if (!a || !buf || n == 0) return -1;

    if (a->family == KL_AF_INET) {
        char ip[16];
        if (kl_sockaddr_format_ip(a, ip, sizeof ip) < 0) return -1;
        int r = snprintf(buf, n, "%s:%u", ip, (unsigned)a->port);
        return (r < 0 || (size_t)r >= n) ? -1 : r;
    }
    if (a->family == KL_AF_INET6) {
        char ip[46];
        if (kl_sockaddr_format_ip(a, ip, sizeof ip) < 0) return -1;
        int r = snprintf(buf, n, "[%s]:%u", ip, (unsigned)a->port);
        return (r < 0 || (size_t)r >= n) ? -1 : r;
    }
    if (a->family == KL_AF_UNIX)
        return kl_sockaddr_format_ip(a, buf, n);
    return -1;
}
