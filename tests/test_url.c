#include "utest.h"
#include <keel/url.h>
#include "url_internal.h"   /* kl_url_authority */
#include <string.h>

UTEST(url, null_args) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse(NULL, &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com", NULL), -1);
}

UTEST(url, simple_http) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com", &u), 0);
    ASSERT_EQ(u.is_https, 0);
    ASSERT_EQ(u.host_len, (size_t)11);
    ASSERT_EQ(memcmp(u.host, "example.com", 11), 0);
    ASSERT_EQ(u.port, 80);
    ASSERT_STREQ(u.path, "/");
    ASSERT_EQ(u.path_len, (size_t)1);
}

UTEST(url, simple_https) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("https://example.com", &u), 0);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(u.host_len, (size_t)11);
    ASSERT_EQ(u.port, 443);
}

UTEST(url, with_port) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://localhost:8080/api", &u), 0);
    ASSERT_EQ(u.port, 8080);
    ASSERT_EQ(u.host_len, (size_t)9);
    ASSERT_EQ(memcmp(u.host, "localhost", 9), 0);
    ASSERT_EQ(u.path_len, (size_t)4);
    ASSERT_EQ(memcmp(u.path, "/api", 4), 0);
}

UTEST(url, with_path_and_query) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("https://api.example.com/v1/users?page=1&limit=10", &u), 0);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(u.port, 443);
    ASSERT_EQ(u.host_len, (size_t)15);
    ASSERT_EQ(memcmp(u.host, "api.example.com", 15), 0);
    /* path includes query */
    ASSERT_TRUE(u.path_len > 0);
    ASSERT_EQ(memcmp(u.path, "/v1/users?page=1&limit=10", 25), 0);
}

UTEST(url, ipv6_no_port) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://[::1]/test", &u), 0);
    ASSERT_EQ(u.host_len, (size_t)3);
    ASSERT_EQ(memcmp(u.host, "::1", 3), 0);
    ASSERT_EQ(u.port, 80);
    ASSERT_EQ(memcmp(u.path, "/test", 5), 0);
}

UTEST(url, ipv6_with_port) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://[::1]:9090/path", &u), 0);
    ASSERT_EQ(u.host_len, (size_t)3);
    ASSERT_EQ(memcmp(u.host, "::1", 3), 0);
    ASSERT_EQ(u.port, 9090);
    ASSERT_EQ(memcmp(u.path, "/path", 5), 0);
}

UTEST(url, crlf_in_host_rejected) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://evil\r\nHost: injected/path", &u), -1);
    ASSERT_EQ(kl_url_parse("http://evil\nhost/path", &u), -1);
}

UTEST(url, crlf_in_path_rejected) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com/path\r\nInjected: header", &u), -1);
}

UTEST(url, unsupported_scheme) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("ftp://example.com", &u), -1);
    ASSERT_EQ(kl_url_parse("gopher://example.com", &u), -1);
}

UTEST(url, empty_host_rejected) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http:///path", &u), -1);
}

UTEST(url, unix_socket_basic) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http+unix://%2Frun%2Fapp.sock/v1/ping", &u), 0);
    ASSERT_TRUE(u.is_unix);
    ASSERT_FALSE(u.is_https);
    ASSERT_STREQ(u.unix_path, "/run/app.sock");
    ASSERT_EQ(u.host_len, (size_t)0);
    ASSERT_EQ(u.port, 0);
    ASSERT_EQ((int)u.path_len, 8);
    ASSERT_STRNEQ(u.path, "/v1/ping", 8);
}

UTEST(url, unix_socket_https) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("https+unix://%2Ftmp%2Fx.sock/", &u), 0);
    ASSERT_TRUE(u.is_unix);
    ASSERT_TRUE(u.is_https);
    ASSERT_STREQ(u.unix_path, "/tmp/x.sock");
    ASSERT_STRNEQ(u.path, "/", 1);
}

UTEST(url, unix_socket_no_path_defaults_root) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http+unix://%2Ftmp%2Fx.sock", &u), 0);
    ASSERT_TRUE(u.is_unix);
    ASSERT_STREQ(u.unix_path, "/tmp/x.sock");
    ASSERT_STRNEQ(u.path, "/", 1);
    ASSERT_EQ((int)u.path_len, 1);
}

UTEST(url, unix_socket_empty_path_rejected) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http+unix:///v1", &u), -1);
}

UTEST(url, unix_socket_bad_escape_rejected) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http+unix://%2G%2Fx/v1", &u), -1);   /* bad hex */
    ASSERT_EQ(kl_url_parse("http+unix://%2/v1", &u), -1);        /* truncated */
    ASSERT_EQ(kl_url_parse("http+unix://%00x/v1", &u), -1);      /* embedded NUL */
}

UTEST(url, ws_unix) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("ws+unix://%2Ftmp%2Fw.sock/chat", &u), 0);
    ASSERT_TRUE(u.is_unix);
    ASSERT_TRUE(u.is_ws);
    ASSERT_FALSE(u.is_https);
    ASSERT_STREQ(u.unix_path, "/tmp/w.sock");
    ASSERT_STRNEQ(u.path, "/chat", 5);
}

UTEST(url, wss_unix) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("wss+unix://%2Ftmp%2Fw.sock/", &u), 0);
    ASSERT_TRUE(u.is_unix);
    ASSERT_TRUE(u.is_ws);
    ASSERT_TRUE(u.is_https);
    ASSERT_STREQ(u.unix_path, "/tmp/w.sock");
}

UTEST(url, unix_socket_unencoded_path) {
    /* Slashes need not be encoded if the socket path has no leading slash
     * conflict; but the first literal '/' always starts the request path.
     * A relative-looking socket name works unescaped: */
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http+unix://app.sock/health", &u), 0);
    ASSERT_STREQ(u.unix_path, "app.sock");
    ASSERT_STRNEQ(u.path, "/health", 7);
}

UTEST(url, invalid_port) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com:0/path", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com:99999/path", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com:abc/path", &u), -1);
}

UTEST(url, ipv6_missing_bracket) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://[::1/path", &u), -1);
}

UTEST(url, https_port_override) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("https://example.com:8443/secure", &u), 0);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(u.port, 8443);
}

UTEST(url, trailing_slash) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com/", &u), 0);
    ASSERT_EQ(u.path_len, (size_t)1);
    ASSERT_STREQ(u.path, "/");
}

UTEST(url, ws_basic) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("ws://example.com", &u), 0);
    ASSERT_EQ(u.is_https, 0);
    ASSERT_EQ(u.is_ws, 1);
    ASSERT_EQ(u.host_len, (size_t)11);
    ASSERT_EQ(memcmp(u.host, "example.com", 11), 0);
    ASSERT_EQ(u.port, 80);
    ASSERT_STREQ(u.path, "/");
}

UTEST(url, wss_basic) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("wss://example.com", &u), 0);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(u.is_ws, 1);
    ASSERT_EQ(u.host_len, (size_t)11);
    ASSERT_EQ(memcmp(u.host, "example.com", 11), 0);
    ASSERT_EQ(u.port, 443);
    ASSERT_STREQ(u.path, "/");
}

UTEST(url, ws_with_port) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("ws://localhost:9090/ws", &u), 0);
    ASSERT_EQ(u.is_https, 0);
    ASSERT_EQ(u.is_ws, 1);
    ASSERT_EQ(u.port, 9090);
    ASSERT_EQ(u.host_len, (size_t)9);
    ASSERT_EQ(memcmp(u.host, "localhost", 9), 0);
    ASSERT_EQ(memcmp(u.path, "/ws", 3), 0);
}

UTEST(url, ws_with_path) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("wss://api.example.com/v1/stream?token=abc", &u), 0);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(u.is_ws, 1);
    ASSERT_EQ(u.port, 443);
    ASSERT_EQ(u.host_len, (size_t)15);
    ASSERT_TRUE(u.path_len > 0);
    ASSERT_EQ(memcmp(u.path, "/v1/stream?token=abc", 20), 0);
}

UTEST(url, http_is_not_ws) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com", &u), 0);
    ASSERT_EQ(u.is_ws, 0);
    ASSERT_EQ(kl_url_parse("https://example.com", &u), 0);
    ASSERT_EQ(u.is_ws, 0);
}

/* kl_url_authority: the one Host / absolute-form / :authority builder the HTTP, WebSocket and HTTP/2
 * clients share (RFC 9110 7.2). */
static const char *authority_of(const char *s, char *buf, size_t cap) {
    KlUrl u;
    if (kl_url_parse(s, &u) != 0) return "(parse failed)";
    if (u.is_unix) { u.host = "localhost"; u.host_len = 9; }   /* as the clients do */
    return kl_url_authority(&u, buf, cap) < 0 ? "(no fit)" : buf;
}

UTEST(url, authority_brackets_ipv6_and_keeps_only_non_default_ports) {
    char b[300];
    ASSERT_STREQ("example.com", authority_of("http://example.com/x", b, sizeof b));
    ASSERT_STREQ("example.com", authority_of("https://example.com/x", b, sizeof b));
    ASSERT_STREQ("example.com:8080", authority_of("http://example.com:8080/", b, sizeof b));
    ASSERT_STREQ("example.com:80", authority_of("https://example.com:80/", b, sizeof b));
    ASSERT_STREQ("example.com", authority_of("wss://example.com/", b, sizeof b));
    ASSERT_STREQ("example.com:9000", authority_of("ws://example.com:9000/", b, sizeof b));
    ASSERT_STREQ("[::1]:8443", authority_of("https://[::1]:8443/", b, sizeof b));
    ASSERT_STREQ("[::1]", authority_of("http://[::1]/", b, sizeof b));
    ASSERT_STREQ("localhost", authority_of("http+unix://%2Ftmp%2Fs.sock/x", b, sizeof b));
    ASSERT_STREQ("(no fit)", authority_of("http://example.com:8080/", b, 8));
}

/* The authority ends at '?' and '#' as well as '/', so neither becomes part of the host. A fragment
 * is the client's own and never reaches the request target. A query with no path in front of it
 * has no origin-form target the parser can point at, so it is refused rather than sent wrong. */
UTEST(url, authority_ends_at_query_and_fragment) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com#top", &u), 0);
    ASSERT_EQ(u.host_len, (size_t)11);
    ASSERT_EQ(memcmp(u.host, "example.com", 11), 0);
    ASSERT_EQ(u.path_len, (size_t)1);
    ASSERT_EQ(memcmp(u.path, "/", 1), 0);

    ASSERT_EQ(kl_url_parse("http://example.com:8080#top", &u), 0);
    ASSERT_EQ(u.host_len, (size_t)11);
    ASSERT_EQ(u.port, 8080);
    ASSERT_EQ(u.path_len, (size_t)1);

    ASSERT_EQ(kl_url_parse("http://[::1]#top", &u), 0);
    ASSERT_EQ(u.host_len, (size_t)3);
    ASSERT_EQ(u.path_len, (size_t)1);

    ASSERT_EQ(kl_url_parse("http://example.com?q=1", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com:8080?q=1", &u), -1);
    ASSERT_EQ(kl_url_parse("http://[::1]?q=1", &u), -1);
}

UTEST(url, fragment_is_not_part_of_the_path) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com/a/b?x=1#frag", &u), 0);
    ASSERT_EQ(u.path_len, (size_t)8);
    ASSERT_EQ(memcmp(u.path, "/a/b?x=1", 8), 0);
    ASSERT_EQ(kl_url_parse("http+unix://%2Ftmp%2Fs.sock/v1#frag", &u), 0);
    ASSERT_EQ(u.path_len, (size_t)3);
    ASSERT_EQ(memcmp(u.path, "/v1", 3), 0);
}

/* A space or control byte would split or corrupt the request line. */
UTEST(url, space_and_controls_rejected) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("http://example.com/a b", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com/a\tb", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com/a\x01" "b", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com/a\x7f", &u), -1);
    ASSERT_EQ(kl_url_parse("http://exa mple.com/", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com\x01/", &u), -1);
    ASSERT_EQ(kl_url_parse("http+unix://app.sock/a b", &u), -1);
    ASSERT_EQ(kl_url_parse("http://example.com/a%20b", &u), 0);   /* encoded is fine */
}

/* RFC 3986 3.1: schemes are case-insensitive. */
UTEST(url, scheme_is_case_insensitive) {
    KlUrl u;
    ASSERT_EQ(kl_url_parse("HTTP://example.com/", &u), 0);
    ASSERT_EQ(u.is_https, 0);
    ASSERT_EQ(u.port, 80);
    ASSERT_EQ(kl_url_parse("HtTpS://example.com/", &u), 0);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(u.port, 443);
    ASSERT_EQ(kl_url_parse("WSS://example.com/", &u), 0);
    ASSERT_EQ(u.is_ws, 1);
    ASSERT_EQ(u.is_https, 1);
    ASSERT_EQ(kl_url_parse("HTTP+UNIX://app.sock/", &u), 0);
    ASSERT_EQ(u.is_unix, 1);
}

UTEST(url, resolve_absolute_location_scheme_is_case_insensitive) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://a.example/x", "HTTPS://b.example/y#f", out, sizeof out), 0);
    ASSERT_STREQ(out, "HTTPS://b.example/y");
}

UTEST_MAIN();
