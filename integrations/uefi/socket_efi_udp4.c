/*
 * socket_efi_udp4.c — datagram half of the unified EFI socket provider (6.4b).
 *
 * See socket_efi_udp4.h + docs/phase10_efi_udp4_provider_design.md (FROZEN). This TU owns
 * the EFI_UDP4 child/token lifecycle on a TAGGED, generation-guarded slot handle and the
 * completion-native Receive/Transmit token primitives event_efi.c drives. It mirrors
 * socket_efi_tcp4.c (magic/generation stale-guard, create-events-once, pump/cancel). This TU
 * now OWNS the EFI_UDP4 calls (Configure / Receive+RecycleSignal / Transmit / quarantine); the
 * pattern was seeded from the now-retired one-shot dns_uefi.c.
 *
 * No libc: memory ops are hand-rolled byte loops (no guaranteed memset in this TU); every
 * -1 is classified via kl_efi_status_to_io off the slot's last EFI_STATUS (no errno — the
 * datagram data-plane reports would-block/fatal through kl_sock_io_status).
 */

#include "socket_efi_udp4.h"
#include "socket_efi_tcp4.h"          /* kl_efi_status_to_io + kl_efi_sockaddr_to_ipv4 / _ipv4_to_sockaddr */
#include "platform_uefi.h"            /* kl_uefi_after_ebs (F3 boot-services guard) */

#include <keel/handle.h>
#include <keel/datagram.h>            /* struct KlDatagramSocketConfig (configure signature) */

/* ── constants (mirror the TCP TU; datagram-local names) ─────────────────────────── */
#define KL_EFI_UDP_EVT_TOKEN     0            /* bare CheckEvent-polled token event */
#ifndef KL_EFI_UDP_PUMP_SPINS
#define KL_EFI_UDP_PUMP_SPINS    60000        /* ~60 s at 1 ms/spin (close/cancel drain bound) */
#endif
#define KL_EFI_UDP_STALL_US      1000
#ifndef KL_EFI_UDP_CANCEL_DRAIN_SPINS
#define KL_EFI_UDP_CANCEL_DRAIN_SPINS 2000    /* ~2 s */
#endif
#ifndef KL_EFI_UDP_DHCP_RETRIES
#define KL_EFI_UDP_DHCP_RETRIES  40
#endif
#define KL_EFI_UDP_DHCP_STALL_US 500000
/* Stable per-slot transmit staging: the firmware references the Tx FragmentBuffer until the
 * Transmit completes, so the payload lives in the slot (never the stack/caller). DNS queries
 * are well under this; a larger completion/sync send is refused. */
#ifndef KL_EFI_UDP_TXBUF
#define KL_EFI_UDP_TXBUF 2048
#endif
/* Distinct magic from the TCP pool ("UDP4KL" tag) so a stream late-completion can never be
 * mistaken for a datagram slot even before the handle-tag check. */
#define KL_EFI_UDP_MAGIC 0x554450344b4c00edULL

static EFI_GUID g_udp4_sb_guid = EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID g_udp4_guid    = EFI_UDP4_PROTOCOL_GUID;

/* ── the slot ────────────────────────────────────────────────────────────────────── */
typedef struct KlUefiUdp {
    UINT64                magic;        /* KL_EFI_UDP_MAGIC while live; 0 == free slot */
    UINT64                generation;   /* odd == live, bumped even on close (stale guard) */

    EFI_BOOT_SERVICES    *bs;
    EFI_HANDLE            image;
    EFI_HANDLE            child;        /* CreateChild result */
    EFI_UDP4_PROTOCOL    *udp;          /* OpenProtocol(EFI_UDP4_PROTOCOL) */
    EFI_SERVICE_BINDING_PROTOCOL *sb;   /* borrowed from the provider ctx */

    EFI_UDP4_CONFIG_DATA  config;       /* pinned config (Configure) */
    int                   configured;
    int                   dead;         /* closed — no further I/O */
    int                   quarantined;  /* a token could not be confirmed retired → leaked to EBS */
    EFI_STATUS            last_status;

    /* Serial Receive: at most one Receive token outstanding. */
    EFI_UDP4_COMPLETION_TOKEN rx_tok;   /* Event created once in socket() */
    int                       rx_posted;

    /* Independent Transmit token + stable staging (descriptor/session/payload). */
    EFI_UDP4_COMPLETION_TOKEN tx_tok;
    EFI_UDP4_TRANSMIT_DATA    tx_desc;
    EFI_UDP4_SESSION_DATA     tx_sess;
    unsigned char             tx_buf[KL_EFI_UDP_TXBUF];
    int                       tx_posted;

    int                       events_created;  /* rx/tx token events exist */
} KlUefiUdp;

typedef struct {
    EFI_BOOT_SERVICES *bs;
    EFI_HANDLE         image;
    EFI_SERVICE_BINDING_PROTOCOL *sb;
    EFI_HANDLE        *sb_handles;      /* LocateHandleBuffer result (to FreePool) */
    int                created;
} KlUefiUdpCtx;

static KlUefiUdpCtx g_uctx;
static KlUefiUdp    g_udp_conns[KL_EFI_MAX_UDP];

/* ── tiny hand-rolled memory ops (no libc in this TU) ────────────────────────────── */
static void u_zero(void *p, size_t n) { unsigned char *b = p; for (size_t i = 0; i < n; i++) b[i] = 0; }

/* ── handle ⇄ slot ───────────────────────────────────────────────────────────────── */
static KlSocketHandle udp_handle_of_slot(int idx) {
    return (KlSocketHandle)(KL_EFI_UDP_HANDLE_TAG | (intptr_t)(idx + 1));
}
/* LIVE slot for @fd (UDP-tagged, in range, magic set, not dead), else NULL. */
static KlUefiUdp *udp_of(KlSocketHandle fd) {
    if (!kl_handle_valid(fd)) return NULL;
    intptr_t v = (intptr_t)fd;
    if (!(v & KL_EFI_UDP_HANDLE_TAG)) return NULL;   /* not a datagram handle */
    intptr_t idx1 = v & ~KL_EFI_UDP_HANDLE_TAG;
    if (idx1 < 1 || idx1 > KL_EFI_MAX_UDP) return NULL;
    KlUefiUdp *u = &g_udp_conns[idx1 - 1];
    if (u->magic != KL_EFI_UDP_MAGIC || u->dead) return NULL;
    return u;
}
/* STABLE slot for @fd even if dead/reused (static storage) — for the generation guard. */
static KlUefiUdp *udp_slot_of(KlSocketHandle fd) {
    if (!kl_handle_valid(fd)) return NULL;
    intptr_t v = (intptr_t)fd;
    if (!(v & KL_EFI_UDP_HANDLE_TAG)) return NULL;
    intptr_t idx1 = v & ~KL_EFI_UDP_HANDLE_TAG;
    if (idx1 < 1 || idx1 > KL_EFI_MAX_UDP) return NULL;
    return &g_udp_conns[idx1 - 1];
}

/* Resolve the EXACT posted operation via STABLE storage + the captured generation — the
 * completion-op identity (review-High). Returns the slot IFF it is still the live op that was
 * posted (magic set, not dead, generation matches); NULL if the op is STALE (closed / reused /
 * quarantined) OR @fd is not a datagram handle. A stale op is NEVER resolved through udp_of(),
 * so a poll can never touch a reused slot's token. @stale (optional) distinguishes "not a udp
 * handle" (0) from "udp handle but stale generation" (1). */
static KlUefiUdp *udp_op_of(KlSocketHandle fd, unsigned long long gen, int *stale) {
    if (stale) *stale = 0;
    KlUefiUdp *u = udp_slot_of(fd);
    if (!u) return NULL;                         /* not a datagram handle */
    if (u->magic != KL_EFI_UDP_MAGIC || u->dead || u->generation != (UINT64)gen) {
        if (stale) *stale = 1;                   /* valid udp handle, but the op is gone */
        return NULL;
    }
    return u;
}

/* Classify a STALE op {fd,@gen} (generation no longer matches / slot dead): was its slot cleanly
 * closed/reused (reaped at close → release life) or QUARANTINED (leaked → retain life forever)? A
 * quarantined slot is never reused; its one quarantined op captured gen = slot->generation - 1 (close
 * bumped the generation exactly once). Any other gen on a quarantined slot is an earlier cleanly-retired
 * op (or bogus) → STALE_RETIRED. Reads STABLE storage (no UAF). */
static KlUefiUdpOpResult udp_stale_class(KlSocketHandle fd, unsigned long long gen) {
    KlUefiUdp *slot = udp_slot_of(fd);
    if (!slot) return KL_UEFI_UDP_OP_INVALID;   /* not a datagram handle */
    if (slot->quarantined && (UINT64)gen == slot->generation - 1)
        return KL_UEFI_UDP_OP_QUARANTINED;      /* this op was quarantined at close — retain life */
    return KL_UEFI_UDP_OP_STALE_RETIRED;        /* cleanly retired at close (or reused) — release life */
}

/* Side-effect-free op state for the event layer's life-release decision. */
KlUefiUdpOpResult kl_uefi_udp_op_state(KlSocketHandle fd, unsigned long long gen) {
    int stale = 0;
    if (udp_op_of(fd, gen, &stale)) return KL_UEFI_UDP_OP_PENDING;   /* still the live posted op */
    return stale ? udp_stale_class(fd, gen) : KL_UEFI_UDP_OP_INVALID;
}

unsigned long long kl_uefi_udp_generation_h(KlSocketHandle fd) {
    KlUefiUdp *u = udp_of(fd);   /* live read: captured at post time */
    return u ? (unsigned long long)u->generation : 0ULL;
}
int kl_uefi_udp_valid_h(KlSocketHandle fd, unsigned long long generation) {
    KlUefiUdp *u = udp_slot_of(fd);   /* stable storage — no UAF */
    return u && u->magic == KL_EFI_UDP_MAGIC && u->generation == (UINT64)generation && !u->dead;
}

/* ── token events (created once, reused across posts) ─────────────────────────────── */
static int udp_create_events(KlUefiUdp *u) {
    EFI_BOOT_SERVICES *bs = u->bs;
    EFI_STATUS st = bs->CreateEvent(KL_EFI_UDP_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                                    &u->rx_tok.Event);
    if (EFI_ERROR(st)) return -1;
    st = bs->CreateEvent(KL_EFI_UDP_EVT_TOKEN, TPL_CALLBACK, NULL, NULL,
                         &u->tx_tok.Event);
    if (EFI_ERROR(st)) {
        bs->CloseEvent(u->rx_tok.Event);
        u->rx_tok.Event = NULL;
        return -1;
    }
    u->events_created = 1;
    return 0;
}
static void udp_close_events(KlUefiUdp *u) {
    if (!u->events_created) return;
    EFI_BOOT_SERVICES *bs = u->bs;
    if (u->rx_tok.Event) bs->CloseEvent(u->rx_tok.Event);
    if (u->tx_tok.Event) bs->CloseEvent(u->tx_tok.Event);
    u->rx_tok.Event = NULL;
    u->tx_tok.Event = NULL;
    u->events_created = 0;
}

/* ── provider init ───────────────────────────────────────────────────────────────── */
int kl_uefi_udp_provider_init(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    if (g_uctx.created) return g_uctx.sb ? 0 : -1;
    if (!bs) return -1;
    u_zero(&g_uctx, sizeof(g_uctx));
    g_uctx.bs    = bs;
    g_uctx.image = image;

    UINTN n = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS st = bs->LocateHandleBuffer(ByProtocol, &g_udp4_sb_guid, NULL, &n, &handles);
    if (EFI_ERROR(st) || n == 0 || !handles) {
        if (handles) bs->FreePool(handles);
        g_uctx.created = 1;          /* inited, but no UDP4 stack present */
        return -1;
    }
    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    st = bs->HandleProtocol(handles[0], &g_udp4_sb_guid, (VOID **)&sb);
    if (EFI_ERROR(st) || !sb) {
        bs->FreePool(handles);
        g_uctx.created = 1;
        return -1;
    }
    g_uctx.sb         = sb;
    g_uctx.sb_handles = handles;
    g_uctx.created    = 1;
    return 0;
}

void kl_uefi_udp_provider_reset(void) {
    if (!g_uctx.created) return;
    if (g_uctx.sb_handles && g_uctx.bs) g_uctx.bs->FreePool(g_uctx.sb_handles);
    u_zero(&g_uctx, sizeof(g_uctx));
}

/* TEST SUPPORT ONLY: forcibly reclaim the ENTIRE UDP slot pool, INCLUDING quarantined slots — which the
 * production kl_uefi_udp_provider_reset intentionally PRESERVES (a quarantined slot is leaked to firmware
 * and must never be reused on real hardware). The host-mock harness calls this between tests so that
 * accumulated by-design quarantine leaks do not exhaust the small fixed pool (KL_EFI_MAX_UDP). Never
 * called on real firmware. */
void kl_uefi_udp_test_reset_pool(void) {
    for (int i = 0; i < KL_EFI_MAX_UDP; i++)
        u_zero(&g_udp_conns[i], sizeof(g_udp_conns[i]));
}

int kl_uefi_udp_provider_live_count(void) {
    int n = 0;
    for (int i = 0; i < KL_EFI_MAX_UDP; i++)
        if (g_udp_conns[i].magic == KL_EFI_UDP_MAGIC && !g_udp_conns[i].dead) n++;
    return n;
}
int kl_uefi_udp_provider_quarantined_count(void) {
    int n = 0;
    for (int i = 0; i < KL_EFI_MAX_UDP; i++)
        if (g_udp_conns[i].quarantined) n++;
    return n;
}

/* ── socket lifecycle ────────────────────────────────────────────────────────────── */
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif

KlSocketHandle kl_uefi_udp_socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    if (!g_uctx.created || !g_uctx.sb) return KL_INVALID_SOCKET;
    if (kl_uefi_after_ebs()) return KL_INVALID_SOCKET;   /* F3 fail-closed */
    if (type != SOCK_DGRAM) return KL_INVALID_SOCKET;

    int idx = -1;
    for (int i = 0; i < KL_EFI_MAX_UDP; i++)
        if (g_udp_conns[i].magic == 0) { idx = i; break; }
    if (idx < 0) return KL_INVALID_SOCKET;   /* pool exhausted */
    KlUefiUdp *u = &g_udp_conns[idx];

    UINT64 prev_gen = u->generation;
    u_zero(u, sizeof(*u));
    u->bs    = g_uctx.bs;
    u->image = g_uctx.image;
    u->sb    = g_uctx.sb;
    u->generation = prev_gen + 1;    /* odd == live */

    EFI_STATUS st = g_uctx.sb->CreateChild(g_uctx.sb, &u->child);
    if (EFI_ERROR(st) || !u->child) {
        u->last_status = st; u->generation++; u->magic = 0;
        return KL_INVALID_SOCKET;
    }
    st = g_uctx.bs->OpenProtocol(u->child, &g_udp4_guid, (VOID **)&u->udp,
                                 g_uctx.image, u->child, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st) || !u->udp)
        st = g_uctx.bs->OpenProtocol(u->child, &g_udp4_guid, (VOID **)&u->udp,
                                     g_uctx.image, u->child, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(st) || !u->udp) {
        u->last_status = st;
        g_uctx.sb->DestroyChild(g_uctx.sb, u->child);
        u->generation++; u->magic = 0;
        return KL_INVALID_SOCKET;
    }
    if (udp_create_events(u) != 0) {
        g_uctx.bs->CloseProtocol(u->child, &g_udp4_guid, g_uctx.image, u->child);
        g_uctx.sb->DestroyChild(g_uctx.sb, u->child);
        u->generation++; u->magic = 0;
        return KL_INVALID_SOCKET;
    }
    u->magic = KL_EFI_UDP_MAGIC;
    u->last_status = EFI_SUCCESS;
    return udp_handle_of_slot(idx);
}

/* Configure the child UNCONNECTED (per-datagram dest via Transmit UdpSessionData). @station
 * NULL = DHCP ephemeral; otherwise a bound station addr/port. Tolerates EFI_NO_MAPPING (DHCP
 * settle). Returns 0 / -1 (last_status set). */
static int udp_configure(KlUefiUdp *u, const EFI_IPv4_ADDRESS *station, UINT16 station_port) {
    EFI_UDP4_CONFIG_DATA cfg;
    u_zero(&cfg, sizeof(cfg));
    cfg.AcceptAnyPort      = FALSE;
    cfg.AllowDuplicatePort = TRUE;
    cfg.TimeToLive         = 64;
    cfg.ReceiveTimeout     = 0;     /* app pump-bounded, no driver timeout */
    cfg.TransmitTimeout    = 0;
    if (station) {
        cfg.UseDefaultAddress = FALSE;
        for (int i = 0; i < 4; i++) cfg.StationAddress.Addr[i] = station->Addr[i];
        cfg.StationPort = station_port;
    } else {
        cfg.UseDefaultAddress = TRUE;   /* DHCP station */
        cfg.StationPort = 0;            /* ephemeral source port */
    }
    /* No RemoteAddress → unconnected (multi-destination); dest carried per Transmit. */
    EFI_STATUS st = EFI_NOT_READY;
    for (int i = 0; i < KL_EFI_UDP_DHCP_RETRIES; i++) {
        st = u->udp->Configure(u->udp, &cfg);
        if (!EFI_ERROR(st)) break;
        if (st == EFI_NO_MAPPING) { u->udp->Poll(u->udp); u->bs->Stall(KL_EFI_UDP_DHCP_STALL_US); continue; }
        break;
    }
    u->last_status = st;
    if (EFI_ERROR(st)) return -1;
    u->config = cfg;
    u->configured = 1;
    return 0;
}

int kl_uefi_udp_bind(KlSocketHandle fd, const KlSockAddr *addr) {
    KlUefiUdp *u = udp_of(fd);
    if (!u) return -1;
    if (kl_uefi_after_ebs()) return -1;
    EFI_IPv4_ADDRESS ip; UINT16 port = 0;
    if (kl_efi_sockaddr_to_ipv4(addr, &ip, &port) != 0) return -1;   /* IPv4 only */
    return udp_configure(u, &ip, port);
}

int kl_uefi_udp_get_local_addr(KlSocketHandle fd, KlSockAddr *addr) {
    KlUefiUdp *u = udp_of(fd);
    if (!u || !addr) return -1;
    if (kl_uefi_after_ebs()) return -1;
    EFI_UDP4_CONFIG_DATA cfg;
    u_zero(&cfg, sizeof(cfg));
    EFI_STATUS st = u->udp->GetModeData(u->udp, &cfg, NULL, NULL, NULL);
    if (EFI_ERROR(st)) {
        /* Fall back to the pinned config (station may be unresolved pre-DHCP). */
        return kl_efi_ipv4_to_sockaddr(&u->config.StationAddress, u->config.StationPort, addr);
    }
    return kl_efi_ipv4_to_sockaddr(&cfg.StationAddress, cfg.StationPort, addr);
}

void kl_uefi_udp_set_cloexec(KlSocketHandle fd) { (void)fd; }   /* no fd inheritance on EFI */
int  kl_uefi_udp_set_nonblocking(KlSocketHandle fd) { return udp_of(fd) ? 0 : -1; }

KlIoStatus kl_uefi_udp_io_status(KlSocketHandle fd) {
    KlUefiUdp *u = udp_slot_of(fd);
    return u ? kl_efi_status_to_io(u->last_status) : KL_IO_FATAL;
}

/* ── pump helper (bounded; used by close/cancel drains) ──────────────────────────── */
static int udp_pump_until(EFI_BOOT_SERVICES *bs, EFI_UDP4_PROTOCOL *udp, EFI_EVENT ev, int spins) {
    for (int s = 0; s < spins; s++) {
        udp->Poll(udp);
        if (bs->CheckEvent(ev) == EFI_SUCCESS) return 1;
        bs->Stall(KL_EFI_UDP_STALL_US);
    }
    return 0;
}

/* Recycle a signalled receive token's firmware buffer (return it to the driver). Safe on a
 * NULL RxData / RecycleSignal. NEVER call for an UNsignalled token (Packet is not valid). */
static void udp_recycle_rx(KlUefiUdp *u) {
    EFI_UDP4_RECEIVE_DATA *rx = u->rx_tok.Packet.RxData;
    if (rx && rx->RecycleSignal) u->bs->SignalEvent(rx->RecycleSignal);
    u->rx_tok.Packet.RxData = NULL;
}

/* ── completion-native token primitives ──────────────────────────────────────────── */
int kl_uefi_udp_recv_posted(KlSocketHandle fd, unsigned long long gen) {
    KlUefiUdp *u = udp_op_of(fd, gen, NULL); return u && u->rx_posted;
}
int kl_uefi_udp_send_posted(KlSocketHandle fd, unsigned long long gen) {
    KlUefiUdp *u = udp_op_of(fd, gen, NULL); return u && u->tx_posted;
}

int kl_uefi_udp_post_recv(KlSocketHandle fd) {
    KlUefiUdp *u = udp_of(fd);
    if (!u) return -1;
    if (kl_uefi_after_ebs()) return -1;
    if (u->rx_posted) return 0;                 /* one receive in flight — idempotent */
    u->rx_tok.Status = EFI_NOT_READY;
    u->rx_tok.Packet.RxData = NULL;             /* Event field is preserved from socket() */
    EFI_STATUS st = u->udp->Receive(u->udp, &u->rx_tok);
    u->last_status = st;
    if (EFI_ERROR(st)) return -1;
    u->rx_posted = 1;
    return 0;
}

KlUefiUdpOpResult kl_uefi_udp_poll_recv(KlSocketHandle fd, unsigned long long gen,
                          void *copy_into, size_t cap,
                          KlSockAddr *out_src, KlSockAddr *out_local,
                          int *out_trunc, size_t *out_bytes, int *out_ok) {
    if (out_trunc) *out_trunc = 0;
    if (out_bytes) *out_bytes = 0;
    if (out_ok)    *out_ok    = 0;
    int stale = 0;
    KlUefiUdp *u = udp_op_of(fd, gen, &stale);   /* EXACT op via stable storage + generation */
    if (!u)
        return stale ? udp_stale_class(fd, gen)  /* STALE_RETIRED (release life) vs QUARANTINED (retain) */
                     : KL_UEFI_UDP_OP_INVALID;   /* not a datagram handle */
    if (!u->rx_posted) return KL_UEFI_UDP_OP_PENDING;
    if (kl_uefi_after_ebs()) return KL_UEFI_UDP_OP_PENDING;
    u->udp->Poll(u->udp);
    if (u->bs->CheckEvent(u->rx_tok.Event) != EFI_SUCCESS)
        return KL_UEFI_UDP_OP_PENDING;           /* not yet signalled */

    /* Signalled: Packet is now valid. */
    u->rx_posted = 0;
    u->last_status = u->rx_tok.Status;
    EFI_UDP4_RECEIVE_DATA *rx = u->rx_tok.Packet.RxData;
    if (rx && !EFI_ERROR(u->rx_tok.Status) && copy_into) {
        size_t off = 0; int over = 0;
        for (UINT32 f = 0; f < rx->FragmentCount; f++) {
            const unsigned char *fb = (const unsigned char *)rx->FragmentTable[f].FragmentBuffer;
            UINT32 fl = rx->FragmentTable[f].FragmentLength;
            if (!fb) continue;
            for (UINT32 k = 0; k < fl; k++) {
                if (off < cap) ((unsigned char *)copy_into)[off++] = fb[k];
                else { over = 1; }
            }
        }
        if (out_bytes) *out_bytes = off;
        if (out_trunc) *out_trunc = over;
        if (out_ok)    *out_ok    = 1;           /* success incl. a genuine 0-length datagram */
        if (out_src)   kl_efi_ipv4_to_sockaddr(&rx->UdpSession.SourceAddress,
                                               rx->UdpSession.SourcePort, out_src);
        if (out_local) kl_efi_ipv4_to_sockaddr(&rx->UdpSession.DestinationAddress,
                                               rx->UdpSession.DestinationPort, out_local);
    }
    /* Return the firmware buffer on any signalled terminal with a published RxData — incl. the
     * NULL-copy (drain-without-deliver) and aborted-but-signalled paths. */
    udp_recycle_rx(u);
    return KL_UEFI_UDP_OP_DELIVERED;
}

/* Internal transmit: build the ENTIRE session (dest + optional source pin) and descriptor
 * BEFORE Transmit — the firmware may consume the descriptor during Transmit, so nothing
 * reachable from the token may be mutated after submit. @src NULL = no source pin (completion
 * send); a non-NULL IPv4 @src is written into UdpSession.SourceAddress. Shared by the public
 * completion post (src=NULL) and the sync source-pinned fallback. */
static int udp_post_send_ex(KlUefiUdp *u, const void *data, size_t len,
                            const KlSockAddr *dest, const KlSockAddr *src) {
    if (!u) return -1;
    if (kl_uefi_after_ebs()) return -1;
    if (u->tx_posted) return -1;                 /* a send is already outstanding */
    if (len > KL_EFI_UDP_TXBUF) return -1;        /* larger than the stable staging buffer */
    EFI_IPv4_ADDRESS dip; UINT16 dport = 0;
    if (kl_efi_sockaddr_to_ipv4(dest, &dip, &dport) != 0) return -1;   /* per-datagram dest, IPv4 */

    const unsigned char *sb = data;
    for (size_t i = 0; i < len; i++) u->tx_buf[i] = sb[i];

    u_zero(&u->tx_sess, sizeof(u->tx_sess));
    for (int i = 0; i < 4; i++) u->tx_sess.DestinationAddress.Addr[i] = dip.Addr[i];
    u->tx_sess.DestinationPort = dport;
    /* Optional source pin — set BEFORE submit. A non-NULL source MUST be a valid IPv4; an
     * unsupported/invalid source is REJECTED (fail, send nothing) rather than silently sent
     * from the default source. NULL = no source pin (the completion send path). */
    if (src) {
        EFI_IPv4_ADDRESS sip; UINT16 sport = 0;
        if (kl_sockaddr_family(src) != KL_AF_INET ||
            kl_efi_sockaddr_to_ipv4(src, &sip, &sport) != 0) {
            u->last_status = EFI_INVALID_PARAMETER;
            return -1;   /* nothing submitted (tx_posted stays 0) */
        }
        for (int i = 0; i < 4; i++) u->tx_sess.SourceAddress.Addr[i] = sip.Addr[i];
        u->tx_sess.SourcePort = sport;
    }

    u_zero(&u->tx_desc, sizeof(u->tx_desc));
    u->tx_desc.UdpSessionData = &u->tx_sess;
    u->tx_desc.DataLength     = (UINT32)len;
    u->tx_desc.FragmentCount  = 1;
    u->tx_desc.FragmentTable[0].FragmentLength = (UINT32)len;
    u->tx_desc.FragmentTable[0].FragmentBuffer = u->tx_buf;

    u->tx_tok.Status = EFI_NOT_READY;
    u->tx_tok.Packet.TxData = &u->tx_desc;
    EFI_STATUS st = u->udp->Transmit(u->udp, &u->tx_tok);   /* firmware may consume the descriptor now */
    u->last_status = st;
    if (EFI_ERROR(st)) return -1;
    u->tx_posted = 1;
    return 0;
}

int kl_uefi_udp_post_send(KlSocketHandle fd, const void *data, size_t len, const KlSockAddr *dest) {
    return udp_post_send_ex(udp_of(fd), data, len, dest, NULL);   /* completion send: no source pin */
}

KlUefiUdpOpResult kl_uefi_udp_poll_send(KlSocketHandle fd, unsigned long long gen,
                          size_t *out_bytes, int *out_ok) {
    if (out_bytes) *out_bytes = 0;
    if (out_ok)    *out_ok    = 0;
    int stale = 0;
    KlUefiUdp *u = udp_op_of(fd, gen, &stale);
    if (!u)
        return stale ? udp_stale_class(fd, gen) : KL_UEFI_UDP_OP_INVALID;
    if (!u->tx_posted) return KL_UEFI_UDP_OP_PENDING;
    if (kl_uefi_after_ebs()) return KL_UEFI_UDP_OP_PENDING;
    u->udp->Poll(u->udp);
    if (u->bs->CheckEvent(u->tx_tok.Event) != EFI_SUCCESS)
        return KL_UEFI_UDP_OP_PENDING;
    u->tx_posted = 0;
    u->last_status = u->tx_tok.Status;
    int ok = !EFI_ERROR(u->tx_tok.Status);
    if (out_ok)    *out_ok    = ok;
    if (out_bytes) *out_bytes = ok ? (size_t)u->tx_desc.DataLength : 0;
    return KL_UEFI_UDP_OP_DELIVERED;
}

/* Cancel + bounded drain a Receive token. RETIRED = confirmed now (a signalled RxData is recycled);
 * a live op with nothing outstanding = RETIRED; QUARANTINED = unconfirmed drain (caller quarantines)
 * OR an already-quarantined slot; a cleanly-gone op = STALE_RETIRED; INVALID = not a datagram handle.
 * Resolves the EXACT op via {fd,@gen} + stable storage — never reports a quarantined op as retired. */
KlUefiUdpOpResult kl_uefi_udp_cancel_recv(KlSocketHandle fd, unsigned long long gen) {
    int stale = 0;
    KlUefiUdp *u = udp_op_of(fd, gen, &stale);
    if (!u) return stale ? udp_stale_class(fd, gen) : KL_UEFI_UDP_OP_INVALID;
    if (!u->rx_posted) return KL_UEFI_UDP_OP_RETIRED;   /* live, nothing outstanding */
    if (kl_uefi_after_ebs()) return KL_UEFI_UDP_OP_QUARANTINED;   /* cannot drive EFI → unconfirmed */
    u->udp->Cancel(u->udp, &u->rx_tok);
    if (udp_pump_until(u->bs, u->udp, u->rx_tok.Event, KL_EFI_UDP_CANCEL_DRAIN_SPINS)) {
        u->rx_posted = 0;
        udp_recycle_rx(u);                       /* signalled (EFI_ABORTED) → return the buffer */
        return KL_UEFI_UDP_OP_RETIRED;
    }
    return KL_UEFI_UDP_OP_QUARANTINED;           /* unconfirmed — caller quarantines */
}

KlUefiUdpOpResult kl_uefi_udp_cancel_send(KlSocketHandle fd, unsigned long long gen) {
    int stale = 0;
    KlUefiUdp *u = udp_op_of(fd, gen, &stale);
    if (!u) return stale ? udp_stale_class(fd, gen) : KL_UEFI_UDP_OP_INVALID;
    if (!u->tx_posted) return KL_UEFI_UDP_OP_RETIRED;
    if (kl_uefi_after_ebs()) return KL_UEFI_UDP_OP_QUARANTINED;
    u->udp->Cancel(u->udp, &u->tx_tok);
    if (udp_pump_until(u->bs, u->udp, u->tx_tok.Event, KL_EFI_UDP_CANCEL_DRAIN_SPINS)) {
        u->tx_posted = 0;
        return KL_UEFI_UDP_OP_RETIRED;
    }
    return KL_UEFI_UDP_OP_QUARANTINED;
}

/* Centralized QUARANTINE transition (review-High): the single place that establishes the invariant
 * an op {fd, captured_gen} needs to later classify as KL_UEFI_UDP_OP_QUARANTINED. Used by BOTH
 * kl_uefi_udp_close (unconfirmed drain) and the synchronous dg_send timeout. Atomic + idempotent:
 *   dead = 1 · quarantined = 1 · generation bumped EXACTLY ONCE (captured_gen == generation - 1) ·
 *   magic/child/events/token storage RETAINED (never CloseEvent/Configure(NULL)/CloseProtocol/
 *   DestroyChild) · slot never reusable (socket() scans magic==0; a quarantined slot keeps its magic).
 * Callers must NOT have bumped the generation themselves (close bumps only in its terminal branches). */
static void udp_quarantine(KlUefiUdp *u) {
    if (u->quarantined) return;     /* idempotent — bump the generation exactly once */
    u->dead        = 1;
    u->quarantined = 1;
    u->generation++;                /* the quarantined op's captured_gen == generation - 1 */
}

/*
 * close(): reconcile every outstanding token before teardown so the firmware cannot write
 * freed/reused storage. Cancel(NULL) all tokens, drain each posted one; if any cannot be
 * confirmed retired → udp_quarantine() (leak the slot to EBS, never CloseEvent/DestroyChild). F3
 * post-EBS: touch no boot service, just free the slot. The generation is bumped exactly once in
 * each TERMINAL branch (clean / quarantine / EBS) — NOT at entry — so the quarantine helper is the
 * sole bump on that path (even == closed / odd == live is preserved; no double bump).
 */
int kl_uefi_udp_close(KlSocketHandle fd) {
    KlUefiUdp *u = udp_of(fd);
    if (!u) return 0;               /* already invalid / dead / quarantined — nothing to do */
    u->dead = 1;                    /* mark dead (single-threaded: no poll interleaves the drain) */

    if (kl_uefi_after_ebs()) { u->generation++; u->magic = 0; return 0; }   /* F3: no boot service */

    EFI_BOOT_SERVICES *bs = u->bs;
    EFI_UDP4_PROTOCOL *udp = u->udp;
    int drained_ok = 1;

    if (udp) udp->Cancel(udp, NULL);   /* abort all outstanding tokens at once */

    if (udp && u->rx_posted) {
        if (udp_pump_until(bs, udp, u->rx_tok.Event, KL_EFI_UDP_CANCEL_DRAIN_SPINS)) {
            u->rx_posted = 0;
            udp_recycle_rx(u);         /* signalled → return the firmware buffer */
        } else {
            drained_ok = 0;            /* unconfirmed: Packet stays untouched */
        }
    }
    if (udp && u->tx_posted) {
        if (udp_pump_until(bs, udp, u->tx_tok.Event, KL_EFI_UDP_CANCEL_DRAIN_SPINS))
            u->tx_posted = 0;
        else
            drained_ok = 0;
    }

    if (!drained_ok) { udp_quarantine(u); return 0; }   /* leak until EBS (helper bumps generation) */

    u->generation++;                      /* clean close: even == closed (reused → socket() → odd) */
    udp_close_events(u);
    if (udp) udp->Configure(udp, NULL);   /* reset config */
    if (u->child) {
        bs->CloseProtocol(u->child, &g_udp4_guid, u->image, u->child);
        if (u->sb) u->sb->DestroyChild(u->sb, u->child);
    }
    u->magic = 0;   /* slot free + reclaimable (all tokens confirmed retired) */
    return 0;
}

/* ── KlDatagramOps: configure (mandatory) + sync send fallback; recv = NULL ──────────
 *
 * ERROR-CHANNEL LIMITATION + ACCEPTANCE (KlDatagramOps.configure returns a capture-cap mask,
 * NOT a status — cross-cutting to add one). EFI_UDP4.Configure can genuinely fail (no DHCP,
 * bad station). On failure we FAIL-CLOSE the slot (full teardown here) so udp_of(fd) returns
 * NULL and EVERY subsequent op on the socket fails. The point at which that failure SURFACES
 * differs, and is a documented, narrowed acceptance:
 *   - BOUND socket (cfg->bind_addr): the single Configure is done by kl_uefi_udp_bind(), whose
 *     -1 return kl_udp_init() checks (src/udp.c) → kl_udp_init() FAILS. (Nothing is configured
 *     here — see the single-Configure note below.)
 *   - UNBOUND / DHCP-default socket (the DNS resolver): kl_udp_init() makes no further provider
 *     call after configure(), so it still RETURNS SUCCESS on a fail-closed slot. The failure
 *     instead surfaces at the FIRST OPERATION — kl_udp_recv_start()'s post_dgram_recv (or a
 *     send) returns -1 on the dead slot. The stock resolver handles this cleanly:
 *     kl_dns_resolver_create() checks kl_udp_recv_start() and tears down (returns NULL). A raw
 *     KlUdp user who never starts receiving holds a socket whose every op fails (safe, not
 *     silently-wrong). This deferred-surface path is covered by the mock state-machine tests.
 *
 * SINGLE Configure per child (a second Configure on an already-started child returns
 * EFI_ALREADY_STARTED): udp.c calls configure() BEFORE kl_sock_bind(), so an explicit
 * bind_addr is DEFERRED to kl_uefi_udp_bind() (the only Configure for a bound socket); the
 * unbound/default case is configured here (no bind() follows). */
static uint32_t dg_configure(void *ctx, KlSocketHandle fd, int family,
                             const struct KlDatagramSocketConfig *cfg) {
    (void)ctx; (void)family;
    KlUefiUdp *u = udp_of(fd);
    if (!u) return 0;
    if (cfg && cfg->bind_addr)
        return 0;   /* explicit station → kl_uefi_udp_bind does the single Configure (checked at init) */
    if (udp_configure(u, NULL, 0) != 0)
        (void)kl_uefi_udp_close(fd);   /* fail-close: every subsequent op fails (see acceptance above) */
    return 0;       /* no KL_DGRAM_RX_* capture caps: src+local ride each completion event natively */
}

/* Sync single-Transmit fallback for the source-pinned / TOS path udp.c routes off the
 * completion fast path. Independent Tx token under the same quarantine discipline. The source
 * pin is written into the session BEFORE submit (via udp_post_send_ex).
 *
 * TOS is PROVIDE-OR-REJECT (frozen contract §9): EFI_UDP4 has no per-datagram TOS — it carries
 * TypeOfService through Configure, not the Transmit token — so an explicitly requested
 * per-datagram tos (>= 0) cannot be honored and is REJECTED (fail, send nothing) rather than
 * silently transmitted without the requested marking. tos < 0 = "no per-datagram TOS" proceeds. */
static kl_ssize_t dg_send(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                          const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    (void)ctx;
    KlUefiUdp *u = udp_of(fd);
    if (!u) {   /* not a live datagram socket — publish a fatal classification so udp.c drops, not queues */
        kl_uefi_provider_set_last_status(EFI_INVALID_PARAMETER);
        return -1;
    }
    kl_ssize_t rv;
    if (tos >= 0) {
        u->last_status = EFI_UNSUPPORTED;   /* per-datagram TOS not representable → reject (send nothing) */
        rv = -1;
    } else if (udp_post_send_ex(u, data, len, dest, src) != 0) {
        rv = -1;                             /* u->last_status set inside (invalid src / too large / EFI) */
    } else if (!udp_pump_until(u->bs, u->udp, u->tx_tok.Event, KL_EFI_UDP_PUMP_SPINS)) {
        if (kl_uefi_udp_cancel_send(fd, u->generation) == KL_UEFI_UDP_OP_QUARANTINED)
            udp_quarantine(u);   /* centralized: dead + quarantined + generation bumped exactly once */
        u->last_status = EFI_TIMEOUT;
        rv = -1;
    } else {
        u->tx_posted = 0;
        u->last_status = u->tx_tok.Status;
        rv = EFI_ERROR(u->tx_tok.Status) ? -1 : (kl_ssize_t)len;
    }
    /* Publish the datagram op's classification into the provider-global io_status channel so udp.c
     * (which reads kl_sock_io_status after a -1) drops an unsupported/failed send instead of queuing. */
    kl_uefi_provider_set_last_status(u->last_status);
    return rv;
}

static const KlDatagramOps EFI_UDP4_DGRAM_OPS = {
    .send      = dg_send,
    .recv      = NULL,          /* FROZEN #1: no second receive machine — completion posts recv */
    .configure = dg_configure,
    /* send_gso/set_tos/mcast_membership + mmsg batching = NULL (EFI_UDP4 has no analogue;
     * capabilities the resolver never uses). */
};

const KlDatagramOps *kl_uefi_udp_dgram_ops(void) { return &EFI_UDP4_DGRAM_OPS; }
