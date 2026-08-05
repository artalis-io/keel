/*
 * host_map_test.c — HOST unit test for the PURE EFI_TCP4 mappings (no firmware).
 *
 * Unit-tests the two pure, firmware-independent pieces of socket_efi_tcp4.c without
 * QEMU:
 *   - kl_efi_status_to_io()  over the full EFI_STATUS set,
 *   - kl_efi_status_to_error() spot checks,
 *   - KlSockAddr ⇄ EFI_IPv4_ADDRESS round-trip.
 *
 * It is a standalone host harness (NOT a tests/test_*.c auto-discovered suite) so it
 * needs no root-Makefile change: build_host_map_test.sh compiles socket_efi_tcp4.c
 * for the host (the ops table is present but never called here — only the pure fns)
 * and links libkeel.a for kl_sockaddr_*. The EFI types come from the vendored
 * efi_min.h/efi_tcp4.h, which compile cleanly on the host (pure typedefs).
 *
 * Exit 0 = all pass; non-zero = first failure printed.
 */

#include "socket_efi_tcp4.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); g_fail = 1; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

static const char *io_name(KlIoStatus s) {
    switch (s) {
        case KL_IO_OK:          return "OK";
        case KL_IO_WOULD_BLOCK: return "WOULD_BLOCK";
        case KL_IO_INTERRUPTED: return "INTERRUPTED";
        case KL_IO_PENDING:     return "PENDING";
        case KL_IO_CLOSED:      return "CLOSED";
        case KL_IO_RESET:       return "RESET";
        case KL_IO_FATAL:       return "FATAL";
        default:                return "?";
    }
}

int main(void) {
    printf("=== U-2 host mapping test (kl_efi_status_to_io + sockaddr) ===\n");

    /* ── kl_efi_status_to_io over the full EFI_STATUS set ── */
    struct { EFI_STATUS st; KlIoStatus want; const char *label; } cases[] = {
        { EFI_SUCCESS,            KL_IO_OK,          "EFI_SUCCESS -> OK" },
        { EFI_NOT_READY,          KL_IO_WOULD_BLOCK, "EFI_NOT_READY -> WOULD_BLOCK" },
        { EFI_TIMEOUT,            KL_IO_WOULD_BLOCK, "EFI_TIMEOUT -> WOULD_BLOCK" },
        { EFI_NO_MAPPING,         KL_IO_WOULD_BLOCK, "EFI_NO_MAPPING -> WOULD_BLOCK" },
        { EFI_CONNECTION_FIN,     KL_IO_CLOSED,      "EFI_CONNECTION_FIN -> CLOSED" },
        { EFI_CONNECTION_RESET,   KL_IO_RESET,       "EFI_CONNECTION_RESET -> RESET" },
        { EFI_CONNECTION_REFUSED, KL_IO_RESET,       "EFI_CONNECTION_REFUSED -> RESET" },
        { EFI_INVALID_PARAMETER,  KL_IO_FATAL,       "EFI_INVALID_PARAMETER -> FATAL" },
        { EFI_UNSUPPORTED,        KL_IO_FATAL,       "EFI_UNSUPPORTED -> FATAL" },
        { EFI_DEVICE_ERROR,       KL_IO_FATAL,       "EFI_DEVICE_ERROR -> FATAL" },
        { EFI_OUT_OF_RESOURCES,   KL_IO_FATAL,       "EFI_OUT_OF_RESOURCES -> FATAL" },
        { EFI_ACCESS_DENIED,      KL_IO_FATAL,       "EFI_ACCESS_DENIED -> FATAL" },
        { EFI_ABORTED,            KL_IO_FATAL,       "EFI_ABORTED -> FATAL" },
        { EFI_LOAD_ERROR,         KL_IO_FATAL,       "EFI_LOAD_ERROR -> FATAL" },
        { EFI_NOT_FOUND,          KL_IO_FATAL,       "EFI_NOT_FOUND -> FATAL" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KlIoStatus got = kl_efi_status_to_io(cases[i].st);
        if (got != cases[i].want) {
            printf("FAIL: %s (got %s)\n", cases[i].label, io_name(got));
            g_fail = 1;
        } else {
            printf("ok:   %s\n", cases[i].label);
        }
    }

    /* ── kl_efi_status_to_error spot checks ── */
    CHECK(kl_efi_status_to_error(EFI_SUCCESS) == KL_ERR_NONE,   "err: SUCCESS -> NONE");
    CHECK(kl_efi_status_to_error(EFI_TIMEOUT) == KL_ERR_TIMEOUT,"err: TIMEOUT -> TIMEOUT");
    CHECK(kl_efi_status_to_error(EFI_CONNECTION_REFUSED) == KL_ERR_CONNECT,
          "err: REFUSED -> CONNECT");
    CHECK(kl_efi_status_to_error(EFI_NO_MAPPING) == KL_ERR_DNS, "err: NO_MAPPING -> DNS");
    CHECK(kl_efi_status_to_error(EFI_OUT_OF_RESOURCES) == KL_ERR_ALLOC,
          "err: OUT_OF_RESOURCES -> ALLOC");

    /* ── KlSockAddr ⇄ EFI_IPv4_ADDRESS round-trip ── */
    {
        uint8_t ip[4] = { 10, 0, 2, 2 };
        KlSockAddr a;
        int r = kl_sockaddr_from_ipv4(&a, ip, 18080);
        CHECK(r == 0, "sockaddr_from_ipv4 builds v4");

        EFI_IPv4_ADDRESS efi;
        UINT16 port = 0;
        r = kl_efi_sockaddr_to_ipv4(&a, &efi, &port);
        CHECK(r == 0, "to_ipv4 accepts v4");
        CHECK(efi.Addr[0] == 10 && efi.Addr[1] == 0 &&
              efi.Addr[2] == 2  && efi.Addr[3] == 2, "to_ipv4 bytes match (10.0.2.2)");
        CHECK(port == 18080, "to_ipv4 port preserved (18080)");

        KlSockAddr b;
        r = kl_efi_ipv4_to_sockaddr(&efi, port, &b);
        CHECK(r == 0, "ipv4_to_sockaddr builds v4");
        CHECK(kl_sockaddr_family(&b) == KL_AF_INET, "round-trip family == INET");
        CHECK(kl_sockaddr_port(&b) == 18080, "round-trip port == 18080");
        CHECK(memcmp(a.u.ip, b.u.ip, 4) == 0, "round-trip address bytes equal");
        CHECK(kl_sockaddr_equal(&a, &b), "round-trip sockaddr_equal");
    }

    /* ── IPv6 rejection ── */
    {
        uint8_t ip6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
        KlSockAddr v6;
        kl_sockaddr_from_ipv6(&v6, ip6, 443, 0);
        EFI_IPv4_ADDRESS efi;
        int r = kl_efi_sockaddr_to_ipv4(&v6, &efi, NULL);
        CHECK(r == -1, "to_ipv4 REJECTS IPv6 (-1)");
    }

    printf(g_fail ? "\nU-2 host map test: FAIL\n" : "\nU-2 host map test: PASS\n");
    return g_fail;
}
