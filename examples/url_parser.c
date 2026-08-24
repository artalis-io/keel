/*
 * url_parser.c: URL parsing demo
 *
 * Concepts: kl_url_parse, http/https/ws/wss schemes, IPv6 addresses,
 * default port selection, KlUrl field inspection, error handling,
 * CRLF injection rejection, zero-allocation design.
 *
 * Build:  make examples
 * Run:    ./examples/url_parser
 */

#include <keel/keel.h>
#include <stdio.h>

static void parse_and_print(const char *url) {
    KlUrl u;
    int rc = kl_url_parse(url, &u);

    printf("  %-45s ", url);
    if (rc < 0) {
        printf("-> ERROR (rejected)\n");
        return;
    }

    printf("-> host=%.*s port=%d path=%.*s",
           (int)u.host_len, u.host, u.port,
           (int)u.path_len, u.path);
    if (u.is_https) printf(" [TLS]");
    if (u.is_ws) printf(" [WS]");
    printf("\n");
}

int main(void) {
    printf("url_parser example\n\n");

    printf("--- Valid URLs ---\n");
    parse_and_print("http://example.com/path");
    parse_and_print("https://example.com/api/v1?key=val");
    parse_and_print("http://localhost:3000/");
    parse_and_print("https://example.com");
    parse_and_print("ws://localhost:8080/ws");
    parse_and_print("wss://secure.example.com/stream");

    printf("\n--- IPv6 ---\n");
    parse_and_print("http://[::1]:8080/hello");
    parse_and_print("https://[2001:db8::1]/api");
    parse_and_print("http://[::1]/");

    printf("\n--- Default ports ---\n");
    parse_and_print("http://example.com/");
    parse_and_print("https://example.com/");
    parse_and_print("ws://example.com/");
    parse_and_print("wss://example.com/");

    printf("\n--- Invalid / rejected ---\n");
    parse_and_print("ftp://example.com/");
    parse_and_print("http://evil.com\\r\\nInjection: yes");
    parse_and_print("");

    return 0;
}
