/*
 * test_kl_cstr.c: the bounded, locale-free C-string helpers used instead of
 * snprintf/strtol/str* in the freestanding client path. Each is checked against
 * its libc equivalent (byte-identical output / sign / acceptance) + edge cases.
 */
#include "utest.h"
#include "../src/kl_cstr.h"

#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>

static int sgn(int v) { return (v > 0) - (v < 0); }

/* ── kl_ascii_strcasecmp / strncasecmp: sign matches libc for ASCII ─────────── */
UTEST(kl_cstr, ascii_casecmp) {
    const char *pairs[][2] = {
        {"Host", "host"}, {"HOST", "host"}, {"host", "host"},
        {"Content-Type", "content-type"}, {"a", "b"}, {"b", "a"},
        {"", ""}, {"", "x"}, {"x", ""}, {"abc", "abcd"}, {"abcd", "abc"},
        {"Transfer-Encoding", "Transfer-Encodin"}, {"AbC", "aBc"},
    };
    for (size_t i = 0; i < sizeof(pairs)/sizeof(pairs[0]); i++) {
        ASSERT_EQ(sgn(kl_ascii_strcasecmp(pairs[i][0], pairs[i][1])),
                  sgn(strcasecmp(pairs[i][0], pairs[i][1])));
    }
    /* Non-alphabetic bytes are NOT case-folded (digits/punct compare by value). */
    ASSERT_EQ(sgn(kl_ascii_strcasecmp("1.1", "1.1")), 0);
    /* n-bounded */
    ASSERT_EQ(kl_ascii_strncasecmp("HELLO", "hello world", 5), 0);
    ASSERT_NE(kl_ascii_strncasecmp("HELLO", "hellX", 5), 0);
    ASSERT_EQ(kl_ascii_strncasecmp("anything", "AnyTHING", 0), 0);  /* n=0 → equal */
    ASSERT_EQ(sgn(kl_ascii_strncasecmp("abc", "abd", 3)),
              sgn(strncasecmp("abc", "abd", 3)));
}

/* ── kl_parse_u16_decimal: port parsing, bounded, overflow-safe ─────────────── */
UTEST(kl_cstr, parse_u16) {
    uint16_t out = 0xdead;
    ASSERT_EQ(kl_parse_u16_decimal("0", 1, &out), 0);      ASSERT_EQ(out, 0);
    ASSERT_EQ(kl_parse_u16_decimal("80", 2, &out), 0);     ASSERT_EQ(out, 80);
    ASSERT_EQ(kl_parse_u16_decimal("443", 3, &out), 0);    ASSERT_EQ(out, 443);
    ASSERT_EQ(kl_parse_u16_decimal("65535", 5, &out), 0);  ASSERT_EQ(out, 65535);
    ASSERT_EQ(kl_parse_u16_decimal("08080", 5, &out), 0);  ASSERT_EQ(out, 8080); /* leading zeros ok */
    /* Only the first `len` chars are parsed (URL passes the digit span). */
    ASSERT_EQ(kl_parse_u16_decimal("8080/path", 4, &out), 0); ASSERT_EQ(out, 8080);
    /* Rejections */
    ASSERT_NE(kl_parse_u16_decimal("65536", 5, &out), 0);   /* overflow 16-bit */
    ASSERT_NE(kl_parse_u16_decimal("99999", 5, &out), 0);   /* overflow */
    ASSERT_NE(kl_parse_u16_decimal("", 0, &out), 0);        /* empty */
    ASSERT_NE(kl_parse_u16_decimal("8a", 2, &out), 0);      /* non-digit */
    ASSERT_NE(kl_parse_u16_decimal("-1", 2, &out), 0);      /* sign not a digit */
}

/* ── kl_u64_to_dec / kl_u64_to_hex: byte-identical to snprintf ──────────────── */
UTEST(kl_cstr, u64_format) {
    uint64_t vals[] = { 0, 1, 9, 10, 80, 255, 1000, 65535, 4294967295ULL,
                        18446744073709551615ULL /* UINT64_MAX */ };
    char got[32], want[32];
    for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
        /* kl_u64_to_* do NOT NUL-terminate: terminate before ASSERT_STREQ. */
        size_t n = kl_u64_to_dec(got, sizeof(got) - 1, vals[i]);
        got[n] = '\0';
        snprintf(want, sizeof(want), "%llu", (unsigned long long)vals[i]);
        ASSERT_EQ(n, strlen(want));
        ASSERT_STREQ(got, want);

        n = kl_u64_to_hex(got, sizeof(got) - 1, vals[i]);
        got[n] = '\0';
        snprintf(want, sizeof(want), "%llx", (unsigned long long)vals[i]);
        ASSERT_EQ(n, strlen(want));
        ASSERT_STREQ(got, want);
    }
    /* Bounded: cap too small → 0 written, buf untouched (not NUL-terminated). */
    char tiny[3];
    ASSERT_EQ(kl_u64_to_dec(tiny, sizeof(tiny), 1000), (size_t)0);  /* "1000" is 4 digits > cap 3 */
}

/* ── kl_buf_append family: bounded builders tracking an offset ──────────────── */
UTEST(kl_cstr, buf_append) {
    char buf[64];
    size_t off = 0;
    ASSERT_EQ(kl_buf_append(buf, sizeof(buf), &off, "GET "), 0);
    ASSERT_EQ(kl_buf_append(buf, sizeof(buf), &off, "/index"), 0);
    ASSERT_EQ(kl_buf_append_n(buf, sizeof(buf), &off, "?a=1&extra", 4), 0); /* "?a=1" */
    ASSERT_EQ(kl_buf_append(buf, sizeof(buf), &off, " HTTP/1.1\r\nHost: h:"), 0);
    ASSERT_EQ(kl_buf_append_u64(buf, sizeof(buf), &off, 8080), 0);
    ASSERT_STREQ(buf, "GET /index?a=1 HTTP/1.1\r\nHost: h:8080");
    ASSERT_EQ(off, strlen(buf));

    /* Overflow: appending past cap fails, does not write OOB. */
    char small[8];
    size_t o2 = 0;
    ASSERT_EQ(kl_buf_append(small, sizeof(small), &o2, "abc"), 0);
    ASSERT_NE(kl_buf_append(small, sizeof(small), &o2, "defghijkl"), 0); /* would overflow */

    /* hex append */
    char hbuf[16]; size_t ho = 0;
    ASSERT_EQ(kl_buf_append_hex(hbuf, sizeof(hbuf), &ho, 0x1a2b), 0);
    ASSERT_STREQ(hbuf, "1a2b");
}

/* ── kl_streq / kl_str_startswith: exact, case-sensitive ────────────────────── */
UTEST(kl_cstr, streq_startswith) {
    ASSERT_TRUE(kl_streq("example.com", "example.com"));
    ASSERT_FALSE(kl_streq("example.com", "example.org"));
    ASSERT_FALSE(kl_streq("Example.com", "example.com"));   /* case-sensitive */
    ASSERT_FALSE(kl_streq("abc", "abcd"));

    ASSERT_TRUE(kl_str_startswith("http://x", "http://"));
    ASSERT_FALSE(kl_str_startswith("https://x", "http://"));
    ASSERT_FALSE(kl_str_startswith("ht", "http://"));       /* prefix longer than s */
    ASSERT_TRUE(kl_str_startswith("anything", ""));         /* empty prefix */
}

/* ── kl_strstr / kl_strchr: same result as libc ────────────────────────────── */
UTEST(kl_cstr, strstr_strchr) {
    const char *h = "HTTP/1.1 200 OK\r\n\r\nbody";
    ASSERT_EQ(kl_strstr(h, "\r\n\r\n"), strstr(h, "\r\n\r\n"));
    ASSERT_EQ(kl_strstr(h, "200"), strstr(h, "200"));
    ASSERT_EQ(kl_strstr(h, "nope"), strstr(h, "nope"));     /* both NULL */
    ASSERT_EQ(kl_strstr(h, ""), strstr(h, ""));             /* empty needle → hay */

    ASSERT_EQ(kl_strchr(h, '/'), strchr(h, '/'));
    ASSERT_EQ(kl_strchr(h, ':'), strchr(h, ':'));           /* not present → both NULL */
    ASSERT_EQ(kl_strchr(h, 'z'), strchr(h, 'z'));
    ASSERT_EQ(kl_strchr(h, '\0'), strchr(h, '\0'));         /* NUL → end pointer */
}

UTEST_MAIN();
