/*
 * Negative + positive coverage for the mbedTLS peer-certificate identity
 * canonicalization (x509_extract_cn / x509_extract_san in tls_mbedtls.c).
 *
 * Embedding a NUL / control byte in a CommonName, or a comma in a DNS SAN, is not
 * expressible through a real certificate-writing API, so these malformed identities
 * cannot be produced end-to-end. This test therefore uses a TEST-ONLY seam: it
 * includes the adapter TU directly to reach its static canonicalization functions
 * (no new public API, no production callback) and drives them with hand-built
 * mbedtls_x509_name / mbedtls_x509_sequence structs, exactly the shapes the mbedTLS
 * X.509 parser produces for an attacker-supplied certificate. The end-to-end handshake
 * + peer_cert path is covered separately by tests/tls_e2e.c.
 */
#include "utest.h"
#include <string.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include <mbedtls/asn1.h>

#include "tls_mbedtls.c"   /* test-only seam: reach the static x509_extract_* helpers */

/* Build a single RelativeDistinguishedName node carrying a CommonName value. */
static mbedtls_x509_name mk_cn(const unsigned char *val, size_t vlen, mbedtls_x509_name *next)
{
    mbedtls_x509_name n;
    memset(&n, 0, sizeof(n));
    n.oid.tag = MBEDTLS_ASN1_OID;
    n.oid.p   = (unsigned char *)MBEDTLS_OID_AT_CN;
    n.oid.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_AT_CN);
    n.val.tag = MBEDTLS_ASN1_UTF8_STRING;
    n.val.p   = (unsigned char *)val;
    n.val.len = vlen;
    n.next    = next;
    return n;
}

/* Build a subjectAltName sequence node (DNS or IP), as the parser leaves the tag
 * (the extractor masks off the context class with MBEDTLS_ASN1_TAG_VALUE_MASK). */
static mbedtls_x509_sequence mk_san(int san_tag, const unsigned char *val, size_t vlen,
                                    mbedtls_x509_sequence *next)
{
    mbedtls_x509_sequence s;
    memset(&s, 0, sizeof(s));
    s.buf.tag = san_tag;
    s.buf.p   = (unsigned char *)val;
    s.buf.len = vlen;
    s.next    = next;
    return s;
}

/* ── CommonName: fail closed on spoofable / ambiguous identities ─────────────── */

/* The headline vector: an embedded NUL must NOT collapse to a strcmp()-matchable prefix. */
UTEST(cn_canon, embedded_nul_is_rejected)
{
    static const unsigned char cn[] = { 'a','d','m','i','n', 0, 'e','v','i','l' };
    mbedtls_x509_name n = mk_cn(cn, sizeof(cn), NULL);
    char out[256] = "sentinel";
    x509_extract_cn(&n, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);   /* omitted, not "admin" */
}

UTEST(cn_canon, control_byte_is_rejected)
{
    static const unsigned char cn[] = { 'a','d', 0x01, 'm','i','n' };
    mbedtls_x509_name n = mk_cn(cn, sizeof(cn), NULL);
    char out[256] = "sentinel";
    x509_extract_cn(&n, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);
}

UTEST(cn_canon, multiple_cn_is_rejected)
{
    static const unsigned char a[] = { 'f','i','r','s','t' };
    static const unsigned char b[] = { 's','e','c','o','n','d' };
    mbedtls_x509_name second = mk_cn(b, sizeof(b), NULL);
    mbedtls_x509_name first  = mk_cn(a, sizeof(a), &second);
    char out[256] = "sentinel";
    x509_extract_cn(&first, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);   /* which CN is authoritative is undefined: omit */
}

UTEST(cn_canon, would_truncate_is_rejected)
{
    static const unsigned char cn[] = { 'a','b','c','d','e','f','g','h','i','j' };  /* 10 bytes */
    mbedtls_x509_name n = mk_cn(cn, sizeof(cn), NULL);
    char out[8] = "sentinel";   /* len (10) >= outlen (8): a cut value is ambiguous */
    x509_extract_cn(&n, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);
}

UTEST(cn_canon, valid_cn_is_extracted_unchanged)
{
    static const unsigned char cn[] = { 'a','d','m','i','n' };
    mbedtls_x509_name n = mk_cn(cn, sizeof(cn), NULL);
    char out[256] = "";
    x509_extract_cn(&n, out, sizeof(out));
    ASSERT_STREQ("admin", out);
}

/* A CN longer than the small-but-fitting buffer still extracts (only >= outlen is cut). */
UTEST(cn_canon, valid_cn_exactly_fits)
{
    static const unsigned char cn[] = { 'r','o','o','t' };   /* 4 bytes */
    mbedtls_x509_name n = mk_cn(cn, sizeof(cn), NULL);
    char out[5] = "";   /* 4 < 5: fits with room for the NUL */
    x509_extract_cn(&n, out, sizeof(out));
    ASSERT_STREQ("root", out);
}

/* ── subjectAltName: skip invalid entries without partial output ─────────────── */

UTEST(san_canon, comma_dns_is_skipped)
{
    static const unsigned char dns[] = { 'a', ',', 'b' };   /* comma would break the joined list */
    mbedtls_x509_sequence s = mk_san(MBEDTLS_X509_SAN_DNS_NAME, dns, sizeof(dns), NULL);
    char out[256] = "sentinel";
    x509_extract_san(&s, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);   /* skipped, no "DNS:a" partial */
}

UTEST(san_canon, control_dns_is_skipped)
{
    static const unsigned char dns[] = { 'a', 0x01, 'b' };
    mbedtls_x509_sequence s = mk_san(MBEDTLS_X509_SAN_DNS_NAME, dns, sizeof(dns), NULL);
    char out[256] = "sentinel";
    x509_extract_san(&s, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);
}

UTEST(san_canon, overlong_dns_is_skipped)
{
    static unsigned char dns[254];
    memset(dns, 'a', sizeof(dns));   /* 254 valid LDH bytes, one over the 253 max */
    mbedtls_x509_sequence s = mk_san(MBEDTLS_X509_SAN_DNS_NAME, dns, sizeof(dns), NULL);
    char out[512] = "sentinel";
    x509_extract_san(&s, out, sizeof(out));
    ASSERT_EQ('\0', out[0]);   /* over-long: omitted, not truncated */
}

/* An invalid entry between two valid ones is dropped; the valid ones join cleanly. */
UTEST(san_canon, invalid_between_valid_is_skipped_cleanly)
{
    static const unsigned char good1[] = { 'a','.','c','o','m' };
    static const unsigned char bad[]   = { 'b', ',', 'd' };
    static const unsigned char good2[] = { 'e','.','n','e','t' };
    mbedtls_x509_sequence n2 = mk_san(MBEDTLS_X509_SAN_DNS_NAME, good2, sizeof(good2), NULL);
    mbedtls_x509_sequence n1 = mk_san(MBEDTLS_X509_SAN_DNS_NAME, bad,   sizeof(bad),   &n2);
    mbedtls_x509_sequence n0 = mk_san(MBEDTLS_X509_SAN_DNS_NAME, good1, sizeof(good1), &n1);
    char out[256] = "";
    x509_extract_san(&n0, out, sizeof(out));
    ASSERT_STREQ("DNS:a.com,DNS:e.net", out);
}

UTEST(san_canon, valid_dns_and_ip_extracted)
{
    static const unsigned char dns[] = { 'h','o','s','t','.','e','x','a','m','p','l','e','.','c','o','m' };
    static const unsigned char ip4[] = { 127, 0, 0, 1 };
    mbedtls_x509_sequence ipn = mk_san(MBEDTLS_X509_SAN_IP_ADDRESS, ip4, sizeof(ip4), NULL);
    mbedtls_x509_sequence dnsn = mk_san(MBEDTLS_X509_SAN_DNS_NAME, dns, sizeof(dns), &ipn);
    char out[256] = "";
    x509_extract_san(&dnsn, out, sizeof(out));
    ASSERT_STREQ("DNS:host.example.com,IP:127.0.0.1", out);
}

/* A full 253-byte DNS name (the max) is surfaced whole, not skipped (buffer-size fix). */
UTEST(san_canon, max_length_dns_is_extracted)
{
    static unsigned char dns[253];
    memset(dns, 'a', sizeof(dns));
    mbedtls_x509_sequence s = mk_san(MBEDTLS_X509_SAN_DNS_NAME, dns, sizeof(dns), NULL);
    char out[512] = "";
    x509_extract_san(&s, out, sizeof(out));
    ASSERT_EQ((size_t)(4 + 253), strlen(out));   /* "DNS:" + 253 */
    ASSERT_EQ(0, strncmp(out, "DNS:aaa", 7));
}

UTEST_MAIN()   /* no trailing ';': -Wpedantic (-Wextra-semi) is active in this integration */
