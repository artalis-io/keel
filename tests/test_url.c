#include "utest.h"
#include <keel/url.h>
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

UTEST_MAIN();
