/*
 * u5_selftest.c — U-5 acceptance self-test (UEFI EFI application).
 *
 * Extends U-3: a STOCK libkeel_freestanding.a async KlClient resolves a DNS HOSTNAME
 * (via a real A-record query over EFI_UDP4 to a configured nameserver — U-5,
 * dns_uefi.c, wired into kl_resolve_sync by resolve_uefi.c under -DKL_U5_DNS) then
 * connects (EFI_TCP4, U-2/U-3) and GETs → 200. PLAINTEXT HTTP (DNS is the focus; TLS
 * is proven in U-4).
 *
 * Flow:
 *   kl_uefi_platform_init(bs, st)          → monotonic clock + EFI_RNG
 *   kl_uefi_dns_init(bs, image)            → stash bs/image for the DNS path
 *   kl_uefi_event_provider(bs, image)      → the completion provider (+ lazy socket)
 *   kl_event_ctx_init_ex(&ev, alloc, ep)   → inject the provider on the stock archive
 *   kl_client_start(GET http://<NAME>:PORT/) → kl_resolve_sync(NAME) → DNS/UDP4 →
 *                                            post_connect → send/recv → parse → on_done
 *
 * Prints:
 *   U-5: resolving <NAME> ...
 *   U-5: resolved <NAME> -> a.b.c.d
 *   U-5: status = <n>
 *   U-5: GO            (done && status == 200)   |   U-5: NO-GO-YET
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"
#include "event_efi.h"
#include "allocator_uefi.h"
#include "dns_uefi.h"

#include <keel/client.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

#ifndef TARGET_PORT
#define TARGET_PORT 18080
#endif
#ifndef KL_U5_HOSTNAME
#define KL_U5_HOSTNAME "keel.test"
#endif

/* Build the hostname URL literal at compile time. */
#define STR2(x) #x
#define STR(x) STR2(x)
#define TARGET_URL "http://" KL_U5_HOSTNAME ":" STR(TARGET_PORT) "/"

/* ── tiny ASCII console (no libc) ─────────────────────────────────────────────── */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_out;

static void print(const char *s) {
    CHAR16 buf[256];
    UINTN i = 0;
    while (s[i] && i < 254) { buf[i] = (CHAR16)(UINT8)s[i]; i++; }
    buf[i] = 0;
    if (g_out) g_out->OutputString(g_out, buf);
}
static void print_line(const char *s) { print(s); print("\r\n"); }

static void write_bytes(const char *b, UINTN n) {
    CHAR16 w[2]; w[1] = 0;
    for (UINTN i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\r') continue;
        if (c == '\n') { CHAR16 cr[2] = { (CHAR16)'\r', 0 }; if (g_out) g_out->OutputString(g_out, cr); }
        w[0] = (CHAR16)(UINT8)c;
        if (g_out) g_out->OutputString(g_out, w);
    }
}

static void print_int(int v) {
    char tmp[16]; int n = 0;
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) print("-");
    if (u == 0) { print("0"); return; }
    while (u && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    char out[17]; int o = 0;
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
    print(out);
}

static void print_ipv4(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        print_int((int)ip[i]);
        if (i < 3) print(".");
    }
}

/* ── the async completion callback ────────────────────────────────────────────── */
typedef struct { int done; int status; KlError err; const char *body; size_t body_len; } DoneCtx;

static void on_done(KlClient *cl, void *ud) {
    DoneCtx *d = ud;
    const KlClientResponse *r = kl_client_response(cl);
    if (r) {
        d->status = r->status;
        d->body = r->body;
        d->body_len = r->body_len;
    } else {
        d->status = -1;
    }
    d->err = kl_client_last_error(cl);
    d->done = 1;
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== U-5 KlClient GET over EFI_TCP4 with DNS-over-EFI_UDP4 ===");
    print("U-5: target = "); print_line(TARGET_URL);

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("U-5: (warn) platform_init failed — clock stuck, continuing");

    /* Stash bs/image for the synchronous DNS-over-UDP4 path (kl_resolve_sync has no
     * bs/image in its signature). */
    kl_uefi_dns_init(bs, image_handle);

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("U-5: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("U-5: kl_event_ctx_init_ex failed (no compatible socket provider?)");
        print_line("U-5: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("U-5: event ctx up (efi-tcp4-completion)");

    print("U-5: resolving "); print(KL_U5_HOSTNAME); print_line(" ...");

    KlClientConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.timeout_ms = 30000;   /* generous: DNS round trip + DHCP + TCG + SLIRP */
    /* No cfg.resolver → client_pick_resolver returns NULL under FREESTANDING → the
     * kl_resolve_sync in resolve_uefi.c resolves the hostname via EFI_UDP4 DNS. */

    DoneCtx d;
    { unsigned char *p = (unsigned char *)&d; for (size_t i = 0; i < sizeof(d); i++) p[i] = 0; }

    KlClient *c = kl_client_start(&ev, &alloc, &cfg, "GET", TARGET_URL,
                                  NULL, 0, NULL, 0, on_done, &d);
    if (!c) { print_line("U-5: kl_client_start failed (resolve failed?)"); kl_event_ctx_free(&ev); goto park; }
    print_line("U-5: client started; pumping the completion loop...");

    for (int tick = 0; tick < 200000 && !d.done; tick++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }

    /* Report the resolved address (captured by dns_uefi during resolve). */
    { uint8_t ip[4];
      if (kl_uefi_dns_last_ip(ip)) {
          print("U-5: resolved "); print(KL_U5_HOSTNAME); print(" -> "); print_ipv4(ip); print_line("");
      } else {
          print_line("U-5: resolved -> (no DNS answer captured)");
      }
    }

    if (d.done) {
        print("U-5: status = "); print_int(d.status); print_line("");
        if (d.status >= 0 && d.body && d.body_len > 0) {
            print_line("U-5: --- body begin ---");
            write_bytes(d.body, d.body_len < 512 ? d.body_len : 512);
            print_line("");
            print_line("U-5: --- body end ---");
        }
        if (d.err != KL_ERR_NONE) { print("U-5: error = "); print_int((int)d.err); print_line(""); }
    } else {
        print_line("U-5: did not complete within the tick bound");
    }

    kl_client_free(c);
    kl_event_ctx_free(&ev);

    if (d.done && d.status == 200)
        print_line("U-5: GO");
    else
        print_line("U-5: NO-GO-YET");

park:
    print_line("U-5: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
