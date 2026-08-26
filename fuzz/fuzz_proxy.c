/*
 * fuzz_proxy.c: libFuzzer target for the PROXY protocol header parser.
 *
 * kl_proxy_parse consumes untrusted bytes from an L4 load-balancer connection:
 * v1 (text, CRLF-terminated) and v2 (binary, 12-byte signature + a 16-bit
 * length field + a variable address block). Length-field binary parsers are a
 * classic source of over-read bugs; this feeds arbitrary bytes and lets
 * ASan/UBSan catch any out-of-bounds read or overflow. Also fuzzes the CIDR
 * list parser on a NUL-terminated view of the same bytes.
 *
 * Build:  make fuzz CC=clang        (Linux)
 *         make fuzz CC=/opt/homebrew/opt/llvm@18/bin/clang   (macOS)
 * Run:    ./fuzz/fuzz_proxy fuzz/corpus_proxy/
 */
#include <keel/proxy_protocol.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t consumed = 0;
    KlSockAddr peer;
    kl_proxy_parse(data, size, &consumed, &peer);

    /* CIDR list parser (config-format, NUL-terminated). */
    char s[512];
    size_t n = size < sizeof(s) - 1 ? size : sizeof(s) - 1;
    memcpy(s, data, n);
    s[n] = '\0';
    KlCidr cidrs[16];
    kl_cidr_parse_list(s, cidrs, 16);
    return 0;
}
