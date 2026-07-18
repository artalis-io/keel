/*
 * fuzz_dns.c — libFuzzer target for the DNS response parser.
 *
 * The parser consumes untrusted network input (server responses). This target
 * feeds arbitrary bytes through kl_dns_parse_response for both address families
 * and a couple of transaction IDs; ASan/UBSan catch any out-of-bounds read,
 * compression-pointer loop, or overflow.
 *
 * Build:  make fuzz CC=clang        (Linux)
 *         make fuzz CC=/opt/homebrew/opt/llvm@18/bin/clang   (macOS)
 * Run:    ./fuzz/fuzz_dns fuzz/corpus_dns/
 */
#include <keel/dns_resolver.h>
#include <keel/resolver.h>

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlResolveResult out;

    /* Transaction-ID mismatch path + both families (no question verification). */
    kl_dns_parse_response(data, size, 0x0000, KL_DNS_TYPE_A, NULL, 0, &out);
    kl_dns_parse_response(data, size, 0xFFFF, KL_DNS_TYPE_AAAA, NULL, 0, &out);

    /* Matching-ID path: parse the id from the packet so the answer walk runs. */
    if (size >= 2) {
        uint16_t id = (uint16_t)((data[0] << 8) | data[1]);
        kl_dns_parse_response(data, size, id, KL_DNS_TYPE_A, NULL, 0, &out);
        kl_dns_parse_response(data, size, id, KL_DNS_TYPE_AAAA, NULL, 0, &out);
        /* Question-verification path: feed part of the packet as expected bytes
         * so the compare + skip logic runs against hostile input. */
        if (size >= 16)
            kl_dns_parse_response(data, size, id, KL_DNS_TYPE_A, data + 12, 8, &out);
    }
    return 0;
}
