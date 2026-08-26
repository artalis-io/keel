/*
 * u2_selftest.c: socket-provider acceptance self-test (UEFI EFI application).
 *
 * Proves the EFI_TCP4 KlSocketProvider's DATA PLANE end-to-end over real firmware:
 * build kl_uefi_socket_provider, then drive a raw HTTP GET / ENTIRELY through the
 * PROVIDER's ops:
 *   provider->ops->socket()            → CreateChild + OpenProtocol (a KlUefiConn)
 *   provider->ops->connect()           → Configure (active, DHCP, remote addr)
 *   kl_uefi_socket_connect_now()       → issue + pump the Connect token (the completion
 *                                        backend's post_connect, done inline here since the
 *                                        completion backend is not built in this test)
 *   provider->ops->send()              → Transmit(GET) + pump
 *   provider->ops->recv()              → Receive + pump (loop until FIN)
 *   provider->ops->get_local_addr()    → station address (DHCP)
 *   provider->ops->close()             → Close/CloseEvent/CloseProtocol/DestroyChild
 *
 * Prints:
 *   socket provider send/recv OK
 *   status = <status line>
 *   GO
 *
 * The completion/connect ORCHESTRATION (races, deadlines, watcher relay) is the
 * completion backend's job; this test proves the socket provider's send/recv works over EFI_TCP4.
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"

#include <keel/socket.h>
#include <keel/sockaddr.h>

#include <stdint.h>
#include <stddef.h>

#ifndef TARGET_PORT
#define TARGET_PORT 18080
#endif

/* The completion backend's post_connect done inline (declared in socket_efi_tcp4.c). */
int kl_uefi_socket_connect_now(KlSocketHandle fd);

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

/* Print raw bytes (CR filtered, LF -> CRLF): used to echo the response body. */
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

static UINTN u64_to_dec(UINT64 v, char *dst, UINTN cap) {
    char tmp[24]; UINTN n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    UINTN o = 0;
    while (n && o < cap - 1) dst[o++] = tmp[--n];
    dst[o] = 0;
    return o;
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== U-2 EFI_TCP4 socket-provider self-test ===");

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("U-2: (warn) platform_init failed, clock stuck, continuing");

    /* 1. Build the provider (locates the TCP4 ServiceBinding). */
    const KlSocketProvider *p = kl_uefi_socket_provider(bs, image_handle);
    if (!p) {
        print_line("U-2: NO EFI_TCP4 ServiceBinding, this OVMF has no network stack");
        print_line("U-2: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("U-2: provider built (efi-tcp4)");

    const KlSocketOps *ops = p->ops;
    void *cx = p->context;

    /* 2. socket(): CreateChild + OpenProtocol. */
    KlSocketHandle fd = ops->socket(cx, /*AF_INET*/2, /*SOCK_STREAM*/1, 0);
    if (!kl_handle_valid(fd)) { print_line("U-2: socket() failed"); goto park; }
    print_line("U-2: socket() OK (child + TCP4 protocol)");

    /* 3. connect(): the SYNC op does Configure (returns -1/pending by contract). */
    KlSockAddr remote;
    { uint8_t ip[4] = { 10, 0, 2, 2 }; kl_sockaddr_from_ipv4(&remote, ip, TARGET_PORT); }
    (void)ops->connect(cx, fd, &remote);   /* -1 + would-block (Configure done, connect pending) */
    if (ops->io_status(cx) == KL_IO_FATAL) {
        print_line("U-2: connect()/Configure failed (fatal)");
        ops->close(cx, fd);
        goto park;
    }
    print_line("U-2: connect() Configure OK (DHCP settled, connect pending)");

    /* 3b. Establish the connection (the completion backend's post_connect, inline here). */
    if (kl_uefi_socket_connect_now(fd) != 0) {
        print_line("U-2: Connect token failed");
        ops->close(cx, fd);
        goto park;
    }
    print_line("U-2: TCP connected");

    /* Station address via get_local_addr (GetModeData). */
    {
        KlSockAddr local;
        if (ops->get_local_addr(cx, fd, &local) == 0) {
            char line[KL_SOCKADDR_STRLEN];
            if (kl_sockaddr_format(&local, line, sizeof(line)) > 0) {
                print("U-2: station "); print_line(line);
            }
        }
    }

    /* 4. send() the raw GET via the PROVIDER. */
    static const char req[] =
        "GET / HTTP/1.1\r\n"
        "Host: 10.0.2.2\r\n"
        "Connection: close\r\n"
        "\r\n";
    kl_ssize_t sent = ops->send(cx, fd, req, sizeof(req) - 1);
    if (sent != (kl_ssize_t)(sizeof(req) - 1)) {
        print_line("U-2: send() did not send the full GET");
        ops->close(cx, fd);
        goto park;
    }
    print_line("U-2: send() GET OK");

    /* 5. recv() loop via the PROVIDER: capture the first line as the status marker. */
    static char statusline[256];
    UINTN statuslen = 0;
    int got_status = 0;
    int recv_ok = 0;

    print_line("U-2: --- response begin ---");
    static char rxbuf[4096];
    /* recv() is NON-BLOCKING: WOULD_BLOCK means "no data yet". Pump the
     * stack ourselves via a short Stall between spins (the provider no longer loops
     * internally), bounded so a wedged connection fails rather than hangs. */
    for (int loop = 0; loop < 60000; loop++) {
        kl_ssize_t n = ops->recv(cx, fd, rxbuf, sizeof(rxbuf));
        if (n == 0) break;                 /* FIN / EOF */
        if (n < 0) {
            KlIoStatus s = ops->io_status(cx);
            if (s == KL_IO_WOULD_BLOCK) { bs->Stall(1000); continue; }  /* pump + retry */
            if (s == KL_IO_CLOSED) break;
            print_line("\nU-2: recv() error");
            break;
        }
        recv_ok = 1;
        if (!got_status) {
            for (kl_ssize_t i = 0; i < n && statuslen < sizeof(statusline) - 1; i++) {
                char c = rxbuf[i];
                if (c == '\r' || c == '\n') { got_status = 1; break; }
                statusline[statuslen++] = c;
            }
            statusline[statuslen] = 0;
        }
        write_bytes(rxbuf, (UINTN)n);
    }
    print_line("");
    print_line("U-2: --- response end ---");

    /* 6. close() via the PROVIDER (teardown). */
    ops->close(cx, fd);

    if (recv_ok && got_status && statuslen > 0) {
        print_line("U-2: socket provider send/recv OK");
        print("U-2: status = "); print_line(statusline);
        print_line("U-2: GO");
    } else {
        print_line("U-2: send/recv did not yield an HTTP status line");
        print_line("U-2: NO-GO-YET");
    }

park:
    /* Park so the markers stay last on the serial line (harness terminates QEMU). */
    print_line("U-2: (parked; harness will terminate QEMU)");
    { char dt[24]; (void)u64_to_dec(0, dt, sizeof(dt)); }
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
