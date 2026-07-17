#include <keel/proxy_protocol.h>

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* ── PROXY protocol header parsing ───────────────────────────────────── */

static const uint8_t V2SIG[12] = {
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A
};
static const char V1SIG[6] = { 'P', 'R', 'O', 'X', 'Y', ' ' };

static KlProxyResult parse_v1(const uint8_t *buf, size_t len, size_t *consumed,
                              struct sockaddr_storage *peer, socklen_t *peer_len) {
    /* Locate CRLF within the 107-byte cap. */
    size_t max = len < 107 ? len : 107;
    size_t nl = 0;
    int found = 0;
    for (size_t i = 5; i + 1 < max; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') { nl = i; found = 1; break; }
    }
    if (!found)
        return (len >= 107) ? KL_PROXY_INVALID : KL_PROXY_NEED_MORE;

    char line[108];
    memcpy(line, buf, nl);
    line[nl] = '\0';
    *consumed = nl + 2;   /* include CRLF */

    /* Tokens: PROXY <proto> <src> <dst> <sport> <dport> */
    char *save = NULL;
    const char *tok = strtok_r(line, " ", &save);      /* "PROXY" */
    if (!tok) return KL_PROXY_INVALID;
    const char *proto = strtok_r(NULL, " ", &save);
    if (!proto) return KL_PROXY_INVALID;

    if (strcmp(proto, "UNKNOWN") == 0) {
        *peer_len = 0;   /* keep the real socket address */
        return KL_PROXY_OK;
    }
    int family;
    if (strcmp(proto, "TCP4") == 0)      family = AF_INET;
    else if (strcmp(proto, "TCP6") == 0) family = AF_INET6;
    else return KL_PROXY_INVALID;

    const char *src = strtok_r(NULL, " ", &save);
    const char *dst = strtok_r(NULL, " ", &save);
    const char *sport = strtok_r(NULL, " ", &save);
    const char *dport = strtok_r(NULL, " ", &save);
    if (!src || !dst || !sport || !dport)
        return KL_PROXY_INVALID;

    char *end;
    long p = strtol(sport, &end, 10);
    if (end == sport || *end != '\0' || p < 0 || p > 65535)
        return KL_PROXY_INVALID;

    memset(peer, 0, sizeof(*peer));
    if (family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)peer;
        if (inet_pton(AF_INET, src, &s4->sin_addr) != 1)
            return KL_PROXY_INVALID;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)p);
        *peer_len = sizeof(*s4);
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)peer;
        if (inet_pton(AF_INET6, src, &s6->sin6_addr) != 1)
            return KL_PROXY_INVALID;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)p);
        *peer_len = sizeof(*s6);
    }
    return KL_PROXY_OK;
}

static KlProxyResult parse_v2(const uint8_t *buf, size_t len, size_t *consumed,
                              struct sockaddr_storage *peer, socklen_t *peer_len) {
    uint8_t ver_cmd = buf[12];
    if ((ver_cmd >> 4) != 0x2)
        return KL_PROXY_INVALID;         /* version must be 2 */
    uint8_t cmd = ver_cmd & 0x0F;
    uint8_t fam = (uint8_t)(buf[13] >> 4);
    uint16_t blocklen = (uint16_t)((buf[14] << 8) | buf[15]);
    size_t total = 16u + blocklen;
    if (total > KL_PROXY_HEADER_MAX)
        return KL_PROXY_INVALID;
    if (len < total)
        return KL_PROXY_NEED_MORE;

    *consumed = total;
    if (cmd == 0x0) {                    /* LOCAL — health check, no address */
        *peer_len = 0;
        return KL_PROXY_OK;
    }
    if (cmd != 0x1)                      /* only LOCAL or PROXY */
        return KL_PROXY_INVALID;

    const uint8_t *a = buf + 16;
    if (fam == 0x1) {                    /* AF_INET: src4 dst4 sport2 dport2 */
        if (blocklen < 12) return KL_PROXY_INVALID;
        struct sockaddr_in *s4 = (struct sockaddr_in *)peer;
        memset(s4, 0, sizeof(*s4));
        s4->sin_family = AF_INET;
        memcpy(&s4->sin_addr, a, 4);
        memcpy(&s4->sin_port, a + 8, 2); /* already network order */
        *peer_len = sizeof(*s4);
        return KL_PROXY_OK;
    }
    if (fam == 0x2) {                    /* AF_INET6: src16 dst16 sport2 dport2 */
        if (blocklen < 36) return KL_PROXY_INVALID;
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)peer;
        memset(s6, 0, sizeof(*s6));
        s6->sin6_family = AF_INET6;
        memcpy(&s6->sin6_addr, a, 16);
        memcpy(&s6->sin6_port, a + 32, 2);
        *peer_len = sizeof(*s6);
        return KL_PROXY_OK;
    }
    *peer_len = 0;   /* AF_UNSPEC / AF_UNIX — keep socket address */
    return KL_PROXY_OK;
}

KlProxyResult kl_proxy_parse(const uint8_t *buf, size_t len, size_t *consumed,
                             struct sockaddr_storage *peer, socklen_t *peer_len) {
    if (!buf || !consumed || !peer || !peer_len)
        return KL_PROXY_INVALID;
    if (len == 0)
        return KL_PROXY_NEED_MORE;

    size_t v2cmp = len < 12 ? len : 12;
    if (memcmp(buf, V2SIG, v2cmp) == 0) {
        if (len < 16) return KL_PROXY_NEED_MORE;
        return parse_v2(buf, len, consumed, peer, peer_len);
    }
    size_t v1cmp = len < 6 ? len : 6;
    if (memcmp(buf, V1SIG, v1cmp) == 0) {
        if (len < 6) return KL_PROXY_NEED_MORE;
        return parse_v1(buf, len, consumed, peer, peer_len);
    }
    return KL_PROXY_NONE;
}

/* ── CIDR trust list ─────────────────────────────────────────────────── */

int kl_cidr_parse_list(const char *s, KlCidr *out, int cap) {
    if (!s)
        return 0;
    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ' && *e != '\t') e++;
        size_t tl = (size_t)(e - p);

        char tok[128];
        if (tl == 0 || tl >= sizeof(tok)) return -1;
        memcpy(tok, p, tl);
        tok[tl] = '\0';
        p = e;

        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';

        uint8_t abuf[16] = {0};
        int family, maxbits;
        if (inet_pton(AF_INET, tok, abuf) == 1)       { family = AF_INET;  maxbits = 32; }
        else if (inet_pton(AF_INET6, tok, abuf) == 1) { family = AF_INET6; maxbits = 128; }
        else return -1;

        int bits = maxbits;
        if (slash) {
            char *end;
            long b = strtol(slash + 1, &end, 10);
            if (end == slash + 1 || *end != '\0' || b < 0 || b > maxbits)
                return -1;
            bits = (int)b;
        }

        if (n >= cap) return -1;
        out[n].family = family;
        out[n].bits = bits;
        memset(out[n].addr, 0, sizeof(out[n].addr));
        memcpy(out[n].addr, abuf, family == AF_INET ? 4 : 16);
        n++;
    }
    return n;
}

static int prefix_match(const uint8_t *a, const uint8_t *b, int bits, int alen) {
    int full = bits / 8, rem = bits % 8;
    if (full > alen) return 0;
    if (full > 0 && memcmp(a, b, (size_t)full) != 0) return 0;
    if (rem) {
        uint8_t mask = (uint8_t)(0xFF << (8 - rem));
        if ((a[full] & mask) != (b[full] & mask)) return 0;
    }
    return 1;
}

int kl_cidr_match(const KlCidr *list, int count, const struct sockaddr *sa) {
    if (!sa || !list)
        return 0;
    const uint8_t *a;
    int alen;
    if (sa->sa_family == AF_INET) {
        a = (const uint8_t *)&((const struct sockaddr_in *)sa)->sin_addr;
        alen = 4;
    } else if (sa->sa_family == AF_INET6) {
        a = (const uint8_t *)&((const struct sockaddr_in6 *)sa)->sin6_addr;
        alen = 16;
    } else {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (list[i].family != sa->sa_family) continue;
        if (prefix_match(a, list[i].addr, list[i].bits, alen)) return 1;
    }
    return 0;
}
