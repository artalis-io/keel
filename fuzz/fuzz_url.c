/*
 * fuzz_url.c — libFuzzer target for the URL parser.
 *
 * kl_url_parse is untrusted on the client side: redirect Location headers are
 * server-controlled, and the parser has a CRLF-injection guard, IPv6/host/port
 * splitting, and an AF_UNIX path decoder (http+unix://) — all exercised here
 * against arbitrary bytes (NUL-terminated).
 *
 * Build:  make fuzz CC=clang
 * Run:    ./fuzz/fuzz_url fuzz/corpus_url/
 */
#include <keel/url.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char s[8193];
    size_t n = size < sizeof(s) - 1 ? size : sizeof(s) - 1;
    memcpy(s, data, n);
    s[n] = '\0';

    KlUrl out;
    kl_url_parse(s, &out);
    return 0;
}
