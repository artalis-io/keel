/*
 * socket_efi_tcp4.c — KlSocketProvider over EFI_TCP4 (U-2, Phase 10 F-8).
 *
 * See socket_efi_tcp4.h for the design + the connect-vs-configure split. This TU is
 * the data plane (send/recv over Transmit/Receive + the Poll()+CheckEvent pump) plus
 * the child/token/generation lifecycle carried on the KlUefiConn handle. No libc, no
 * errno — every -1 is classified via kl_efi_status_to_io off the conn's last status.
 *
 * The KlUefiConn is the handle: KlSocketHandle (intptr_t) holds a KlUefiConn* — NOT
 * the raw EFI_TCP4_PROTOCOL* — so the handle carries child ownership + preallocated
 * tokens + a generation, and a completion delivered after close (memory reused) is
 * rejected by the generation check (see kl_efi_conn_valid).
 */

#include "socket_efi_tcp4.h"
#include "allocator_uefi.h"           /* kl_uefi_allocator */
#include "../../src/socket.h"         /* KL_SOCK_CAP_OVERLAPPED (internal cap bit) */

/* ── errno (WRITE-only) ──────────────────────────────────────────────────────
 * U-6 makes recv non-blocking; the mbedTLS BIO (tls_mbedtls.c) detects a
 * would-block by reading errno==EAGAIN, so the provider MUST set it on the
 * would-block/error -1 paths (matching the lwip provider contract). We only
 * WRITE errno here; the definition lives in u1_link_stubs.c (linked by every
 * U-3/U-4/U-5 build). No libc errno.h — the freestanding shim may lack it —
 * so declare the global + the two codes locally. */
extern int errno;
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EIO
#define EIO 5
#endif

/* Provider-owned receive buffer: big enough to usually hold a whole TCP segment
 * so a single posted Receive satisfies most recv() calls (U-6). */
#ifndef KL_EFI_RXBUF
#define KL_EFI_RXBUF 8192
#endif

/* ── EFI_TCP4_PROTOCOL_GUID / ServiceBinding GUID (file-scope, mutable per the
 * EFI ABI which takes EFI_GUID* non-const) ─────────────────────────────────────── */
static EFI_GUID g_tcp4_sb_guid = EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID g_tcp4_guid    = EFI_TCP4_PROTOCOL_GUID;

/* Token completion events are bare (type 0): EVT_NOTIFY_WAIT/SIGNAL REQUIRE a
 * non-NULL NotifyFunction (UEFI 2.10 §7.1.1) — passing NULL there returns
 * EFI_INVALID_PARAMETER. Type 0 is the correct choice for CheckEvent-polled tokens
 * (U-0 finding #1). */
#define EFI_TCP4_EVT_TOKEN 0

/* Bounded per-op pump. Each spin does Poll()+CheckEvent()+Stall(1ms). The overall
 * request deadline is owned above the seam (KlClientConfig.timeout_ms); this bound
 * only stops a wedged single op from spinning forever. */
#ifndef KL_EFI_PUMP_SPINS
#define KL_EFI_PUMP_SPINS 60000   /* ~60 s worst case at 1 ms/spin (matches U-0) */
#endif
#define KL_EFI_PUMP_STALL_US 1000

/* Configure()/DHCP settle: retry EFI_NO_MAPPING while the default address binds
 * (U-0 finding #4). Bounded so a network-less run fails rather than hangs. */
#ifndef KL_EFI_DHCP_RETRIES
#define KL_EFI_DHCP_RETRIES 40
#endif
#define KL_EFI_DHCP_STALL_US 500000  /* 0.5 s between Configure retries */

/* A KlUefiConn generation sentinel written into freed memory would be unreliable
 * (the allocator may reuse it), so validity is proved structurally: a live conn's
 * `magic` field equals KL_EFI_CONN_MAGIC and its generation is odd (bumped to even
 * on close). A completion holding {conn,gen} checks both. */
#define KL_EFI_CONN_MAGIC 0x544350344b4c00ecULL  /* "TCP4KL" tag + 0xec */

/* ── the handle: KlUefiConn ────────────────────────────────────────────────────── */
typedef struct KlUefiConn {
    UINT64                magic;        /* KL_EFI_CONN_MAGIC while live; cleared on free */
    UINT64                generation;   /* bumped on close (stale-completion rejection) */

    EFI_BOOT_SERVICES    *bs;
    EFI_HANDLE            image;        /* the loaded image (OpenProtocol AgentHandle) */

    EFI_HANDLE            child;        /* ServiceBinding->CreateChild result */
    EFI_TCP4_PROTOCOL    *tcp;          /* OpenProtocol(child, EFI_TCP4_PROTOCOL) */
    EFI_SERVICE_BINDING_PROTOCOL *sb;   /* borrowed from the provider ctx */

    /* Preallocated per-op token+event records (created once at socket(), closed at
     * close()). Reused across the conn's lifetime — no per-op CreateEvent in the
     * hot path beyond the one-time creation here. */
    EFI_TCP4_CONNECTION_TOKEN conn_tok;
    EFI_TCP4_IO_TOKEN         tx_tok;
    EFI_TCP4_IO_TOKEN         rx_tok;
    EFI_TCP4_CLOSE_TOKEN      close_tok;
    int                       events_created;  /* the 4 token events exist */

    EFI_TCP4_CONFIG_DATA  config;       /* pinned config (built in connect()) */
    int                   configured;   /* Configure() succeeded */
    int                   connected;    /* the Connect token completed OK */
    int                   dead;         /* closed / torn down — no further I/O */

    EFI_STATUS            last_status;  /* most-recent op's raw EFI_STATUS (io_status) */

    /* ── U-6 non-blocking receive state ──────────────────────────────────────
     * A Receive token posted into rx_buf is polled (not pumped) by
     * kl_uefi_socket_recv_ready; efi_sock_recv drains rx_buf without blocking.
     *   rx_posted  — a Receive token is outstanding (awaiting completion)
     *   rx_ready   — rx_buf holds [rx_off,rx_len) unconsumed bytes
     *   rx_eof     — peer sent FIN (orderly EOF, sticky)
     *   rx_err     — a fatal receive error latched (rx_err_status carries it)
     */
    int                       rx_posted;
    int                       rx_ready;
    int                       rx_eof;
    EFI_STATUS                rx_err;      /* latched fatal status (0 == none) */
    size_t                    rx_len;      /* bytes in rx_buf */
    size_t                    rx_off;      /* consume cursor into rx_buf */
    EFI_TCP4_RECEIVE_DATA     rx_data;     /* the posted Receive's descriptor */
    unsigned char             rx_buf[KL_EFI_RXBUF];

    KlAllocator           alloc;        /* by-value copy of the provider's allocator */
} KlUefiConn;

/* ── provider context (file-scope single instance) ─────────────────────────────── */
typedef struct {
    EFI_BOOT_SERVICES            *bs;
    EFI_HANDLE                    image;
    EFI_SERVICE_BINDING_PROTOCOL *sb;
    EFI_HANDLE                   *sb_handles;   /* LocateHandleBuffer result (to FreePool) */
    KlAllocator                   alloc;
    int                           created;
} KlUefiSockCtx;

static KlUefiSockCtx g_ctx;

/* ── pure mappings (host-testable) ─────────────────────────────────────────────── */

KlIoStatus kl_efi_status_to_io(EFI_STATUS st) {
    switch (st) {
        case EFI_SUCCESS:            return KL_IO_OK;
        case EFI_NOT_READY:          return KL_IO_WOULD_BLOCK;
        case EFI_TIMEOUT:            return KL_IO_WOULD_BLOCK;
        case EFI_NO_MAPPING:         return KL_IO_WOULD_BLOCK;
        case EFI_CONNECTION_FIN:     return KL_IO_CLOSED;
        case EFI_CONNECTION_RESET:   return KL_IO_RESET;
        case EFI_CONNECTION_REFUSED: return KL_IO_RESET;
        default:
            /* Any other error bit set → fatal; any other success → OK. */
            return EFI_ERROR(st) ? KL_IO_FATAL : KL_IO_OK;
    }
}

KlError kl_efi_status_to_error(EFI_STATUS st) {
    switch (st) {
        case EFI_SUCCESS:            return KL_ERR_NONE;
        case EFI_NOT_READY:          return KL_ERR_IO;      /* transient — retryable */
        case EFI_TIMEOUT:            return KL_ERR_TIMEOUT;
        case EFI_NO_MAPPING:         return KL_ERR_DNS;     /* address not bound (DHCP) */
        case EFI_CONNECTION_FIN:     return KL_ERR_IO;      /* orderly EOF */
        case EFI_CONNECTION_RESET:   return KL_ERR_IO;
        case EFI_CONNECTION_REFUSED: return KL_ERR_CONNECT;
        case EFI_INVALID_PARAMETER:  return KL_ERR_INVALID_ARG;
        case EFI_OUT_OF_RESOURCES:   return KL_ERR_ALLOC;
        case EFI_ACCESS_DENIED:      return KL_ERR_CONNECT;
        default:                     return EFI_ERROR(st) ? KL_ERR_IO : KL_ERR_NONE;
    }
}

/* ── KlSockAddr ⇄ EFI_IPv4_ADDRESS ─────────────────────────────────────────────── */

int kl_efi_sockaddr_to_ipv4(const KlSockAddr *a, EFI_IPv4_ADDRESS *out, UINT16 *out_port) {
    if (!a || !out) return -1;
    if (kl_sockaddr_family(a) != KL_AF_INET) return -1;   /* IPv6 / unix rejected */
    /* KlSockAddr.u.ip[0..3] is network order (wire) — EFI_IPv4_ADDRESS.Addr is the
     * same on-the-wire byte order, so a straight copy. */
    for (int i = 0; i < 4; i++) out->Addr[i] = a->u.ip[i];
    if (out_port) *out_port = (UINT16)kl_sockaddr_port(a);
    return 0;
}

int kl_efi_ipv4_to_sockaddr(const EFI_IPv4_ADDRESS *ip, UINT16 port, KlSockAddr *out) {
    if (!ip || !out) return -1;
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = ip->Addr[i];
    return kl_sockaddr_from_ipv4(out, bytes, (uint16_t)port);
}

/* ── conn helpers ──────────────────────────────────────────────────────────────── */

static KlUefiConn *conn_of(KlSocketHandle fd) {
    if (!kl_handle_valid(fd)) return NULL;
    KlUefiConn *c = (KlUefiConn *)(void *)(intptr_t)fd;
    if (!c || c->magic != KL_EFI_CONN_MAGIC) return NULL;
    return c;
}

/* Validate a {conn, generation} pair — the stale-completion guard U-3 will use. A
 * conn whose memory was freed + reused will (almost surely) fail the magic check;
 * a conn that was closed + a NEW conn built in the same slot will have a different
 * (bumped) generation. Both are rejected here. Exposed for U-3 / the selftest. */
int kl_uefi_conn_valid(KlUefiConn *c, UINT64 generation);
int kl_uefi_conn_valid(KlUefiConn *c, UINT64 generation) {
    return c && c->magic == KL_EFI_CONN_MAGIC && c->generation == generation && !c->dead;
}

/* Pump the TCP4 stack until @ev fires or the bound elapses. Returns 1 if signaled. */
static int pump_until(KlUefiConn *c, EFI_EVENT ev) {
    EFI_BOOT_SERVICES *bs = c->bs;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    for (int spins = 0; spins < KL_EFI_PUMP_SPINS; spins++) {
        tcp->Poll(tcp);
        if (bs->CheckEvent(ev) == EFI_SUCCESS) return 1;
        bs->Stall(KL_EFI_PUMP_STALL_US);
    }
    return 0;
}

/* Create the 4 preallocated token events (bare type-0). Returns 0 / -1. */
static int create_events(KlUefiConn *c) {
    EFI_BOOT_SERVICES *bs = c->bs;
    EFI_STATUS st;
    st = bs->CreateEvent(EFI_TCP4_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                         &c->conn_tok.CompletionToken.Event);
    if (EFI_ERROR(st)) return -1;
    st = bs->CreateEvent(EFI_TCP4_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                         &c->tx_tok.CompletionToken.Event);
    if (EFI_ERROR(st)) goto fail1;
    st = bs->CreateEvent(EFI_TCP4_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                         &c->rx_tok.CompletionToken.Event);
    if (EFI_ERROR(st)) goto fail2;
    st = bs->CreateEvent(EFI_TCP4_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                         &c->close_tok.CompletionToken.Event);
    if (EFI_ERROR(st)) goto fail3;
    c->events_created = 1;
    return 0;
fail3:
    bs->CloseEvent(c->rx_tok.CompletionToken.Event);
    c->rx_tok.CompletionToken.Event = NULL;
fail2:
    bs->CloseEvent(c->tx_tok.CompletionToken.Event);
    c->tx_tok.CompletionToken.Event = NULL;
fail1:
    bs->CloseEvent(c->conn_tok.CompletionToken.Event);
    c->conn_tok.CompletionToken.Event = NULL;
    return -1;
}

/* Close all 4 token events (exactly once — guarded by events_created). */
static void close_events(KlUefiConn *c) {
    if (!c->events_created) return;
    EFI_BOOT_SERVICES *bs = c->bs;
    if (c->conn_tok.CompletionToken.Event)  bs->CloseEvent(c->conn_tok.CompletionToken.Event);
    if (c->tx_tok.CompletionToken.Event)    bs->CloseEvent(c->tx_tok.CompletionToken.Event);
    if (c->rx_tok.CompletionToken.Event)    bs->CloseEvent(c->rx_tok.CompletionToken.Event);
    if (c->close_tok.CompletionToken.Event) bs->CloseEvent(c->close_tok.CompletionToken.Event);
    c->conn_tok.CompletionToken.Event = NULL;
    c->tx_tok.CompletionToken.Event = NULL;
    c->rx_tok.CompletionToken.Event = NULL;
    c->close_tok.CompletionToken.Event = NULL;
    c->events_created = 0;
}

/* ── socket ops ────────────────────────────────────────────────────────────────── */

/* socket(): CreateChild + OpenProtocol → a fresh KlUefiConn (generation odd). IPv4 /
 * SOCK_STREAM only. Returns the conn pointer as a KlSocketHandle, or KL_INVALID_SOCKET. */
#ifndef SOCK_STREAM
#define SOCK_STREAM 1   /* the seam passes the Keel/POSIX numeric; only STREAM is honored */
#endif

static KlSocketHandle efi_sock_socket(void *cx, int domain, int type, int protocol) {
    KlUefiSockCtx *ctx = (KlUefiSockCtx *)cx;
    (void)protocol;
    if (!ctx || !ctx->created || !ctx->sb) return KL_INVALID_SOCKET;
    /* IPv4 + STREAM only. The seam passes the POSIX numeric domain (AF_INET=2) and
     * type (SOCK_STREAM=1); reject anything but a stream socket. Family enforcement
     * on the address happens at connect() (the neutral KlSockAddr carries it). */
    if (type != SOCK_STREAM) return KL_INVALID_SOCKET;
    (void)domain;

    KlUefiConn *c = (KlUefiConn *)kl_malloc(&ctx->alloc, sizeof(*c));
    if (!c) return KL_INVALID_SOCKET;
    /* zero-init (no libc memset guaranteed in this TU — do it by hand). */
    { unsigned char *p = (unsigned char *)c; for (size_t i = 0; i < sizeof(*c); i++) p[i] = 0; }
    c->bs    = ctx->bs;
    c->image = ctx->image;
    c->sb    = ctx->sb;
    c->alloc = ctx->alloc;
    c->generation = 1;   /* odd == live */

    EFI_STATUS st = ctx->sb->CreateChild(ctx->sb, &c->child);
    if (EFI_ERROR(st) || !c->child) {
        c->last_status = st;
        kl_free(&ctx->alloc, c, sizeof(*c));
        return KL_INVALID_SOCKET;
    }

    st = ctx->bs->OpenProtocol(c->child, &g_tcp4_guid, (VOID **)&c->tcp,
                               ctx->image, c->child, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st) || !c->tcp) {
        st = ctx->bs->OpenProtocol(c->child, &g_tcp4_guid, (VOID **)&c->tcp,
                                   ctx->image, c->child, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    }
    if (EFI_ERROR(st) || !c->tcp) {
        c->last_status = st;
        ctx->sb->DestroyChild(ctx->sb, c->child);
        kl_free(&ctx->alloc, c, sizeof(*c));
        return KL_INVALID_SOCKET;
    }

    if (create_events(c) != 0) {
        ctx->bs->CloseProtocol(c->child, &g_tcp4_guid, ctx->image, c->child);
        ctx->sb->DestroyChild(ctx->sb, c->child);
        kl_free(&ctx->alloc, c, sizeof(*c));
        return KL_INVALID_SOCKET;
    }

    c->magic = KL_EFI_CONN_MAGIC;
    c->last_status = EFI_SUCCESS;
    return (KlSocketHandle)(intptr_t)(void *)c;
}

/*
 * connect(): the SYNC socket op. Per the split (see the header), this does the
 * Configure ONLY — active mode, UseDefaultAddress (DHCP), remote addr/port from the
 * KlSockAddr — tolerating EFI_NO_MAPPING (retry while DHCP settles, U-0 #4). It does
 * NOT issue the Connect token; the actual async connect (Connect + pump) is U-3's
 * completion post_connect (or kl_uefi_socket_connect_now() for the U-2 selftest).
 *
 * A blocking connect is nonsensical over token completions, so — like lwip-raw's
 * ENOTSUP connect — this returns -1 with the conn's io_status set to KL_IO_PENDING
 * (via last_status = EFI_NOT_READY), telling the caller "connect is in progress,
 * drive it on the completion loop", not "connect failed".
 */
/* kl_uefi_socket_configure — the Configure half of connect (active/DHCP/remote),
 * factored out so both the sync socket op (efi_sock_connect) and the async
 * completion backend (event_efi.c post_connect) share it. 0 = configured, -1 =
 * failed (c->last_status carries the class). Idempotent once configured. */
int kl_uefi_socket_configure(KlSocketHandle fd, const KlSockAddr *a) {
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return -1;
    if (c->configured) return 0;
    if (!a || kl_sockaddr_family(a) != KL_AF_INET) {   /* IPv6 unsupported (EFI_TCP6 later) */
        c->last_status = EFI_INVALID_PARAMETER;
        return -1;
    }

    EFI_IPv4_ADDRESS remote;
    UINT16 rport = 0;
    if (kl_efi_sockaddr_to_ipv4(a, &remote, &rport) != 0) {
        c->last_status = EFI_INVALID_PARAMETER;
        return -1;
    }

    /* Build the pinned active config: DHCP default address, remote from the addr. */
    EFI_TCP4_CONFIG_DATA *cfg = &c->config;
    { unsigned char *p = (unsigned char *)cfg; for (size_t i = 0; i < sizeof(*cfg); i++) p[i] = 0; }
    cfg->TimeToLive = 64;
    cfg->AccessPoint.UseDefaultAddress = TRUE;
    cfg->AccessPoint.ActiveFlag        = TRUE;
    cfg->AccessPoint.RemotePort        = rport;
    cfg->AccessPoint.RemoteAddress     = remote;

    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    EFI_STATUS st = EFI_NOT_READY;
    for (int i = 0; i < KL_EFI_DHCP_RETRIES; i++) {
        st = tcp->Configure(tcp, cfg);
        if (!EFI_ERROR(st)) { c->configured = 1; break; }
        if (st == EFI_NO_MAPPING) {      /* DHCP not settled — pump + retry (U-0 #4) */
            tcp->Poll(tcp);
            c->bs->Stall(KL_EFI_DHCP_STALL_US);
            continue;
        }
        break;   /* a real Configure error */
    }

    if (!c->configured) {
        c->last_status = st;   /* NO_MAPPING → WOULD_BLOCK; other → fatal */
        return -1;
    }
    return 0;
}

static int efi_sock_connect(void *cx, KlSocketHandle fd, const KlSockAddr *a) {
    (void)cx;
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return -1;

    if (kl_uefi_socket_configure(fd, a) != 0)
        return -1;   /* c->last_status already set (invalid arg / DHCP fatal) */

    /* Configure done; the connect token is the completion backend's job. Report
     * "pending" so the caller drives it on the completion loop (KL_IO_PENDING). */
    c->last_status = EFI_NOT_READY;   /* NOT_READY → KL_IO_WOULD_BLOCK; see io_status note */
    return -1;
}

/* connect_post — issue the Connect token WITHOUT pumping (the async counterpart of
 * kl_uefi_socket_connect_now's issue step). 0 = posted, -1 = immediate error. */
int kl_uefi_socket_connect_post(KlSocketHandle fd) {
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead || !c->configured) return -1;
    if (c->connected) return 0;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    c->conn_tok.CompletionToken.Status = EFI_NOT_READY;
    EFI_STATUS st = tcp->Connect(tcp, &c->conn_tok);
    if (EFI_ERROR(st)) { c->last_status = st; return -1; }
    return 0;
}

/* connect_poll — pump once + test the Connect token. 1 = terminal (*out_ok set),
 * 0 = still pending, -1 = invalid handle. The async counterpart of the pump loop
 * inside kl_uefi_socket_connect_now. */
int kl_uefi_socket_connect_poll(KlSocketHandle fd, int *out_ok) {
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return -1;
    if (c->connected) { if (out_ok) *out_ok = 1; return 1; }
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    tcp->Poll(tcp);
    if (c->bs->CheckEvent(c->conn_tok.CompletionToken.Event) != EFI_SUCCESS)
        return 0;   /* not signaled yet */
    EFI_STATUS st = c->conn_tok.CompletionToken.Status;
    c->last_status = st;
    int ok = !EFI_ERROR(st);
    if (ok) c->connected = 1;
    if (out_ok) *out_ok = ok;
    return 1;
}

/* Handle-based generation helpers for the completion backend's stale guard. */
unsigned long long kl_uefi_conn_generation_h(KlSocketHandle fd) {
    KlUefiConn *c = conn_of(fd);
    return c ? (unsigned long long)c->generation : 0ULL;
}
int kl_uefi_conn_valid_h(KlSocketHandle fd, unsigned long long generation) {
    return kl_uefi_conn_valid(conn_of(fd), (UINT64)generation);
}

/*
 * kl_uefi_socket_connect_now — issue the Connect token + pump it to completion,
 * synchronously. This is what U-3's completion post_connect will do asynchronously;
 * exposed here so the U-2 selftest (which has no completion backend) can establish
 * the connection inline after efi_sock_connect() has done the Configure. Returns 0
 * on connected, -1 on failure (conn->last_status carries the EFI_STATUS).
 *
 * Precondition: connect() (Configure) has already run (c->configured).
 */
int kl_uefi_socket_connect_now(KlSocketHandle fd);
int kl_uefi_socket_connect_now(KlSocketHandle fd) {
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead || !c->configured) return -1;
    if (c->connected) return 0;

    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    c->conn_tok.CompletionToken.Status = EFI_NOT_READY;
    EFI_STATUS st = tcp->Connect(tcp, &c->conn_tok);
    if (EFI_ERROR(st)) { c->last_status = st; return -1; }
    if (!pump_until(c, c->conn_tok.CompletionToken.Event)) {
        c->last_status = EFI_TIMEOUT;
        return -1;
    }
    st = c->conn_tok.CompletionToken.Status;
    c->last_status = st;
    if (EFI_ERROR(st)) return -1;
    c->connected = 1;
    return 0;
}

/* Classify a send-path EFI_STATUS into a POSIX-ish errno for the mbedTLS BIO:
 * WOULD_BLOCK-class (NOT_READY/TIMEOUT/NO_MAPPING) → EAGAIN, else EIO. */
static int send_errno(EFI_STATUS st) {
    switch (kl_efi_status_to_io(st)) {
        case KL_IO_WOULD_BLOCK: return EAGAIN;
        default:                return EIO;
    }
}

/*
 * send(): Transmit one fragment + pump. Returns bytes accepted (== len on success),
 * or -1 with last_status set. Mirrors lwr_sock_send: the emulated-readiness data
 * path driven by the pump. (EFI_TCP4 Transmit takes the whole buffer; the token
 * completes when the stack has queued it — treated as the full write.)
 */
static kl_ssize_t efi_sock_send(void *cx, KlSocketHandle fd, const void *buf, size_t len) {
    (void)cx;
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) { errno = EIO; return -1; }
    if (!c->connected) { c->last_status = EFI_NOT_READY; errno = EAGAIN; return -1; }  /* connect first */
    if (len == 0) { c->last_status = EFI_SUCCESS; return 0; }
    /* EFI_TCP4 DataLength / FragmentLength are UINT32 — bound the single fragment. */
    if (len > 0xFFFFFFFFu) len = 0xFFFFFFFFu;

    EFI_TCP4_TRANSMIT_DATA tx;
    { unsigned char *p = (unsigned char *)&tx; for (size_t i = 0; i < sizeof(tx); i++) p[i] = 0; }
    tx.Push = TRUE;
    tx.DataLength = (UINT32)len;
    tx.FragmentCount = 1;
    tx.FragmentTable[0].FragmentLength = (UINT32)len;
    tx.FragmentTable[0].FragmentBuffer = (VOID *)(uintptr_t)buf;

    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    c->tx_tok.Packet.TxData = &tx;
    c->tx_tok.CompletionToken.Status = EFI_NOT_READY;

    EFI_STATUS st = tcp->Transmit(tcp, &c->tx_tok);
    if (EFI_ERROR(st)) { c->last_status = st; errno = send_errno(st); return -1; }
    if (!pump_until(c, c->tx_tok.CompletionToken.Event)) {
        c->last_status = EFI_TIMEOUT;
        errno = EAGAIN;   /* pump deadline == retry, not fatal (WOULD_BLOCK class) */
        return -1;
    }
    st = c->tx_tok.CompletionToken.Status;
    c->last_status = st;
    if (EFI_ERROR(st)) { errno = send_errno(st); return -1; }
    return (kl_ssize_t)len;
}

/*
 * kl_uefi_socket_recv_ready — the U-6 non-blocking READ-readiness probe the
 * completion drain calls. Returns 1 iff recv() can return something now
 * (buffered data, EOF, or a latched error), 0 if still pending. Never pumps.
 *
 * State machine (per KlUefiConn):
 *   - Already have unconsumed buffered data / EOF / error  → 1 immediately.
 *   - No Receive posted → post one into rx_buf. If Receive() returns
 *     EFI_CONNECTION_FIN synchronously, latch rx_eof → 1; another error →
 *     latch rx_err → 1; success → rx_posted=1 (token outstanding).
 *   - A Receive is outstanding → Poll() once + CheckEvent(rx_tok):
 *       not signaled → 0 (still pending);
 *       signaled     → rx_posted=0, read rx_tok.Status:
 *                        FIN   → rx_eof=1 → 1
 *                        error → rx_err=status → 1
 *                        else  → rx_len=rx_data.DataLength, rx_off=0,
 *                                rx_ready=1 → 1.
 */
int kl_uefi_socket_recv_ready(KlSocketHandle fd) {
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return 0;

    /* Anything already available to hand back? */
    if ((c->rx_ready && c->rx_off < c->rx_len) || c->rx_eof || c->rx_err)
        return 1;
    /* rx_ready but fully drained — clear the stale flag before deciding. */
    if (c->rx_ready && c->rx_off >= c->rx_len) c->rx_ready = 0;

    if (!c->connected) return 0;   /* nothing to receive before connect */

    EFI_TCP4_PROTOCOL *tcp = c->tcp;

    if (!c->rx_posted) {
        /* Post a Receive into the provider-owned buffer. */
        EFI_TCP4_RECEIVE_DATA *rx = &c->rx_data;
        { unsigned char *p = (unsigned char *)rx; for (size_t i = 0; i < sizeof(*rx); i++) p[i] = 0; }
        rx->DataLength = (UINT32)KL_EFI_RXBUF;
        rx->FragmentCount = 1;
        rx->FragmentTable[0].FragmentLength = (UINT32)KL_EFI_RXBUF;
        rx->FragmentTable[0].FragmentBuffer = c->rx_buf;

        c->rx_tok.Packet.RxData = rx;
        c->rx_tok.CompletionToken.Status = EFI_NOT_READY;
        EFI_STATUS st = tcp->Receive(tcp, &c->rx_tok);
        if (st == EFI_CONNECTION_FIN) { c->rx_eof = 1; c->last_status = st; return 1; }
        if (EFI_ERROR(st)) { c->rx_err = st; c->last_status = st; return 1; }
        c->rx_posted = 1;
    }

    /* Poll the stack once and test the outstanding Receive token — no pump. */
    tcp->Poll(tcp);
    if (c->bs->CheckEvent(c->rx_tok.CompletionToken.Event) != EFI_SUCCESS)
        return 0;   /* still pending */

    c->rx_posted = 0;
    EFI_STATUS st = c->rx_tok.CompletionToken.Status;
    c->last_status = st;
    if (st == EFI_CONNECTION_FIN) { c->rx_eof = 1; return 1; }
    if (EFI_ERROR(st)) { c->rx_err = st; return 1; }
    c->rx_len = (size_t)c->rx_data.DataLength;
    c->rx_off = 0;
    c->rx_ready = 1;
    return 1;
}

/* Copy up to @len buffered bytes out of rx_buf into @dst (byte loop, no libc
 * memcpy), advancing rx_off; clears rx_ready when the buffer is drained. */
static kl_ssize_t rx_consume(KlUefiConn *c, void *dst, size_t len) {
    size_t avail = c->rx_len - c->rx_off;
    size_t n = len < avail ? len : avail;
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = c->rx_buf[c->rx_off + i];
    c->rx_off += n;
    if (c->rx_off >= c->rx_len) { c->rx_ready = 0; c->rx_len = c->rx_off = 0; }
    c->last_status = EFI_SUCCESS;
    return (kl_ssize_t)n;
}

/*
 * recv(): NON-BLOCKING (U-6). Serves buffered data first; otherwise probes
 * readiness ONCE (kl_uefi_socket_recv_ready, which posts + polls a Receive
 * without pumping). Returns bytes (>0), 0 on FIN (EOF), or -1 would-block/error.
 * On a would-block -1 sets errno=EAGAIN (mbedTLS BIO would-block detection); on a
 * fatal error -1 sets errno=EIO. NO internal pump loop — the drain drives it.
 */
static kl_ssize_t efi_sock_recv(void *cx, KlSocketHandle fd, void *buf, size_t len) {
    (void)cx;
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) { if (c) c->last_status = EFI_INVALID_PARAMETER; errno = EIO; return -1; }
    if (!c->connected) { c->last_status = EFI_NOT_READY; errno = EAGAIN; return -1; }
    if (len == 0) { c->last_status = EFI_SUCCESS; return 0; }

    /* 1. Buffered data from an earlier posted Receive. */
    if (c->rx_ready && c->rx_off < c->rx_len)
        return rx_consume(c, buf, len);

    /* 2. Sticky EOF / latched error. */
    if (c->rx_eof) { c->last_status = EFI_CONNECTION_FIN; return 0; }
    if (c->rx_err) {
        c->last_status = c->rx_err;
        c->rx_err = EFI_SUCCESS;   /* consume the latch */
        errno = EIO;
        return -1;
    }

    /* 3. Probe once (posts/polls a Receive, no pump). */
    if (kl_uefi_socket_recv_ready(fd)) {
        if (c->rx_ready && c->rx_off < c->rx_len)
            return rx_consume(c, buf, len);
        if (c->rx_eof) { c->last_status = EFI_CONNECTION_FIN; return 0; }
        if (c->rx_err) {
            c->last_status = c->rx_err;
            c->rx_err = EFI_SUCCESS;
            errno = EIO;
            return -1;
        }
    }

    /* 4. Still pending → would-block. */
    c->last_status = EFI_NOT_READY;
    errno = EAGAIN;
    return -1;
}

/*
 * close(): teardown per U-0 finding #6. Configure(NULL) reset → Close(await) →
 * CloseEvent all → CloseProtocol → DestroyChild → kl_free (generation bumped even).
 * Exactly-once, no leak, no stale-completion UAF.
 */
static int efi_sock_close(void *cx, KlSocketHandle fd) {
    (void)cx;
    KlUefiConn *c = conn_of(fd);
    if (!c) return 0;               /* already invalid — nothing to do */
    if (c->dead) return 0;          /* idempotent */
    c->dead = 1;
    c->generation++;                /* even == closed: reject any late {conn,gen} */

    EFI_BOOT_SERVICES *bs = c->bs;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;

    /* Graceful Close (async) — pump its token. AbortOnClose=FALSE: flush then FIN. */
    if (tcp && c->events_created && c->close_tok.CompletionToken.Event) {
        c->close_tok.AbortOnClose = FALSE;
        c->close_tok.CompletionToken.Status = EFI_NOT_READY;
        EFI_STATUS st = tcp->Close(tcp, &c->close_tok);
        if (!EFI_ERROR(st)) pump_until(c, c->close_tok.CompletionToken.Event);
    }

    close_events(c);                /* CloseEvent all 4 (exactly once) */

    if (tcp) tcp->Configure(tcp, NULL);   /* reset config (U-0 #6) */

    if (c->child) {
        bs->CloseProtocol(c->child, &g_tcp4_guid, c->image, c->child);
        if (c->sb) c->sb->DestroyChild(c->sb, c->child);
    }

    c->magic = 0;   /* dead: conn_of() on a stale handle now returns NULL */
    kl_free(&c->alloc, c, sizeof(*c));
    return 0;
}

/* get_local_addr(): GetModeData → the DHCP-assigned station address → KlSockAddr. */
static int efi_sock_get_local_addr(void *cx, KlSocketHandle fd, KlSockAddr *addr) {
    (void)cx;
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead || !addr || !c->tcp) return -1;

    EFI_TCP4_CONFIG_DATA mode;
    { unsigned char *p = (unsigned char *)&mode; for (size_t i = 0; i < sizeof(mode); i++) p[i] = 0; }
    EFI_STATUS st = c->tcp->GetModeData(c->tcp, NULL, &mode, NULL, NULL, NULL);
    c->last_status = st;
    if (EFI_ERROR(st)) return -1;
    return kl_efi_ipv4_to_sockaddr(&mode.AccessPoint.StationAddress,
                                   mode.AccessPoint.StationPort, addr);
}

/* io_status(): classify the last op's stored EFI_STATUS. The provider ctx is not
 * per-conn, so we cannot key on a specific conn here — the seam calls io_status()
 * with only the provider ctx right after a -1 on a specific fd. We therefore stash
 * the last status on the CONTEXT too, updated by every op below via last_ctx_status.
 * (Kept in sync with per-conn c->last_status; the ctx copy is what io_status reads.) */
static EFI_STATUS g_last_ctx_status = EFI_SUCCESS;

static KlIoStatus efi_sock_io_status(void *cx) {
    (void)cx;
    return kl_efi_status_to_io(g_last_ctx_status);
}

/* No-op setup ops: EFI has no fd flags (the provider is inherently non-blocking via
 * tokens). All return success. */
static int  efi_noop_fd(void *cx, KlSocketHandle fd)          { (void)cx; (void)fd; return 0; }
static void efi_void_fd(void *cx, KlSocketHandle fd)          { (void)cx; (void)fd; }
static int  efi_opt(void *cx, KlSocketHandle fd, int on)      { (void)cx; (void)fd; (void)on; return 0; }

/* bind / listen / accept: client-only provider (like lwip-raw's client path).
 * bind → -1 (ENOTSUP-equivalent, no errno on UEFI); listen/accept unsupported. */
static int efi_sock_bind(void *cx, KlSocketHandle fd, const KlSockAddr *a) {
    (void)cx; (void)fd; (void)a; g_last_ctx_status = EFI_UNSUPPORTED; return -1;
}
static int efi_sock_listen(void *cx, KlSocketHandle fd, int backlog) {
    (void)cx; (void)fd; (void)backlog; g_last_ctx_status = EFI_UNSUPPORTED; return -1;
}
static KlSocketHandle efi_sock_accept(void *cx, KlSocketHandle fd, KlSockAddr *peer) {
    (void)cx; (void)fd; (void)peer; g_last_ctx_status = EFI_UNSUPPORTED; return KL_INVALID_SOCKET;
}
static int efi_sock_get_so_error(void *cx, KlSocketHandle fd, int *out_err) {
    (void)cx; (void)fd; if (out_err) *out_err = 0; return 0;
}

/* ── ops wrappers that also stash the ctx-level last status ───────────────────────
 * The vtable entries below wrap the conn-op so io_status() (which only gets the ctx)
 * reads the right status. Thin — they just mirror c->last_status into the ctx global. */
static KlSocketHandle w_socket(void *cx, int d, int t, int p) {
    KlSocketHandle h = efi_sock_socket(cx, d, t, p);
    KlUefiConn *c = conn_of(h);
    g_last_ctx_status = c ? c->last_status : EFI_OUT_OF_RESOURCES;
    return h;
}
static int w_connect(void *cx, KlSocketHandle fd, const KlSockAddr *a) {
    int r = efi_sock_connect(cx, fd, a);
    KlUefiConn *c = conn_of(fd);
    if (c) g_last_ctx_status = c->last_status;
    return r;
}
static kl_ssize_t w_send(void *cx, KlSocketHandle fd, const void *b, size_t n) {
    kl_ssize_t r = efi_sock_send(cx, fd, b, n);
    KlUefiConn *c = conn_of(fd);
    if (c) g_last_ctx_status = c->last_status;
    return r;
}
static kl_ssize_t w_recv(void *cx, KlSocketHandle fd, void *b, size_t n) {
    kl_ssize_t r = efi_sock_recv(cx, fd, b, n);
    KlUefiConn *c = conn_of(fd);
    if (c) g_last_ctx_status = c->last_status;
    return r;
}
static int w_get_local_addr(void *cx, KlSocketHandle fd, KlSockAddr *addr) {
    int r = efi_sock_get_local_addr(cx, fd, addr);
    KlUefiConn *c = conn_of(fd);
    if (c) g_last_ctx_status = c->last_status;
    return r;
}
static kl_ssize_t w_recv_peek(void *cx, KlSocketHandle fd, void *b, size_t n) {
    (void)cx; (void)fd; (void)b; (void)n;
    g_last_ctx_status = EFI_UNSUPPORTED;   /* peek unused by the async client */
    return -1;
}

/* ── the ops table + provider ─────────────────────────────────────────────────── */
static const KlSocketOps efi_tcp4_ops = {
    .set_nonblocking = efi_noop_fd, .set_blocking = efi_noop_fd,
    .set_cloexec = efi_void_fd,     .set_nosigpipe = efi_void_fd,
    .set_reuseaddr = efi_opt, .set_reuseport = efi_opt,
    .set_ipv6only = efi_opt,  .set_tcp_nodelay = efi_opt,
    .set_cork = efi_opt,
    .socket = w_socket, .connect = w_connect, .bind = efi_sock_bind,
    .listen = efi_sock_listen, .accept = efi_sock_accept, .close = efi_sock_close,
    .get_local_addr = w_get_local_addr, .get_so_error = efi_sock_get_so_error,
    .send = w_send, .recv = w_recv, .recv_peek = w_recv_peek,
    .writev = NULL, .sendfile = NULL,
    .io_status = efi_sock_io_status,
    .destroy = NULL,
    .name = "efi-tcp4",
};

static KlSocketProvider g_provider;

const KlSocketProvider *kl_uefi_socket_provider(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    if (g_ctx.created) return &g_provider;   /* single-instance */
    if (!bs) return NULL;

    /* zero the ctx (no libc memset guaranteed) */
    { unsigned char *p = (unsigned char *)&g_ctx; for (size_t i = 0; i < sizeof(g_ctx); i++) p[i] = 0; }
    g_ctx.bs    = bs;
    g_ctx.image = image;
    g_ctx.alloc = kl_uefi_allocator(bs);

    UINTN n = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS st = bs->LocateHandleBuffer(ByProtocol, &g_tcp4_sb_guid, NULL, &n, &handles);
    if (EFI_ERROR(st) || n == 0 || !handles) {
        if (handles) bs->FreePool(handles);
        return NULL;   /* this OVMF build has no TCP4 stack */
    }

    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    st = bs->HandleProtocol(handles[0], &g_tcp4_sb_guid, (VOID **)&sb);
    if (EFI_ERROR(st) || !sb) {
        bs->FreePool(handles);
        return NULL;
    }

    g_ctx.sb         = sb;
    g_ctx.sb_handles = handles;   /* freed in reset() */
    g_ctx.created    = 1;

    g_provider.ops          = &efi_tcp4_ops;
    g_provider.context      = &g_ctx;
    g_provider.capabilities = KL_SOCK_CAP_OVERLAPPED;
    g_provider.dgram        = NULL;
    return &g_provider;
}

void kl_uefi_socket_provider_reset(void) {
    if (!g_ctx.created) return;
    if (g_ctx.sb_handles && g_ctx.bs) g_ctx.bs->FreePool(g_ctx.sb_handles);
    { unsigned char *p = (unsigned char *)&g_ctx; for (size_t i = 0; i < sizeof(g_ctx); i++) p[i] = 0; }
    g_last_ctx_status = EFI_SUCCESS;
}
