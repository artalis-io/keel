/*
 * kl_cstr.c: locale-free, bounded C-string primitives (see kl_cstr.h).
 *
 * Intentionally uses ONLY strlen from the C runtime (plus its own byte loops);
 * no snprintf / strtol / strcasecmp / strstr / strchr. This is what lets the
 * freestanding archive drop the formatted-I/O + locale surface.
 */
#include "kl_cstr.h"

#include <string.h>   /* strlen only */

/* ── ASCII case-fold (locale-free) ────────────────────────────────── */

static unsigned char ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

int kl_ascii_strcasecmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = ascii_lower((unsigned char)*a);
        unsigned char cb = ascii_lower((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;   /* both hit NUL simultaneously */
        a++; b++;
    }
}

int kl_ascii_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = ascii_lower((unsigned char)a[i]);
        unsigned char cb = ascii_lower((unsigned char)b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;   /* strncasecmp stops at a shared NUL */
    }
    return 0;
}

/* ── decimal port parse (replaces strtol) ─────────────────────────── */

int kl_parse_u16_decimal(const char *s, size_t len, uint16_t *out) {
    if (!s || !out || len == 0) return -1;
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10u + (uint32_t)(c - '0');
        if (v > 65535u) return -1;   /* overflow / out of uint16 range */
    }
    *out = (uint16_t)v;
    return 0;
}

/* ── integer -> ASCII (replaces snprintf numeric conversions) ─────── */

size_t kl_u64_to_dec(char *buf, size_t cap, uint64_t v) {
    char tmp[20];   /* 2^64-1 == 20 digits */
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    if (n > cap) return 0;          /* would not fit */
    for (size_t i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];    /* reverse into place */
    return n;
}

size_t kl_u64_to_hex(char *buf, size_t cap, uint64_t v) {
    static const char hexd[] = "0123456789abcdef";
    char tmp[16];   /* 2^64-1 == 16 hex digits */
    size_t n = 0;
    do {
        tmp[n++] = hexd[v & 0xfu];
        v >>= 4;
    } while (v != 0);
    if (n > cap) return 0;
    for (size_t i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

/* ── bounded append builders (replace snprintf string concat) ─────── */

int kl_buf_append_n(char *buf, size_t cap, size_t *off, const char *s,
                    size_t len) {
    size_t o = *off;
    /* need len bytes + 1 for the trailing NUL, overflow-guarded */
    if (o > cap || len > cap - o) return -1;   /* no room for the bytes */
    if (len == cap - o) return -1;             /* no room for the NUL */
    memcpy(buf + o, s, len);
    o += len;
    buf[o] = '\0';
    *off = o;
    return 0;
}

int kl_buf_append(char *buf, size_t cap, size_t *off, const char *s) {
    return kl_buf_append_n(buf, cap, off, s, strlen(s));
}

int kl_buf_append_u64(char *buf, size_t cap, size_t *off, uint64_t v) {
    char tmp[20];
    size_t n = kl_u64_to_dec(tmp, sizeof(tmp), v);
    /* n is always <= 20 <= sizeof(tmp); 0 only if cap==0 which can't happen */
    return kl_buf_append_n(buf, cap, off, tmp, n);
}

int kl_buf_append_hex(char *buf, size_t cap, size_t *off, uint64_t v) {
    char tmp[16];
    size_t n = kl_u64_to_hex(tmp, sizeof(tmp), v);
    return kl_buf_append_n(buf, cap, off, tmp, n);
}

/* ── bounded find (replace strstr / strchr) ───────────────────────── */

const char *kl_strchr(const char *s, int c) {
    unsigned char target = (unsigned char)c;
    for (;;) {
        unsigned char cur = (unsigned char)*s;
        if (cur == target) return s;   /* also matches NUL when c==0 */
        if (cur == 0) return NULL;
        s++;
    }
}

int kl_streq(const char *a, const char *b) {
    for (size_t i = 0;; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;   /* both terminated together */
    }
}

int kl_str_startswith(const char *s, const char *prefix) {
    for (size_t i = 0; prefix[i]; i++) {
        if (s[i] != prefix[i]) return 0;   /* also fails if s ended early */
    }
    return 1;
}

const char *kl_strstr(const char *hay, const char *needle) {
    if (needle[0] == '\0') return hay;      /* empty needle: matches start */
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (*n == '\0') return hay;         /* full needle matched */
    }
    return NULL;
}
