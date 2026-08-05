/*
 * tcp4_get.c — U-0 spike: raw EFI_TCP4 HTTP/1.1 GET under QEMU/OVMF.
 *
 * Proves the EFI_TCP4 completion/token/event/pump model end-to-end, shaping the future
 * KlCompletionOps -> EFI_TCP4 mapping (docs/phase10_uefi_feasibility_design.md §4).
 *
 * NOT Keel code. Throwaway spike. Self-contained: uses efi_min.h (vendored minimal EFI
 * base + boot/console services) + efi_tcp4.h (vendored TCP4/ServiceBinding surface) so it
 * builds under the clang/lld freestanding toolchain (x86_64-unknown-windows PE) with NO
 * gnu-efi runtime dependency — matching the KEEL freestanding toolchain.
 *
 * Flow: LocateHandleBuffer(TCP4 ServiceBinding) -> CreateChild -> OpenProtocol(TCP4 on child)
 *       -> Configure(UseDefaultAddress=TRUE, active) [DHCP settle retry]
 *       -> CreateEvent + Connect -> CheckEvent/Poll pump
 *       -> Transmit(GET) -> pump
 *       -> Receive loop -> print bytes -> stop on FIN/EOF
 *       -> Close -> CloseEvent -> DestroyChild
 *
 * Target responder: 10.0.2.2:<PORT> (QEMU SLIRP host). Guest gets 10.0.2.15 via DHCP.
 */

#include "efi_min.h"
#include "efi_tcp4.h"

#ifndef TARGET_PORT
#define TARGET_PORT 18080
#endif

#define RX_BUFSZ 4096
#define DHCP_RETRIES 40      /* Configure() retries while DHCP settles */
#define DHCP_STALL_US 500000 /* 0.5s between Configure retries */
#define PUMP_SPINS  60000    /* per-op pump iterations (1ms Stall each) */

/* Token completion events: a bare event (type 0, no notify fn) that the TCP4 driver
 * signals on completion and we poll with CheckEvent. NOTE: EVT_NOTIFY_WAIT/SIGNAL
 * REQUIRE a NotifyFunction+NotifyTpl (UEFI 2.10 §7.1.1 CreateEvent) — passing NULL there
 * returns EFI_INVALID_PARAMETER. Type 0 is the correct choice for CheckEvent-polled tokens. */
#define EVT_TOKEN 0

static EFI_GUID gTcp4SbGuid = EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID gTcp4Guid   = EFI_TCP4_PROTOCOL_GUID;

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* ---- tiny console helpers (no libc, no gnu-efi Print) ---- */

static void puts_a(const char *s) {
    CHAR16 w[2]; w[1] = 0;
    for (; *s; s++) {
        if (*s == '\n') { CHAR16 cr[2] = { (CHAR16)'\r', 0 }; ST->ConOut->OutputString(ST->ConOut, cr); }
        w[0] = (CHAR16)(UINT8)*s;
        ST->ConOut->OutputString(ST->ConOut, w);
    }
}

static void write_bytes(const char *b, UINTN n) {
    CHAR16 w[2]; w[1] = 0;
    for (UINTN i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\r') continue;
        if (c == '\n') { CHAR16 cr[2] = { (CHAR16)'\r', 0 }; ST->ConOut->OutputString(ST->ConOut, cr); }
        w[0] = (CHAR16)(UINT8)c;
        ST->ConOut->OutputString(ST->ConOut, w);
    }
}

static void put_u(UINTN v) {
    char tmp[24]; int i = 0;
    if (v == 0) { puts_a("0"); return; }
    while (v && i < 24) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    char out[25]; int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
    puts_a(out);
}

/* Print an EFI_STATUS as 0x<hex> so failures are legible without %r. */
static void put_status(EFI_STATUS s) {
    static const char hex[] = "0123456789abcdef";
    char out[19]; out[0]='0'; out[1]='x'; int j=2;
    for (int shift = 60; shift >= 0; shift -= 4)
        out[j++] = hex[(s >> shift) & 0xF];
    out[j]=0;
    puts_a(out);
}

static void put_ip(const EFI_IPv4_ADDRESS *a) {
    put_u(a->Addr[0]); puts_a("."); put_u(a->Addr[1]); puts_a(".");
    put_u(a->Addr[2]); puts_a("."); put_u(a->Addr[3]);
}

/* Pump the TCP4 stack until the token event fires or we time out. Returns 1 if signaled. */
static int pump_until(EFI_TCP4_PROTOCOL *tcp, EFI_EVENT ev) {
    for (int spins = 0; spins < PUMP_SPINS; spins++) {
        tcp->Poll(tcp);
        if (BS->CheckEvent(ev) == EFI_SUCCESS) return 1;
        BS->Stall(1000);
    }
    return 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    ST = SystemTable;
    BS = SystemTable->BootServices;
    EFI_STATUS st;

    puts_a("\nU-0: EFI_TCP4 spike starting\n");

    /* 1. Locate the TCP4 ServiceBinding protocol. */
    UINTN       nHandles = 0;
    EFI_HANDLE *handles  = NULL;
    st = BS->LocateHandleBuffer(ByProtocol, &gTcp4SbGuid, NULL, &nHandles, &handles);
    if (EFI_ERROR(st) || nHandles == 0) {
        puts_a("U-0: NO EFI_TCP4_SERVICE_BINDING_PROTOCOL found (status="); put_status(st); puts_a(")\n");
        puts_a("U-0: This OVMF build ships no TCP4/network stack (SnpDxe/Ip4Dxe/Tcp4Dxe).\n");
        puts_a("U-0: NO-GO-YET (needs network-enabled OVMF: build edk2 OvmfPkg -D NETWORK_ENABLE)\n");
        return EFI_SUCCESS;
    }
    puts_a("U-0: found "); put_u(nHandles); puts_a(" TCP4 ServiceBinding handle(s)\n");

    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    st = BS->HandleProtocol(handles[0], &gTcp4SbGuid, (VOID **)&sb);
    if (EFI_ERROR(st) || !sb) { puts_a("U-0: HandleProtocol(SB) failed\n"); return EFI_SUCCESS; }

    /* 2. CreateChild -> OpenProtocol(TCP4) on the child handle. */
    EFI_HANDLE child = NULL;
    st = sb->CreateChild(sb, &child);
    if (EFI_ERROR(st)) { puts_a("U-0: CreateChild failed "); put_status(st); puts_a("\n"); return EFI_SUCCESS; }

    EFI_TCP4_PROTOCOL *tcp = NULL;
    st = BS->OpenProtocol(child, &gTcp4Guid, (VOID **)&tcp,
                          ImageHandle, child, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st) || !tcp) {
        st = BS->OpenProtocol(child, &gTcp4Guid, (VOID **)&tcp,
                              ImageHandle, child, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    }
    if (EFI_ERROR(st) || !tcp) {
        puts_a("U-0: OpenProtocol(TCP4 child) failed "); put_status(st); puts_a("\n");
        sb->DestroyChild(sb, child);
        return EFI_SUCCESS;
    }
    puts_a("U-0: TCP4 child protocol opened\n");

    /* 3. Configure active (UseDefaultAddress=TRUE => DHCP), retry until DHCP settles. */
    EFI_TCP4_CONFIG_DATA cfg;
    ZeroMem(&cfg, sizeof(cfg));
    cfg.TimeToLive = 64;
    cfg.AccessPoint.UseDefaultAddress = TRUE;
    cfg.AccessPoint.ActiveFlag        = TRUE;
    cfg.AccessPoint.RemotePort        = TARGET_PORT;
    cfg.AccessPoint.RemoteAddress.Addr[0] = 10;
    cfg.AccessPoint.RemoteAddress.Addr[1] = 0;
    cfg.AccessPoint.RemoteAddress.Addr[2] = 2;
    cfg.AccessPoint.RemoteAddress.Addr[3] = 2;

    int configured = 0;
    for (int i = 0; i < DHCP_RETRIES; i++) {
        st = tcp->Configure(tcp, &cfg);
        if (!EFI_ERROR(st)) { configured = 1; break; }
        if (st == EFI_NO_MAPPING) { tcp->Poll(tcp); BS->Stall(DHCP_STALL_US); continue; }
        puts_a("U-0: Configure failed "); put_status(st); puts_a("\n");
        break;
    }
    if (!configured) {
        puts_a("U-0: Configure never succeeded (DHCP did not settle?)\n");
        puts_a("U-0: NO-GO-YET (DHCP / address mapping)\n");
        goto cleanup_child;
    }
    puts_a("U-0: Configure OK (address mapped via DHCP)\n");

    {
        EFI_TCP4_CONFIG_DATA mode;
        ZeroMem(&mode, sizeof(mode));
        if (!EFI_ERROR(tcp->GetModeData(tcp, NULL, &mode, NULL, NULL, NULL))) {
            puts_a("U-0: station "); put_ip(&mode.AccessPoint.StationAddress);
            puts_a(" -> remote "); put_ip(&cfg.AccessPoint.RemoteAddress);
            puts_a(":"); put_u(TARGET_PORT); puts_a("\n");
        }
    }

    /* 4. Connect. */
    EFI_TCP4_CONNECTION_TOKEN conn;
    ZeroMem(&conn, sizeof(conn));
    st = BS->CreateEvent(EVT_TOKEN, TPL_CALLBACK, NULL, NULL, &conn.CompletionToken.Event);
    if (EFI_ERROR(st)) { puts_a("U-0: CreateEvent(connect) "); put_status(st); puts_a("\n"); goto cleanup_child; }

    st = tcp->Connect(tcp, &conn);
    if (EFI_ERROR(st)) { puts_a("U-0: Connect submit "); put_status(st); puts_a("\n"); goto cleanup_connevt; }
    if (!pump_until(tcp, conn.CompletionToken.Event)) { puts_a("U-0: Connect timed out\n"); goto cleanup_connevt; }
    if (EFI_ERROR(conn.CompletionToken.Status)) {
        puts_a("U-0: Connect status "); put_status(conn.CompletionToken.Status); puts_a("\n");
        puts_a("U-0: NO-GO-YET (TCP connect failed)\n");
        goto cleanup_connevt;
    }
    puts_a("U-0: TCP connected\n");

    /* 5. Transmit the raw GET. */
    static const char req[] =
        "GET / HTTP/1.1\r\n"
        "Host: 10.0.2.2\r\n"
        "Connection: close\r\n"
        "\r\n";

    EFI_TCP4_TRANSMIT_DATA tx;
    ZeroMem(&tx, sizeof(tx));
    tx.Push = TRUE;
    tx.DataLength = (UINT32)(sizeof(req) - 1);
    tx.FragmentCount = 1;
    tx.FragmentTable[0].FragmentLength = (UINT32)(sizeof(req) - 1);
    tx.FragmentTable[0].FragmentBuffer = (VOID *)req;

    EFI_TCP4_IO_TOKEN txt;
    ZeroMem(&txt, sizeof(txt));
    txt.Packet.TxData = &tx;
    st = BS->CreateEvent(EVT_TOKEN, TPL_CALLBACK, NULL, NULL, &txt.CompletionToken.Event);
    if (EFI_ERROR(st)) { puts_a("U-0: CreateEvent(tx) "); put_status(st); puts_a("\n"); goto cleanup_conn; }

    st = tcp->Transmit(tcp, &txt);
    if (EFI_ERROR(st)) { puts_a("U-0: Transmit submit "); put_status(st); puts_a("\n"); goto cleanup_txevt; }
    if (!pump_until(tcp, txt.CompletionToken.Event)) { puts_a("U-0: Transmit timed out\n"); goto cleanup_txevt; }
    if (EFI_ERROR(txt.CompletionToken.Status)) {
        puts_a("U-0: Transmit status "); put_status(txt.CompletionToken.Status); puts_a("\n");
        goto cleanup_txevt;
    }
    puts_a("U-0: GET transmitted\n");

    /* 6. Receive loop. Capture the first line as the status marker. */
    static char statusline[256];
    UINTN statuslen = 0;
    int got_status = 0;

    puts_a("U-0: --- response begin ---\n");

    EFI_TCP4_IO_TOKEN rxt;
    ZeroMem(&rxt, sizeof(rxt));
    st = BS->CreateEvent(EVT_TOKEN, TPL_CALLBACK, NULL, NULL, &rxt.CompletionToken.Event);
    if (EFI_ERROR(st)) { puts_a("U-0: CreateEvent(rx) "); put_status(st); puts_a("\n"); goto cleanup_txevt; }

    static char rxbuf[RX_BUFSZ];
    int eof = 0;
    for (int loop = 0; loop < 400 && !eof; loop++) {
        EFI_TCP4_RECEIVE_DATA rx;
        ZeroMem(&rx, sizeof(rx));
        rx.DataLength = RX_BUFSZ;
        rx.FragmentCount = 1;
        rx.FragmentTable[0].FragmentLength = RX_BUFSZ;
        rx.FragmentTable[0].FragmentBuffer = rxbuf;

        rxt.Packet.RxData = &rx;
        rxt.CompletionToken.Status = EFI_NOT_READY;

        st = tcp->Receive(tcp, &rxt);
        if (st == EFI_CONNECTION_FIN) { eof = 1; break; }
        if (EFI_ERROR(st)) { puts_a("U-0: Receive submit "); put_status(st); puts_a("\n"); break; }

        if (!pump_until(tcp, rxt.CompletionToken.Event)) { puts_a("\nU-0: receive timed out\n"); break; }

        EFI_STATUS rs = rxt.CompletionToken.Status;
        if (rs == EFI_CONNECTION_FIN) eof = 1;
        else if (EFI_ERROR(rs)) { puts_a("\nU-0: Receive status "); put_status(rs); puts_a("\n"); break; }

        UINTN got = rx.DataLength;
        if (got > 0) {
            if (!got_status) {
                for (UINTN i = 0; i < got && statuslen < sizeof(statusline) - 1; i++) {
                    char c = rxbuf[i];
                    if (c == '\r' || c == '\n') { got_status = 1; break; }
                    statusline[statuslen++] = c;
                }
                statusline[statuslen] = 0;
            }
            write_bytes(rxbuf, got);
        }
    }

    puts_a("\nU-0: --- response end ---\n");

    if (got_status && statuslen > 0) {
        puts_a("U-0: EFI_TCP4 GET status = "); puts_a(statusline); puts_a("\n");
        puts_a("U-0: GO\n");
    } else {
        puts_a("U-0: no status line captured\n");
        puts_a("U-0: NO-GO-YET (connected but no HTTP response)\n");
    }

    BS->CloseEvent(rxt.CompletionToken.Event);

cleanup_txevt:
    BS->CloseEvent(txt.CompletionToken.Event);
cleanup_conn:
    {
        EFI_TCP4_CLOSE_TOKEN ct;
        ZeroMem(&ct, sizeof(ct));
        if (BS->CreateEvent(EVT_TOKEN, TPL_CALLBACK, NULL, NULL, &ct.CompletionToken.Event) == EFI_SUCCESS) {
            if (tcp->Close(tcp, &ct) == EFI_SUCCESS) pump_until(tcp, ct.CompletionToken.Event);
            BS->CloseEvent(ct.CompletionToken.Event);
        }
    }
cleanup_connevt:
    BS->CloseEvent(conn.CompletionToken.Event);
cleanup_child:
    tcp->Configure(tcp, NULL);
    BS->CloseProtocol(child, &gTcp4Guid, ImageHandle, child);
    sb->DestroyChild(sb, child);
    if (handles) BS->FreePool(handles);

    puts_a("U-0: done\n");
    return EFI_SUCCESS;
}
