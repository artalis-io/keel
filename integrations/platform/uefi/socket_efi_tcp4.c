/*
 * socket_efi_tcp4.c — KlSocketProvider over EFI_TCP4.
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
/* KEEL_UEFI_DATAGRAM (optional transport): only then does this become the UNIFIED
 * stream+datagram provider (SOCK_DGRAM → EFI_UDP4). A TCP-only build compiles a pure EFI_TCP4
 * stream provider — no socket_efi_udp4 dependency, .dgram = NULL, no DATAGRAM cap. */
#ifdef KEEL_UEFI_DATAGRAM
#include "socket_efi_udp4.h"          /* unified stream+datagram provider (tag-dispatched) */
#endif
#include "allocator_uefi.h"           /* kl_uefi_allocator */
#include "platform_uefi.h"            /* kl_uefi_after_ebs (boot-services guard) */
#include "../../../src/socket.h"         /* KL_SOCK_CAP_OVERLAPPED / KL_SOCK_CAP_DATAGRAM */

/* ── errno (WRITE-only) ──────────────────────────────────────────────────────
 * recv is non-blocking; the mbedTLS BIO (tls_mbedtls.c) detects a
 * would-block by reading errno==EAGAIN, so the provider MUST set it on the
 * would-block/error -1 paths (matching the lwip provider contract). We only
 * WRITE errno here; the definition lives in u1_link_stubs.c (linked by every
 * build). No libc errno.h — the freestanding shim may lack it —
 * so declare the global + the two codes locally. */
extern int errno;
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EIO
#define EIO 5
#endif

/* Provider-owned receive buffer: big enough to usually hold a whole TCP segment
 * so a single posted Receive satisfies most recv() calls. */
#ifndef KL_EFI_RXBUF
#define KL_EFI_RXBUF 8192
#endif
#ifndef KL_EFI_TXBUF
/* Slot-owned transmit staging. A Transmit token references its FragmentBuffer until
 * the token completes; if a Transmit times out and cannot be cancel-drained it is
 * QUARANTINED, and its storage must stay valid indefinitely — so the send descriptor +
 * data live in the STABLE slot, never on the stack or in the caller's buffer. */
#define KL_EFI_TXBUF 8192
#endif

/* ── EFI_TCP4_PROTOCOL_GUID / ServiceBinding GUID (file-scope, mutable per the
 * EFI ABI which takes EFI_GUID* non-const) ─────────────────────────────────────── */
static EFI_GUID g_tcp4_sb_guid = EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID g_tcp4_guid    = EFI_TCP4_PROTOCOL_GUID;

/* Token completion events are bare (type 0): EVT_NOTIFY_WAIT/SIGNAL REQUIRE a
 * non-NULL NotifyFunction (UEFI 2.10 §7.1.1) — passing NULL there returns
 * EFI_INVALID_PARAMETER. Type 0 is the correct choice for CheckEvent-polled tokens. */
#define EFI_TCP4_EVT_TOKEN 0

/* Bounded per-op pump. Each spin does Poll()+CheckEvent()+Stall(1ms). The overall
 * request deadline is owned above the seam (KlHttpClientConfig.timeout_ms); this bound
 * only stops a wedged single op from spinning forever. */
#ifndef KL_EFI_PUMP_SPINS
#define KL_EFI_PUMP_SPINS 60000   /* ~60 s worst case at 1 ms/spin */
#endif
#define KL_EFI_PUMP_STALL_US 1000

/* After a pump timeout leaves a token outstanding, Cancel() it and drain its
 * (now EFI_ABORTED) completion so no firmware token still references our storage.
 * Bounded so even a wedged cancel cannot spin forever. */
#ifndef KL_EFI_CANCEL_DRAIN_SPINS
#define KL_EFI_CANCEL_DRAIN_SPINS 2000   /* ~2 s worst case at 1 ms/spin */
#endif

/* Fixed slot pool. KlUefiConn slots are provider-owned static storage — a closed
 * slot's memory survives, so kl_uefi_conn_valid_h() reads STABLE storage (no UAF). The
 * client uses 1–2 concurrent conns; 8 is generous. Bump this #define for a server. */
#ifndef KL_EFI_MAX_CONNS
#define KL_EFI_MAX_CONNS 8
#endif

/* Configure()/DHCP settle: retry EFI_NO_MAPPING while the default address binds
 * Bounded so a network-less run fails rather than hangs. */
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

    /* ── Non-blocking receive state ──────────────────────────────────────
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

    /* ── explicit token-state (a token is "posted" only after a successful submit and
     * is cleared only after its terminal event is OBSERVED). close() drains ONLY the
     * tokens still flagged — never a token whose signal was already consumed (which
     * would spin ~forever on an event that never fires again). ─────────────────── */
    int                       conn_posted;  /* a Connect token is outstanding */
    int                       tx_posted;    /* a Transmit token is outstanding */
    int                       close_posted; /* a Close token is outstanding */
    /* Slot-owned Transmit staging (stable across a failed cancel-drain / quarantine). */
    EFI_TCP4_TRANSMIT_DATA    tx_data;
    unsigned char             tx_buf[KL_EFI_TXBUF];

    /* Set when a submitted token could NOT be confirmed terminal (cancel-drain failed):
     * the firmware may still reference this slot's tokens/buffers forever, so the slot
     * is QUARANTINED — never CloseEvent'd, never DestroyChild'd, never reclaimed by
     * socket(). Its stable storage is deliberately leaked until ExitBootServices. */
    int                       quarantined;

    /* ── Server (passive) state ──────────────────────────────────────────
     * A listener conn is Configure()d with ActiveFlag=FALSE; bind() records the
     * station port here, listen() arms the Accept-token pool (g_listener). An
     * accepted child is an ordinary KlUefiConn (adopted from the Accept token's
     * NewChildHandle) that reuses send/recv/close verbatim. */
    int                       is_listener;   /* this conn is a passive listener */
    UINT16                    station_port;   /* bind() port (0 = any) */

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

/* Provider-owned fixed slot pool. A KlUefiConn lives in stable static storage for
 * the whole provider lifetime; socket() claims a free slot (magic==0), close() marks it
 * free (magic=0, generation bumped even) but NEVER frees the storage. The KlSocketHandle
 * is (intptr_t)(slot_index + 1) — 0 stays KL_INVALID_SOCKET. Because the slot storage is
 * stable, kl_uefi_conn_valid_h() reading {magic, generation} after a close can never
 * dereference freed memory. Single-threaded pre-boot env → no locking. */
static KlUefiConn g_conns[KL_EFI_MAX_CONNS];

/* ── Listener: a single passive Accept-token pool (UEFI is single-threaded pre-boot,
 * so one listener / one loop; horizontal scaling is not a pre-boot concern). Each token
 * is armed via EFI_TCP4_PROTOCOL.Accept and, on completion, yields a NewChildHandle. The
 * pool storage is file-scope-stable so a firmware write into a hung Accept token after a
 * failed cancel lands in valid memory (the client's DNS-token lesson); a token that can't
 * be confirmed retired is quarantined (its event never CloseEvent'd, leaked to EBS). */
#define KL_EFI_ACCEPT_BACKLOG 4
typedef struct {
    KlSocketHandle fd;                                  /* listener conn handle (0 = none) */
    EFI_TCP4_LISTEN_TOKEN tok[KL_EFI_ACCEPT_BACKLOG];
    int            posted[KL_EFI_ACCEPT_BACKLOG];       /* token armed, awaiting completion */
    int            quarantined[KL_EFI_ACCEPT_BACKLOG];  /* failed cancel — event leaked, never reused */
    int            n;                                   /* number of pool slots created */
} KlUefiListener;
static KlUefiListener g_listener;

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

/* Handle ⇄ slot. The handle is (slot_index + 1) so 0 == KL_INVALID_SOCKET. conn_of
 * bounds-checks the index and returns the slot only if it is LIVE (magic set, not dead)
 * — reading STABLE static storage, never freed memory. */
static KlUefiConn *conn_of(KlSocketHandle fd) {
    if (!kl_handle_valid(fd)) return NULL;
    intptr_t v = (intptr_t)fd;
    if (v < 1 || v > KL_EFI_MAX_CONNS) return NULL;
    KlUefiConn *c = &g_conns[v - 1];
    if (c->magic != KL_EFI_CONN_MAGIC || c->dead) return NULL;
    return c;
}

/* Handle for a slot index (0-based) → (index + 1) as a KlSocketHandle. */
static KlSocketHandle handle_of_slot(int idx) {
    return (KlSocketHandle)(intptr_t)(idx + 1);
}

/* Validate a {conn, generation} pair — the stale-completion guard the completion backend
 * uses. A conn that was closed (generation bumped even) or whose slot was re-claimed by a
 * NEW conn (different generation) is rejected. With the fixed slot pool the conn pointer is
 * ALWAYS a stable slot address, so this never dereferences freed memory. */
int kl_uefi_conn_valid(KlUefiConn *c, UINT64 generation);
int kl_uefi_conn_valid(KlUefiConn *c, UINT64 generation) {
    return c && c->magic == KL_EFI_CONN_MAGIC && c->generation == generation && !c->dead;
}

/* Like conn_of but returns the slot even if it is dead/reused (stable storage),
 * so the generation/magic check can run against it without a UAF. NULL only for an
 * out-of-range handle. */
static KlUefiConn *slot_of(KlSocketHandle fd) {
    if (!kl_handle_valid(fd)) return NULL;
    intptr_t v = (intptr_t)fd;
    if (v < 1 || v > KL_EFI_MAX_CONNS) return NULL;
    return &g_conns[v - 1];
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

/*
 * Pump the token's event to completion; on a pump timeout, Cancel() the token and
 * pump until its (now EFI_ABORTED) event is signaled, so no outstanding token still
 * references our stack/caller storage when we return.
 *
 * Returns 1 if the event is signaled (naturally OR post-cancel — the caller reads
 * ct->Status, which is EFI_ABORTED after a cancel), or 0 only if even the cancel-drain
 * could not confirm the event fired (last resort — the caller must then treat the conn
 * as unusable, since the firmware may still hold the token).
 *
 * The token-lifetime rule: every submitted token MUST reach ONE terminal state
 * (completed OR cancelled-and-drained) before its backing storage is released/reused.
 */
static int pump_or_cancel(KlUefiConn *c, EFI_TCP4_COMPLETION_TOKEN *ct) {
    if (pump_until(c, ct->Event)) return 1;   /* completed naturally */

    EFI_BOOT_SERVICES *bs = c->bs;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    /* Cancel(token) → the firmware aborts it and signals the event with EFI_ABORTED. */
    tcp->Cancel(tcp, ct);
    for (int spins = 0; spins < KL_EFI_CANCEL_DRAIN_SPINS; spins++) {
        tcp->Poll(tcp);
        if (bs->CheckEvent(ct->Event) == EFI_SUCCESS) return 1;   /* drained */
        bs->Stall(KL_EFI_PUMP_STALL_US);
    }
    return 0;   /* could not confirm — conn is unusable */
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
    /* Refuse new sockets once boot services are gone — EFI_TCP4/CreateChild are
     * invalid after ExitBootServices. The lifetime discipline is kl_uefi_shutdown()
     * BEFORE EBS; this is the fail-closed safety net if that was skipped. */
    if (kl_uefi_after_ebs()) return KL_INVALID_SOCKET;
    /* IPv4 + STREAM only. The seam passes the POSIX numeric domain (AF_INET=2) and
     * type (SOCK_STREAM=1); reject anything but a stream socket. Family enforcement
     * on the address happens at connect() (the neutral KlSockAddr carries it). */
    if (type != SOCK_STREAM) return KL_INVALID_SOCKET;
    (void)domain;

    /* Find a free slot (magic == 0). No per-conn kl_malloc — the pool is static. */
    int idx = -1;
    for (int i = 0; i < KL_EFI_MAX_CONNS; i++)
        if (g_conns[i].magic == 0) { idx = i; break; }
    if (idx < 0) return KL_INVALID_SOCKET;   /* pool exhausted */
    KlUefiConn *c = &g_conns[idx];

    /* Preserve the (even, post-close) generation across re-use so a NEW conn in this
     * slot gets a DIFFERENT (bumped) generation — stale completions for the prior
     * tenant are rejected. First use: generation is 0 → bump to 1 (odd == live). */
    UINT64 prev_gen = c->generation;
    /* zero the slot (no libc memset guaranteed in this TU — do it by hand). */
    { unsigned char *p = (unsigned char *)c; for (size_t i = 0; i < sizeof(*c); i++) p[i] = 0; }
    c->bs    = ctx->bs;
    c->image = ctx->image;
    c->sb    = ctx->sb;
    c->alloc = ctx->alloc;
    c->generation = prev_gen + 1;   /* odd == live (even+1 or 0+1) */

    EFI_STATUS st = ctx->sb->CreateChild(ctx->sb, &c->child);
    if (EFI_ERROR(st) || !c->child) {
        c->last_status = st;
        c->generation++;            /* back to even == free */
        c->magic = 0;               /* release the slot */
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
        c->generation++;
        c->magic = 0;
        return KL_INVALID_SOCKET;
    }

    if (create_events(c) != 0) {
        ctx->bs->CloseProtocol(c->child, &g_tcp4_guid, ctx->image, c->child);
        ctx->sb->DestroyChild(ctx->sb, c->child);
        c->generation++;
        c->magic = 0;
        return KL_INVALID_SOCKET;
    }

    c->magic = KL_EFI_CONN_MAGIC;
    c->last_status = EFI_SUCCESS;
    return handle_of_slot(idx);
}

/*
 * connect(): the SYNC socket op. Per the split (see the header), this does the
 * Configure ONLY — active mode, UseDefaultAddress (DHCP), remote addr/port from the
 * KlSockAddr — tolerating EFI_NO_MAPPING (retry while DHCP settles). It does
 * NOT issue the Connect token; the actual async connect (Connect + pump) is the
 * completion post_connect (or kl_uefi_socket_connect_now() for the selftest).
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
    if (kl_uefi_after_ebs()) return -1;   /* exposed entry — guard EBS itself */
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
        if (st == EFI_NO_MAPPING) {      /* DHCP not settled — pump + retry */
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
    if (kl_uefi_after_ebs()) return -1;   /* fail-closed post-EBS */
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
    if (kl_uefi_after_ebs()) return -1;   /* exposed entry — guard EBS itself */
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead || !c->configured || c->quarantined) return -1;
    if (c->connected) return 0;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    c->conn_tok.CompletionToken.Status = EFI_NOT_READY;
    EFI_STATUS st = tcp->Connect(tcp, &c->conn_tok);
    if (EFI_ERROR(st)) { c->last_status = st; return -1; }
    c->conn_posted = 1;   /* Connect token now outstanding — close() must reconcile it */
    return 0;
}

/* connect_poll — pump once + test the Connect token. 1 = terminal (*out_ok set),
 * 0 = still pending, -1 = invalid handle. The async counterpart of the pump loop
 * inside kl_uefi_socket_connect_now. */
int kl_uefi_socket_connect_poll(KlSocketHandle fd, int *out_ok) {
    if (kl_uefi_after_ebs()) return -1;   /* fail-closed post-EBS */
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return -1;
    if (c->connected) { if (out_ok) *out_ok = 1; return 1; }
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    tcp->Poll(tcp);
    if (c->bs->CheckEvent(c->conn_tok.CompletionToken.Event) != EFI_SUCCESS)
        return 0;   /* not signaled yet */
    c->conn_posted = 0;   /* terminal observed — close() must NOT re-drain it */
    EFI_STATUS st = c->conn_tok.CompletionToken.Status;
    c->last_status = st;
    int ok = !EFI_ERROR(st);
    if (ok) c->connected = 1;
    if (out_ok) *out_ok = ok;
    return 1;
}

/* Handle-based generation helpers for the completion backend's stale guard. */
unsigned long long kl_uefi_conn_generation_h(KlSocketHandle fd) {
    KlUefiConn *c = conn_of(fd);   /* live read: captured at post time */
    return c ? (unsigned long long)c->generation : 0ULL;
}
int kl_uefi_conn_valid_h(KlSocketHandle fd, unsigned long long generation) {
    /* Read the STABLE slot (slot_of, not conn_of) so a completion delivered after
     * the conn was closed + freed cannot deref freed memory — the slot storage is
     * static. The magic/generation/dead check inside kl_uefi_conn_valid rejects it. */
    return kl_uefi_conn_valid(slot_of(fd), (UINT64)generation);
}

/*
 * kl_uefi_socket_connect_now — issue the Connect token + pump it to completion,
 * synchronously. This is what the completion post_connect does asynchronously;
 * exposed here so the selftest (which has no completion backend) can establish
 * the connection inline after efi_sock_connect() has done the Configure. Returns 0
 * on connected, -1 on failure (conn->last_status carries the EFI_STATUS).
 *
 * Precondition: connect() (Configure) has already run (c->configured).
 */
int kl_uefi_socket_connect_now(KlSocketHandle fd);
int kl_uefi_socket_connect_now(KlSocketHandle fd) {
    if (kl_uefi_after_ebs()) return -1;   /* fail-closed post-EBS */
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead || !c->configured) return -1;
    if (c->connected) return 0;

    if (c->quarantined) return -1;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    c->conn_tok.CompletionToken.Status = EFI_NOT_READY;
    EFI_STATUS st = tcp->Connect(tcp, &c->conn_tok);
    if (EFI_ERROR(st)) { c->last_status = st; return -1; }
    c->conn_posted = 1;
    /* On a pump timeout, Cancel+drain the Connect token so no outstanding token
     * references c->conn_tok after we return -1 (the failure path). */
    if (!pump_or_cancel(c, &c->conn_tok.CompletionToken)) {
        c->last_status = EFI_TIMEOUT;
        c->quarantined = 1;   /* couldn't retire the token — never free/reuse this slot */
        c->dead = 1;
        return -1;
    }
    c->conn_posted = 0;                        /* terminal observed */
    st = c->conn_tok.CompletionToken.Status;   /* EFI_ABORTED after a cancel */
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
    /* Fail-closed once boot services are gone (EFI_TCP4 is invalid post-EBS). */
    if (kl_uefi_after_ebs()) { errno = EIO; return -1; }
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) { errno = EIO; return -1; }
    if (!c->connected) { c->last_status = EFI_NOT_READY; errno = EAGAIN; return -1; }  /* connect first */
    if (c->quarantined) { c->last_status = EFI_ABORTED; errno = EIO; return -1; }
    if (len == 0) { c->last_status = EFI_SUCCESS; return 0; }
    /* Copy into the SLOT-OWNED tx buffer so the Transmit token never references the
     * caller's buffer (which it may reuse) — and, on a failed cancel-drain, the token's
     * storage stays valid in the quarantined slot. Bound to one KL_EFI_TXBUF fragment;
     * the caller loops for the remainder (partial write). */
    if (len > KL_EFI_TXBUF) len = KL_EFI_TXBUF;
    for (size_t i = 0; i < len; i++) c->tx_buf[i] = ((const unsigned char *)buf)[i];

    EFI_TCP4_TRANSMIT_DATA *tx = &c->tx_data;
    { unsigned char *p = (unsigned char *)tx; for (size_t i = 0; i < sizeof(*tx); i++) p[i] = 0; }
    tx->Push = TRUE;
    tx->DataLength = (UINT32)len;
    tx->FragmentCount = 1;
    tx->FragmentTable[0].FragmentLength = (UINT32)len;
    tx->FragmentTable[0].FragmentBuffer = c->tx_buf;

    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    c->tx_tok.Packet.TxData = tx;
    c->tx_tok.CompletionToken.Status = EFI_NOT_READY;

    EFI_STATUS st = tcp->Transmit(tcp, &c->tx_tok);
    if (EFI_ERROR(st)) { c->last_status = st; errno = send_errno(st); return -1; }
    c->tx_posted = 1;
    /* On a pump timeout, Cancel+drain the tx token so the firmware no longer
     * references the slot's tx_data/tx_buf. */
    if (!pump_or_cancel(c, &c->tx_tok.CompletionToken)) {
        /* Cancel-drain could NOT confirm the token retired: the firmware may still own
         * tx_data/tx_buf. QUARANTINE — never free/reuse this slot; its storage is
         * retained until EBS. (A bounded wait is fine; freeing afterward is not.) */
        c->last_status = EFI_TIMEOUT;
        c->quarantined = 1;
        c->dead = 1;
        errno = EIO;
        return -1;
    }
    c->tx_posted = 0;                         /* terminal observed */
    st = c->tx_tok.CompletionToken.Status;    /* EFI_ABORTED after a cancel */
    c->last_status = st;
    if (EFI_ERROR(st)) { errno = send_errno(st); return -1; }
    return (kl_ssize_t)len;
}

/*
 * kl_uefi_socket_recv_ready — the non-blocking READ-readiness probe the
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
    if (kl_uefi_after_ebs()) return 0;   /* fail-closed post-EBS (no readiness) */
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
    /* The firmware-reported DataLength is UNTRUSTED — a broken/hostile provider
     * could report a length exceeding the buffer we posted, and rx_consume() would then
     * read past rx_buf. Any impossible length is a fatal protocol/provider error. */
    if (c->rx_data.DataLength > (UINT32)KL_EFI_RXBUF ||
        c->rx_data.FragmentTable[0].FragmentLength > (UINT32)KL_EFI_RXBUF) {
        c->rx_err = EFI_DEVICE_ERROR;
        c->last_status = EFI_DEVICE_ERROR;
        return 1;
    }
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
 * recv(): NON-BLOCKING. Serves buffered data first; otherwise probes
 * readiness ONCE (kl_uefi_socket_recv_ready, which posts + polls a Receive
 * without pumping). Returns bytes (>0), 0 on FIN (EOF), or -1 would-block/error.
 * On a would-block -1 sets errno=EAGAIN (mbedTLS BIO would-block detection); on a
 * fatal error -1 sets errno=EIO. NO internal pump loop — the drain drives it.
 */
static kl_ssize_t efi_sock_recv(void *cx, KlSocketHandle fd, void *buf, size_t len) {
    (void)cx;
    /* Fail-closed once boot services are gone. */
    if (kl_uefi_after_ebs()) { errno = EIO; return -1; }
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
 * close(): teardown with token-lifetime reconciliation + fail-closed post-EBS.
 *
 * BEFORE any teardown, reconcile every token that may still be outstanding so the
 * firmware cannot later write freed/reused storage:
 *   Cancel(NULL) cancels ALL pending tokens at once → then DRAIN each token that may be
 *   live by pumping its event to signaled: the Receive (rx_posted), a pending
 *   Connect (configured && !connected). The graceful Close token itself rides the
 *   timeout guard (pump_or_cancel), not a bare pump — the ignored close-token timeout
 *   would otherwise leak.
 * Order: Cancel(NULL) → drain rx/connect → graceful Close → pump_or_cancel(close) →
 *        close_events → Configure(NULL) → CloseProtocol → DestroyChild → free slot.
 *
 * After ExitBootServices the EFI protocols are GONE — do NOT call
 * Cancel/Close/CloseEvent/CloseProtocol/DestroyChild; just mark the slot dead+free.
 *
 * The KlUefiConn lives in a static slot — close() marks it free (magic=0,
 * generation bumped even) but NEVER frees the storage, so a late completion's
 * {handle,gen} read is safe (stable storage).
 */
/* Tear down the Accept-token pool when its listener closes: cancel outstanding
 * Accept tokens, drain each to a terminal signal, then CloseEvent it — quarantining
 * (leaking) any that can't be confirmed retired (the firmware may still write the
 * token / NewChildHandle). Mirrors efi_sock_close's per-conn token discipline. */
static int listener_teardown(KlUefiConn *lc) {
    EFI_TCP4_PROTOCOL *tcp = lc->tcp;
    int drained_ok = 1;
    if (tcp) tcp->Cancel(tcp, NULL);   /* abort all outstanding Accept tokens */
    for (int i = 0; i < g_listener.n; i++) {
        if (g_listener.quarantined[i]) { drained_ok = 0; continue; }   /* already leaked */
        if (g_listener.posted[i]) {
            if (!pump_until(lc, g_listener.tok[i].CompletionToken.Event)) {
                g_listener.quarantined[i] = 1;      /* hung — leak the event, never close */
                drained_ok = 0;
                continue;
            }
            g_listener.posted[i] = 0;
        }
        if (g_listener.tok[i].CompletionToken.Event) {
            lc->bs->CloseEvent(g_listener.tok[i].CompletionToken.Event);
            g_listener.tok[i].CompletionToken.Event = NULL;
        }
    }
    g_listener.fd = 0;
    g_listener.n  = 0;
    /* If any Accept token couldn't be confirmed retired, the firmware may still write
     * it via the listener's tcp — the CHILD must not be destroyed (caller quarantines). */
    return drained_ok;
}

static int efi_sock_close(void *cx, KlSocketHandle fd) {
    (void)cx;
#ifdef KEEL_UEFI_DATAGRAM
    if (kl_efi_is_udp_handle(fd)) return kl_uefi_udp_close(fd);   /* unified: datagram close */
#endif
    KlUefiConn *c = conn_of(fd);
    if (!c) return 0;               /* already invalid — nothing to do */
    if (c->dead) return 0;          /* idempotent */
    c->dead = 1;
    c->generation++;                /* even == closed: reject any late {conn,gen} */

    /* Post-EBS, the EFI protocols vanished — touch NO boot service. Just free
     * the slot (magic=0) and return; conn_of() then rejects the handle. */
    if (kl_uefi_after_ebs()) {
        c->magic = 0;
        return 0;
    }

    /* A quarantined conn already leaked its (still-firmware-owned) tokens/buffers.
     * Do NOT touch them again; leave the slot occupied-but-dead (never reclaimed). */
    if (c->quarantined) return 0;

    /* A listener also owns the Accept-token pool — drain/quarantine it before the
     * conn's own teardown. If any Accept token couldn't be retired, the firmware may
     * still write it via this listener's tcp: QUARANTINE the whole conn (never
     * CloseEvent/Configure(NULL)/DestroyChild) — its stable storage is leaked to EBS. */
    if (c->is_listener && g_listener.fd == fd) {
        if (!listener_teardown(c)) { c->quarantined = 1; return 0; }
    }

    EFI_BOOT_SERVICES *bs = c->bs;
    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    int drained_ok = 1;   /* every posted token confirmed retired? */

    /* Cancel ALL pending tokens at once, then drain ONLY the tokens that are
     * actually posted (explicit *_posted flags — NEVER a token whose terminal signal was
     * already consumed, which would spin forever on an event that never fires again).
     * Check EACH drain: if one cannot be confirmed retired, the token is still live. */
    if (tcp) tcp->Cancel(tcp, NULL);

    if (tcp && c->rx_posted) {
        if (!pump_until(c, c->rx_tok.CompletionToken.Event)) drained_ok = 0;
        c->rx_posted = 0;
    }
    if (tcp && c->conn_posted) {
        if (!pump_until(c, c->conn_tok.CompletionToken.Event)) drained_ok = 0;
        c->conn_posted = 0;
    }
    if (tcp && c->tx_posted) {   /* normally cleared by send; belt-and-suspenders */
        if (!pump_until(c, c->tx_tok.CompletionToken.Event)) drained_ok = 0;
        c->tx_posted = 0;
    }

    /* Graceful Close — its token also needs a terminal path (the pump_or_cancel guard). */
    if (tcp && c->events_created && c->close_tok.CompletionToken.Event) {
        c->close_tok.AbortOnClose = FALSE;
        c->close_tok.CompletionToken.Status = EFI_NOT_READY;
        EFI_STATUS st = tcp->Close(tcp, &c->close_tok);
        if (!EFI_ERROR(st)) {
            c->close_posted = 1;
            if (!pump_or_cancel(c, &c->close_tok.CompletionToken)) drained_ok = 0;
            c->close_posted = 0;
        }
    }

    /* If ANY token could not be confirmed retired, the firmware may still own this
     * slot's tokens / buffers / events — QUARANTINE: do NOT CloseEvent, Configure(NULL),
     * CloseProtocol, DestroyChild, or reclaim the slot. Its stable storage is deliberately
     * leaked until ExitBootServices. (A bounded wait is fine; freeing afterward is not.) */
    if (!drained_ok) { c->quarantined = 1; return 0; }

    close_events(c);                /* CloseEvent all 4 (exactly once) */
    if (tcp) tcp->Configure(tcp, NULL);   /* reset config */
    if (c->child) {
        bs->CloseProtocol(c->child, &g_tcp4_guid, c->image, c->child);
        if (c->sb) c->sb->DestroyChild(c->sb, c->child);
    }
    c->magic = 0;   /* slot free + reclaimable (all tokens confirmed retired) */
    return 0;
}

/* get_local_addr(): GetModeData → the DHCP-assigned station address → KlSockAddr. */
static int efi_sock_get_local_addr(void *cx, KlSocketHandle fd, KlSockAddr *addr) {
    (void)cx;
    if (kl_uefi_after_ebs()) return -1;   /* fail-closed post-EBS */
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

/* The unified provider's io_status is provider-global (KlSocketOps.io_status has no fd), so the
 * DATAGRAM side must publish its last op's EFI_STATUS into the SAME channel the (stream) io_status
 * reads — otherwise a synchronous datagram failure (TOS/source-pin reject → EFI_UNSUPPORTED) would be
 * classified from stale TCP state and udp.c could enqueue a packet it should have dropped. Called by
 * socket_efi_udp4.c's sync dgram->send. Single-threaded + sequential, so the last op wins. */
void kl_uefi_provider_set_last_status(EFI_STATUS st) { g_last_ctx_status = st; }

/* No-op setup ops: EFI has no fd flags (the provider is inherently non-blocking via
 * tokens). All return success. */
static int  efi_noop_fd(void *cx, KlSocketHandle fd)          { (void)cx; (void)fd; return 0; }
static void efi_void_fd(void *cx, KlSocketHandle fd)          { (void)cx; (void)fd; }
static int  efi_opt(void *cx, KlSocketHandle fd, int on)      { (void)cx; (void)fd; (void)on; return 0; }

/* ── Server: passive open (bind / listen / accept) ─────────────────────────── */

/* Adopt an Accept token's NewChildHandle into a fresh KlUefiConn slot: OpenProtocol
 * the child's EFI_TCP4, create its per-op token events, mark it connected/configured
 * (Accept already established + configured the child). Returns the conn (magic live)
 * or NULL on failure — in which case the child is destroyed so it isn't orphaned. */
static KlUefiConn *adopt_child(EFI_HANDLE child) {
    KlUefiSockCtx *ctx = &g_ctx;
    if (!ctx->created || !child) return NULL;
    int idx = -1;
    for (int i = 0; i < KL_EFI_MAX_CONNS; i++)
        if (g_conns[i].magic == 0) { idx = i; break; }
    if (idx < 0) {                                   /* pool full — destroy the child */
        ctx->sb->DestroyChild(ctx->sb, child);
        return NULL;
    }
    KlUefiConn *c = &g_conns[idx];
    UINT64 prev_gen = c->generation;
    { unsigned char *p = (unsigned char *)c; for (size_t i = 0; i < sizeof(*c); i++) p[i] = 0; }
    c->bs = ctx->bs; c->image = ctx->image; c->sb = ctx->sb; c->alloc = ctx->alloc;
    c->generation = prev_gen + 1;                    /* odd == live */
    c->child = child;

    EFI_STATUS st = ctx->bs->OpenProtocol(child, &g_tcp4_guid, (VOID **)&c->tcp,
                                          ctx->image, child, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st) || !c->tcp)
        st = ctx->bs->OpenProtocol(child, &g_tcp4_guid, (VOID **)&c->tcp,
                                   ctx->image, child, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(st) || !c->tcp) {
        c->last_status = st; c->generation++; c->magic = 0;
        ctx->sb->DestroyChild(ctx->sb, child);
        return NULL;
    }
    if (create_events(c) != 0) {
        ctx->bs->CloseProtocol(child, &g_tcp4_guid, ctx->image, child);
        c->generation++; c->magic = 0;
        ctx->sb->DestroyChild(ctx->sb, child);
        return NULL;
    }
    c->configured = 1;   /* Accept configured + connected the child */
    c->connected  = 1;
    c->magic = KL_EFI_CONN_MAGIC;
    c->last_status = EFI_SUCCESS;
    return c;
}

/* Arm (or re-arm) Accept token `i` on the listener. Returns 0 on success. On a
 * submit failure the token stays un-posted (accept() simply has one fewer in flight). */
static int listener_arm_token(KlUefiConn *lc, int i) {
    if (g_listener.quarantined[i]) return -1;        /* never touch a leaked token */
    g_listener.tok[i].CompletionToken.Status = EFI_NOT_READY;
    g_listener.tok[i].NewChildHandle = NULL;
    EFI_STATUS st = lc->tcp->Accept(lc->tcp, &g_listener.tok[i]);
    if (EFI_ERROR(st)) { lc->last_status = st; g_listener.posted[i] = 0; return -1; }
    g_listener.posted[i] = 1;
    return 0;
}

/* bind(): record the listen port on the conn (station address is UseDefaultAddress /
 * DHCP). The passive Configure happens in listen(). */
static int efi_sock_bind(void *cx, KlSocketHandle fd, const KlSockAddr *a) {
    (void)cx;
#ifdef KEEL_UEFI_DATAGRAM
    if (kl_efi_is_udp_handle(fd)) return kl_uefi_udp_bind(fd, a);   /* unified: datagram bind */
#endif
    if (kl_uefi_after_ebs()) return -1;
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return -1;
    UINT16 port = 0;
    if (a && kl_sockaddr_family(a) == KL_AF_INET) {
        EFI_IPv4_ADDRESS ip;
        if (kl_efi_sockaddr_to_ipv4(a, &ip, &port) != 0) {
            c->last_status = EFI_INVALID_PARAMETER;
            return -1;
        }
    }
    c->station_port = port;
    c->is_listener  = 1;
    c->last_status  = EFI_SUCCESS;
    return 0;
}

/* listen(): passive Configure (ActiveFlag=FALSE, StationPort) + prime the Accept-token
 * pool (backlog clamped to KL_EFI_ACCEPT_BACKLOG). */
static int efi_sock_listen(void *cx, KlSocketHandle fd, int backlog) {
    (void)cx;
    if (kl_uefi_after_ebs()) return -1;
    KlUefiConn *c = conn_of(fd);
    if (!c || c->dead) return -1;

    EFI_TCP4_CONFIG_DATA *cfg = &c->config;
    { unsigned char *p = (unsigned char *)cfg; for (size_t i = 0; i < sizeof(*cfg); i++) p[i] = 0; }
    cfg->TimeToLive = 64;
    cfg->AccessPoint.UseDefaultAddress = TRUE;
    cfg->AccessPoint.ActiveFlag        = FALSE;   /* passive open */
    cfg->AccessPoint.StationPort       = c->station_port;

    EFI_TCP4_PROTOCOL *tcp = c->tcp;
    EFI_STATUS st = EFI_NOT_READY;
    for (int i = 0; i < KL_EFI_DHCP_RETRIES; i++) {
        st = tcp->Configure(tcp, cfg);
        if (!EFI_ERROR(st)) { c->configured = 1; break; }
        if (st == EFI_NO_MAPPING) { tcp->Poll(tcp); c->bs->Stall(KL_EFI_DHCP_STALL_US); continue; }
        break;
    }
    if (!c->configured) { c->last_status = st; return -1; }
    c->is_listener = 1;   /* a socket that listened is a listener (bind optional) */

    /* Create the Accept-token pool events (backlog clamped to KL_EFI_ACCEPT_BACKLOG) but
     * do NOT arm them here. Arming is capacity-gated (backpressure): the event
     * layer calls kl_uefi_socket_accept_arm(fd, free_keel_slots) each completion tick, so
     * EFI never has more armed Accept tokens than Keel has free connection slots — a
     * connection Keel can't service waits in the TCP backlog, not in a firmware child. */
    int n = backlog;
    if (n < 1) n = 1;
    if (n > KL_EFI_ACCEPT_BACKLOG) n = KL_EFI_ACCEPT_BACKLOG;
    { unsigned char *p = (unsigned char *)&g_listener; for (size_t i = 0; i < sizeof(g_listener); i++) p[i] = 0; }
    g_listener.fd = fd;
    for (int i = 0; i < n; i++) {
        st = c->bs->CreateEvent(EFI_TCP4_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                                &g_listener.tok[i].CompletionToken.Event);
        if (EFI_ERROR(st)) { c->last_status = st; break; }
        g_listener.n = i + 1;   /* event created; posted[i] stays 0 until accept_arm */
    }
    if (g_listener.n == 0) { c->last_status = st; return -1; }
    return 0;
}

/* Capacity-gated Accept-token arming (backpressure, the "post_accept" half of the
 * accept lifecycle, symmetric with kl_uefi_socket_connect_post). Arm unposted,
 * non-quarantined token slots until the posted count reaches min(pool size, `want`),
 * where `want` is the number of FREE Keel connection slots the event layer passes in.
 * Never un-arms (an outstanding EFI token cannot be cheaply cancelled) — but because
 * `want` tracks free capacity and each completion is harvested (posted -> 0) as its slot
 * is consumed, the posted count stays <= free capacity. Returns the resulting posted
 * count. Idempotent; safe to call every tick. */
int kl_uefi_socket_accept_arm(KlSocketHandle fd, int want) {
    if (kl_uefi_after_ebs()) return 0;
    KlUefiConn *lc = conn_of(fd);
    if (!lc || lc->dead || g_listener.fd != fd) return 0;
    if (want < 0) want = 0;
    if (want > g_listener.n) want = g_listener.n;

    int posted = 0;
    for (int i = 0; i < g_listener.n; i++)
        if (g_listener.posted[i]) posted++;          /* armed OR completed-unharvested */
    for (int i = 0; i < g_listener.n && posted < want; i++) {
        if (g_listener.posted[i] || g_listener.quarantined[i]) continue;
        if (listener_arm_token(lc, i) == 0) posted++;
    }
    return posted;
}

/* accept(): non-blocking. Poll the listener, hand back the first completed Accept token's
 * child as a fresh KlUefiConn (peer via GetModeData). It does NOT re-arm the consumed token
 * — re-arming is capacity-gated and done explicitly by kl_uefi_socket_accept_arm (the event
 * layer arms only up to free Keel pool slots; see below). KL_INVALID_SOCKET + EFI_NOT_READY
 * (would-block) when none is ready. */
static KlSocketHandle efi_sock_accept(void *cx, KlSocketHandle fd, KlSockAddr *peer) {
    (void)cx;
    if (kl_uefi_after_ebs()) return KL_INVALID_SOCKET;
    KlUefiConn *lc = conn_of(fd);
    if (!lc || lc->dead || g_listener.fd != fd) return KL_INVALID_SOCKET;

    lc->tcp->Poll(lc->tcp);
    for (int i = 0; i < g_listener.n; i++) {
        if (!g_listener.posted[i]) continue;
        if (lc->bs->CheckEvent(g_listener.tok[i].CompletionToken.Event) != EFI_SUCCESS)
            continue;   /* still pending */
        g_listener.posted[i] = 0;   /* harvest only — NO re-arm here (capacity-gated
                                     * re-arm is kl_uefi_socket_accept_arm, called by the
                                     * event layer with the free-slot count: backpressure). */
        EFI_STATUS st = g_listener.tok[i].CompletionToken.Status;
        EFI_HANDLE child = g_listener.tok[i].NewChildHandle;
        if (EFI_ERROR(st) || !child) {
            lc->last_status = st;
            continue;   /* failed accept — slot is now unposted; accept_arm re-arms it */
        }
        KlUefiConn *nc = adopt_child(child);
        if (!nc) { lc->last_status = EFI_OUT_OF_RESOURCES; return KL_INVALID_SOCKET; }

        if (peer) {
            EFI_TCP4_CONFIG_DATA cd;
            { unsigned char *p = (unsigned char *)&cd; for (size_t k = 0; k < sizeof(cd); k++) p[k] = 0; }
            EFI_STATUS gm = nc->tcp->GetModeData(nc->tcp, NULL, &cd, NULL, NULL, NULL);
            if (!EFI_ERROR(gm))
                (void)kl_efi_ipv4_to_sockaddr(&cd.AccessPoint.RemoteAddress,
                                              cd.AccessPoint.RemotePort, peer);
        }
        lc->last_status = EFI_SUCCESS;
        return handle_of_slot((int)(nc - g_conns));
    }
    lc->last_status = EFI_NOT_READY;   /* would-block */
    return KL_INVALID_SOCKET;
}
static int efi_sock_get_so_error(void *cx, KlSocketHandle fd, int *out_err) {
    (void)cx; (void)fd; if (out_err) *out_err = 0; return 0;
}

/* ── ops wrappers that also stash the ctx-level last status ───────────────────────
 * The vtable entries below wrap the conn-op so io_status() (which only gets the ctx)
 * reads the right status. Thin — they just mirror c->last_status into the ctx global. */
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif
static KlSocketHandle w_socket(void *cx, int d, int t, int p) {
#ifdef KEEL_UEFI_DATAGRAM
    if (t == SOCK_DGRAM)                          /* unified provider: SOCK_DGRAM → EFI_UDP4 child */
        return kl_uefi_udp_socket(d, t, p);
#endif
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
#ifdef KEEL_UEFI_DATAGRAM
    if (kl_efi_is_udp_handle(fd)) return kl_uefi_udp_get_local_addr(fd, addr);   /* unified: datagram */
#endif
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

#ifdef KEEL_UEFI_DATAGRAM
    /* One unified provider serves SOCK_STREAM (EFI_TCP4) AND SOCK_DGRAM (EFI_UDP4) — a UEFI
     * client resolves (UDP) then connects (TCP) on one KlEventCtx.sockets. Add the datagram
     * data-plane iff an EFI_UDP4 ServiceBinding is present (tolerated absent — no DATAGRAM cap). */
    if (kl_uefi_udp_provider_init(bs, image) == 0) {
        g_provider.capabilities |= KL_SOCK_CAP_DATAGRAM;
        g_provider.dgram         = kl_uefi_udp_dgram_ops();
    }
#endif
    return &g_provider;
}

void kl_uefi_socket_provider_reset(void) {
    if (!g_ctx.created) return;
    if (g_ctx.sb_handles && g_ctx.bs) g_ctx.bs->FreePool(g_ctx.sb_handles);
    { unsigned char *p = (unsigned char *)&g_ctx; for (size_t i = 0; i < sizeof(g_ctx); i++) p[i] = 0; }
    /* DO NOT blanket-zero the pool. A QUARANTINED slot's tokens/buffers may still be
     * firmware-owned — zeroing it would let a later socket() reuse storage the firmware
     * writes (corruption). A still-LIVE slot (magic set, not dead) means the caller reset
     * without closing (a lifecycle-contract violation, see lifecycle_uefi.h). Preserve
     * both; only slots that are already free (magic == 0, i.e. never used or cleanly
     * closed) are reclaimable, and those are already zero. So we clear nothing here — a
     * subsequent provider reuses the free slots and skips the occupied ones. */
    g_last_ctx_status = EFI_SUCCESS;
#ifdef KEEL_UEFI_DATAGRAM
    kl_uefi_udp_provider_reset();   /* the unified provider owns the datagram side too */
#endif
}

/* How many slots are still LIVE (open, not closed) — the lifecycle contract is that
 * this is 0 before kl_uefi_shutdown()/EBS. kl_uefi_shutdown() checks it. Declared in
 * socket_efi_tcp4.h. */
int kl_uefi_socket_provider_live_count(void) {
    int n = 0;
    for (int i = 0; i < KL_EFI_MAX_CONNS; i++)
        if (g_conns[i].magic == KL_EFI_CONN_MAGIC && !g_conns[i].dead) n++;
    return n;
}
