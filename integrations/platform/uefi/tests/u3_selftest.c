/*
 * u3_selftest.c — plaintext-client acceptance self-test (UEFI EFI application).
 *
 * A STOCK libkeel_freestanding.a async KlHttpClient runs an
 * HTTP/1.1 GET on bare UEFI firmware, driven by the EFI completion backend
 * (event_efi.c) over the EFI_TCP4 socket provider (socket_efi_tcp4.c), with the
 * platform/allocator shims and a numeric resolver (resolve_uefi.c). No epoll /
 * kqueue / io_uring, no OS sockets, no errno, no libc — just the firmware's event
 * services pumping EFI_TCP4 completion tokens.
 *
 * Flow (all through the PUBLIC KlHttpClient API — the client is model-blind):
 *   kl_uefi_platform_init(bs, st)          → monotonic clock + EFI_RNG
 *   kl_uefi_event_provider(bs, image)      → the completion provider (+ lazy socket provider)
 *   kl_event_ctx_init_ex(&ev, alloc, ep)   → inject the provider on the stock archive
 *   kl_http_client_start(GET http://10.0.2.2:PORT/)  → resolve(numeric) → post_connect →
 *                                            drain KL_COMP_CONNECT → send/recv via the
 *                                            watcher relay → parse → on_done
 *   pump: kl_event_ctx_run + kl_timer_fire until done or bounded ticks
 *
 * Prints:
 *   status = <n>
 *   body: <bytes>
 *   GO            (done && status == 200)   |   NO-GO-YET
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"
#include "event_efi.h"
#include "allocator_uefi.h"

#include <keel/http_client.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

#ifndef TARGET_PORT
#define TARGET_PORT 18080
#endif
#ifndef TARGET_HOST
#define TARGET_HOST "10.0.2.2"      /* SLIRP host/gateway from the guest */
#endif

/* Build the numeric URL literal at compile time. */
#define STR2(x) #x
#define STR(x) STR2(x)
#define TARGET_URL "http://" TARGET_HOST ":" STR(TARGET_PORT) "/"

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

/* ── the async completion callback ────────────────────────────────────────────── */
typedef struct { int done; int status; KlError err; const char *body; size_t body_len; } DoneCtx;

static void on_done(KlHttpClient *cl, void *ud) {
    DoneCtx *d = ud;
    const KlHttpClientResponse *r = kl_http_client_response(cl);
    if (r) {
        d->status = r->status;
        d->body = r->body;
        d->body_len = r->body_len;
    } else {
        d->status = -1;
    }
    d->err = kl_http_client_last_error(cl);
    d->done = 1;
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== U-3 KlHttpClient GET over EFI_TCP4 completion backend ===");
    print("U-3: target = "); print_line(TARGET_URL);

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("U-3: (warn) platform_init failed — clock stuck, continuing");

    /* Inject the EFI completion provider on the stock freestanding archive. The
     * provider's native_provider() lazily builds the EFI_TCP4 socket provider; if this
     * OVMF has no TCP4 stack that returns NULL and ctx init fails. */
    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("U-3: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("U-3: kl_event_ctx_init_ex failed (no compatible socket provider?)");
        print_line("U-3: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("U-3: event ctx up (efi-tcp4-completion)");

    KlHttpClientConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.timeout_ms = 20000;   /* generous: DHCP + TCG + SLIRP */
    /* No cfg.resolver → client_pick_resolver returns NULL under FREESTANDING → the
     * numeric kl_resolve_sync (resolve_uefi.c) resolves the dotted-quad host. */

    DoneCtx d;
    { unsigned char *p = (unsigned char *)&d; for (size_t i = 0; i < sizeof(d); i++) p[i] = 0; }

    KlHttpClient *c = kl_http_client_start(&ev, &alloc, &cfg, "GET", TARGET_URL,
                                  NULL, 0, NULL, 0, on_done, &d);
    if (!c) { print_line("U-3: kl_http_client_start failed"); kl_event_ctx_free(&ev); goto park; }
    print_line("U-3: client started; pumping the completion loop...");

    /* Drive the loop until the request completes (or a generous tick bound). Each
     * tick drains completions (connect + watcher relay) then fires timers (the
     * request deadline). The socket provider's send/recv pump the tokens internally. */
    for (int tick = 0; tick < 200000 && !d.done; tick++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }

    if (d.done) {
        print("U-3: status = "); print_int(d.status); print_line("");
        if (d.status >= 0 && d.body && d.body_len > 0) {
            print_line("U-3: --- body begin ---");
            write_bytes(d.body, d.body_len < 512 ? d.body_len : 512);
            print_line("");
            print_line("U-3: --- body end ---");
        }
        if (d.err != KL_ERR_NONE) { print("U-3: error = "); print_int((int)d.err); print_line(""); }
    } else {
        print_line("U-3: did not complete within the tick bound");
    }

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);

    if (d.done && d.status == 200)
        print_line("U-3: GO");
    else
        print_line("U-3: NO-GO-YET");

park:
    print_line("U-3: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
