#ifndef KEEL_DATAGRAM_H
#define KEEL_DATAGRAM_H

/*
 * datagram.h: the public fixed-slot KlDatagram API.
 *
 * KlDatagram is THE datagram primitive: a caller-owned, single-threaded, event-loop-driven handle over
 * a prepared UDP fd. It is a thin facade over the internal KlDgramCore assembly and is the single
 * canonical message transport for portable protocols (batching/GSO/GRO/multicast/source-pin/per-packet
 * TOS all ride this surface).
 *
 * This header is also the single home for the public datagram data TYPES (KlDatagramMessage,
 * KlDatagramSendStatus, KlDatagramCloseResult, KlDgramCloseState, KL_DGRAM_CAP_*, KL_DGRAM_HAS_LOCAL/
 * TRUNCATED); the internal machine headers (src/datagram_*.h) include this header for them.
 *
 * STABILITY. The kl_datagram_* FUNCTION + TYPE CONTRACT is STABLE: validated LIVE across every
 * supported event backend: the completion loops pollcomp, io_uring, Windows IOCP, raw lwIP-raw, and
 * firmware EFI_UDP4, plus the readiness loops POSIX epoll/kqueue/poll and Windows WSAPoll; each
 * validated in docs/contracts/datagram.md §10. The
 * struct LAYOUT is NOT ABI-stable: a consumer that stack-/embed-allocates a KlDatagram opts into the
 * layout by including <keel/datagram_detail.h> and MUST recompile when it changes. A consumer that only
 * holds a `KlDatagram *` (created behind the API) is insulated from layout.
 *
 * kl_datagram_init_ex additively extends the STABLE surface: it selects the BOTH (count+byte)
 * send-queue policy; kl_datagram_init and the KlDatagramConfig layout are UNCHANGED (the budget is a
 * parameter, not a struct field), so the extension never modifies the existing contract.
 *
 * The provider data-plane vtable (KlDatagramOps + KlDgramRx/TxDesc descriptors + KL_DGRAM_RX_ flags)
 * lives on the socket axis in <keel/socket_dgram.h>, re-exported here so `#include <keel/datagram.h>`
 * reaches it.
 */

#include <keel/socket_dgram.h>   /* provider datagram data-plane (re-export) */
#include <keel/handle.h>         /* KlSocketHandle */
#include <keel/sockaddr.h>       /* KlSockAddr */
#include <keel/allocator.h>      /* KlAllocator */
#include <keel/error.h>          /* KlError */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct KlEventCtx;
struct KlSocketProvider;
struct KlDatagram;                       /* layout in <keel/datagram_detail.h> (opt-in) */
typedef struct KlDatagram KlDatagram;

/* ── Received-datagram metadata flags (a delivery's `flags`) ─────────────────────────────────── */
#define KL_DGRAM_TRUNCATED  (1u << 0)  /* captured prefix: the datagram exceeded inbound capacity */
#define KL_DGRAM_HAS_LOCAL  (1u << 1)  /* `local` (destination / receiving interface) is valid */

/* ── Requested-feature capabilities (KlDatagramConfig.want_caps + kl_datagram_caps) ───────────── */
#define KL_DGRAM_CAP_SOURCE_PIN  (1u << 0)   /* msg.local source-pin on send */
#define KL_DGRAM_CAP_TOS         (1u << 1)   /* per-packet TOS/ECN on send */
#define KL_DGRAM_CAP_CONNECTED   (1u << 2)   /* connected-mode send (msg.peer == NULL) */
#define KL_DGRAM_CAP_MULTICAST   (1u << 3)   /* runtime multicast join/leave (kl_datagram_multicast_*) */
#define KL_DGRAM_CAP_BROADCAST   (1u << 4)   /* SO_BROADCAST: IPv4 fds only (reported per-fd); config via KlDatagramSocketConfig */
/* Provider SUPPORT for the optional high-throughput batch extension (kl_datagram_provider_caps).
 * Directional, and advisory for GSO/GRO: RX_BATCH/TX_BATCH = the provider has recvmmsg/sendmmsg;
 * GSO = the send_gso op exists (first-use may still fail → extension latch); GRO = the provider CAN
 * capture UDP_GRO (per-socket activation ALSO needs KlDatagramConfig.accepted_rx_caps &
 * KL_DGRAM_RX_GRO). See <keel/datagram_batch.h>. */
#define KL_DGRAM_CAP_RX_BATCH    (1u << 5)   /* recvmmsg batching */
#define KL_DGRAM_CAP_TX_BATCH    (1u << 6)   /* sendmmsg batching */
#define KL_DGRAM_CAP_GSO         (1u << 7)   /* UDP GSO segmentation offload (advisory) */
#define KL_DGRAM_CAP_GRO         (1u << 8)   /* UDP GRO receive coalescing (provider support) */

/* ── TOS / DSCP / ECN helpers (datagram-neutral; compose KlDatagramMessage.tos + kl_datagram_set_tos) ──
 * DSCP class selectors + Expedited Forwarding (RFC 2474 / 4594). Shift into the TOS / Traffic-Class byte
 * with KL_TOS(). */
#define KL_DSCP_CS0   0
#define KL_DSCP_CS1   8
#define KL_DSCP_CS2  16
#define KL_DSCP_CS3  24
#define KL_DSCP_CS4  32
#define KL_DSCP_CS5  40
#define KL_DSCP_CS6  48
#define KL_DSCP_CS7  56
#define KL_DSCP_AF11 10
#define KL_DSCP_AF21 18
#define KL_DSCP_AF31 26
#define KL_DSCP_AF41 34
#define KL_DSCP_EF   46
/* ECN codepoints: the low 2 bits of the TOS byte (RFC 3168). */
#define KL_ECN_NOT_ECT 0
#define KL_ECN_ECT1    1
#define KL_ECN_ECT0    2
#define KL_ECN_CE      3
/* Compose a TOS/Traffic-Class byte from a DSCP class and an ECN codepoint. */
#define KL_TOS(dscp, ecn) ((((dscp) & 0x3f) << 2) | ((ecn) & 0x3))

/* ── Send status: atomic accept-or-refuse, no ownership on refusal (invariant 4) ──────────────── */
typedef enum {
    KL_DATAGRAM_ACCEPTED = 0,   /* whole datagram taken (sent or slot-queued) */
    KL_DATAGRAM_WOULD_BLOCK,    /* transient: no free outbound slot, or the BOTH byte gate; nothing taken */
    KL_DATAGRAM_TOO_LARGE,      /* permanent: exceeds a hard configured send-path capacity: one slot
                                   (send_slot_cap), or the whole send_byte_budget under BOTH; nothing taken */
    KL_DATAGRAM_UNSUPPORTED,    /* an explicitly-requested capability is unavailable; nothing sent */
    KL_DATAGRAM_CLOSED,         /* closing/closed: no further sends */
    KL_DATAGRAM_ERROR           /* bad argument or sticky transport error */
} KlDatagramSendStatus;

/* ── One outbound datagram (borrowed; COPIED before kl_datagram_send returns) ──────────────────── */
typedef struct {
    const void       *data;
    size_t            len;
    const KlSockAddr *peer;    /* destination; NULL = connected-mode send */
    const KlSockAddr *local;   /* source/interface pin; NULL = none */
    int               tos;     /* per-packet TOS/ECN byte, or -1 = none */
    unsigned          flags;   /* stored on the slot; opaque to the send machine */
} KlDatagramMessage;

/* ── Close lifecycle phase + terminal classification (§4) ──────────────────────────────────────── */
typedef enum {
    KL_DGRAM_CLOSE_OPEN = 0,       /* lifecycle PHASE (not the terminal result) */
    KL_DGRAM_CLOSE_CLOSING,
    KL_DGRAM_CLOSE_CLOSED
} KlDgramCloseState;

typedef enum {
    KL_DGRAM_CLOSE_NONE = 0,   /* SENTINEL: no terminal reached yet (also a zeroed/reused object) */
    KL_DGRAM_DETACHED,         /* CONFIRMED physical retirement of every op: the strong guarantee */
    KL_DGRAM_QUARANTINED,      /* wrapper-safe but an op could NOT be confirmed retired (fail-closed) */
    KL_DGRAM_CLOSE_ERROR       /* terminal transport error AND every op nevertheless RETIRED (§4.3) */
} KlDatagramCloseResult;

/* ── Public callbacks ─────────────────────────────────────────────────────────────────────────── */
/* One received datagram (borrowed from the inbound slot; valid ONLY for the call). `peer` is always
 * non-NULL; `local` is non-NULL iff KL_DGRAM_HAS_LOCAL is set in `flags`; `flags` may carry
 * KL_DGRAM_TRUNCATED. `len` is the captured-prefix length. */
typedef void (*KlDatagramRecvFn)(void *ud, const void *data, size_t len,
                                 const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);
typedef void (*KlDatagramWritableFn)(void *ud);   /* send full→non-full edge (reentrant, non-destructive) */
typedef void (*KlDatagramDrainFn)(void *ud);      /* send non-empty→empty edge (destructive tail) */
typedef void (*KlDatagramCloseFn)(void *ud, KlDatagramCloseResult result);  /* terminal (fires once) */

/* ── Configuration (all pointers borrowed) ────────────────────────────────────────────────────── */
typedef struct {
    struct KlEventCtx       *ctx;        /* the event loop (selects completion vs readiness) */
    KlAllocator             *alloc;      /* MUST outlive posted ops (frees the life-owned rx holder) */
    const struct KlSocketProvider *sockets;  /* the datagram-capable socket provider */
    KlSocketHandle           fd;         /* already socket()'d/configure()'d/bound via sockets->dgram */
    size_t                   send_slots;    /* fixed outbound slot count (count-based backpressure) */
    size_t                   send_slot_cap; /* per outbound slot payload capacity */
    size_t                   recv_cap;      /* inbound slot payload capacity */
    unsigned                 want_caps;     /* KL_DGRAM_CAP_* the consumer REQUIRES (fail-loud: init -1
                                             * with KL_ERR_UNSUPPORTED if the provider lacks any) */
    /* KL_DGRAM_CAP_* the consumer would LIKE but does not require: granted opportunistically
     * where the provider supports them, silently dropped otherwise (never an init failure). The granted
     * cap set (kl_datagram_caps) is `want_caps | (optional_caps & provider_caps)`. Lets a consumer
     * request every capability its use may need without regressing reduced/freestanding providers that
     * only support unconnected send_to. 0 = request nothing beyond want_caps. */
    unsigned                 optional_caps;
    /* the KL_DGRAM_RX_* capture mask the socket actually has ENABLED (from the prep's
     * KlDatagramPrep.rx_caps): enabled-per-socket state, SEPARATE from provider support. init masks it
     * to known KL_DGRAM_RX_* bits and stores it; GRO activates only when the provider supports GRO AND
     * this has KL_DGRAM_RX_GRO. 0 = the caller knows of no enabled capture (graceful). */
    unsigned                 accepted_rx_caps;
} KlDatagramConfig;

/* ── Lifecycle ────────────────────────────────────────────────────────────────────────────────── */
/* Initialize `dg` (a memset-zero handle) over the prepared fd. fd OWNERSHIP TRANSFERS to KlDatagram
 * ONLY when this returns 0; on failure nothing the caller must reclaim is touched (the caller retains
 * the fd). Returns 0, or -1 (kl_datagram_last_error carries why).
 *
 * kl_datagram_init selects the SLOT send-queue policy (count-bounded backpressure, the STABLE
 * default). kl_datagram_init_ex is the additive variant: send_byte_budget == 0 is identical to
 * kl_datagram_init (SLOT); send_byte_budget > 0 selects the BOTH policy: admission is bounded by both
 * the slot COUNT and that byte budget (bytes of queued+in-flight payload). Any budget > 0 is valid,
 * including one below send_slot_cap: a datagram larger than the whole budget is a permanent
 * KL_DATAGRAM_TOO_LARGE (delivered first if the socket is ready). See docs/datagram_m1_queue_policy_*. */
int  kl_datagram_init(KlDatagram *dg, const KlDatagramConfig *cfg);
int  kl_datagram_init_ex(KlDatagram *dg, const KlDatagramConfig *cfg, size_t send_byte_budget);

/* ── Public socket convenience (create + configure + bind + adopt in one call) ────────────────
 * kl_datagram_init/_init_ex above adopt an ALREADY-prepared fd (advanced / bring-your-own). Most callers
 * want the socket created for them: kl_datagram_socket_init creates + configures + (optionally) binds +
 * adopts, reusing the M0 provider-neutral preparation internally. Works on every loop model + provider
 * (POSIX, Winsock/IOCP, lwIP, EFI). See docs/archive/designs/datagram_m6_kludp_decision_design.md §12. */

/* Send-queue backpressure policy. A zeroed config selects DEFAULT == BOTH. */
typedef enum {
    KL_DATAGRAM_QUEUE_DEFAULT = 0,  /* == BOTH (the sensible default for a UDP socket) */
    KL_DATAGRAM_QUEUE_SLOT,         /* count-only backpressure; send_byte_budget IGNORED */
    KL_DATAGRAM_QUEUE_BOTH,         /* count + byte backpressure */
} KlDatagramQueuePolicy;

typedef struct KlDatagramSocketConfig {   /* named tag: the datagram provider seam (socket_dgram.h
                                           * configure() + M0 kl_datagram_open) references it by tag */
    struct KlEventCtx             *ctx;        /* event loop (selects completion vs readiness) */
    const struct KlSocketProvider *sockets;    /* datagram-capable provider; NULL = ctx->sockets; if that
                                                * is NULL too, the built-in default provider */
    KlAllocator                   *alloc;      /* NULL = ctx->alloc */

    /* ── socket creation + options ── */
    int          family;            /* AF_INET / AF_INET6 / AF_UNSPEC (auto from bind_addr) */
    const char  *bind_addr;         /* numeric bind address, or NULL = unbound (pure client) */
    uint16_t     bind_port;         /* bind port; 0 = ephemeral (ignored when bind_addr == NULL) */
    int          reuse_addr, reuse_port;
    int          recv_pktinfo;      /* capture each datagram's local (dest) addr */
    int          recv_tos;          /* deliver each datagram's TOS (kl_datagram_recv_tos) */
    int          recv_gro;          /* enable UDP_GRO capture (readiness only; ignored on completion).
                                     * Enabling it OBLIGES attaching a splitting RECV batch before
                                     * recv_start; else recv_start refuses (KL_ERR_UNSUPPORTED). */
    int          broadcast;         /* SO_BROADCAST (IPv4) */
    int          multicast_ttl, multicast_disable_loop;
    unsigned     multicast_iface;   /* egress interface index; 0 = kernel default */
    const char  *multicast_group;   /* optional numeric group joined at init (after bind); NULL/"" = none.
                                     * A NON-EMPTY group is a MANDATORY multicast request: CAP_MULTICAST is
                                     * added to required caps (init fails if unsupported). */
    int          so_rcvbuf, so_sndbuf;
    int          tos;               /* socket-default TOS/Traffic-Class (IP_TOS / IPV6_TCLASS) */

    /* ── sizing + policy (explicit units; 0 = documented default) ── */
    KlDatagramQueuePolicy queue_policy; /* 0 = DEFAULT = BOTH; unknown value → KL_ERR_INVALID_ARG */
    size_t       send_slots;        /* fixed outbound slot COUNT; 0 = derived (BOTH) / 8 (SLOT) */
    size_t       send_slot_cap;     /* per-slot payload BYTES; 0 = 65507 (a full UDP payload) */
    size_t       send_byte_budget;  /* BOTH byte-gate BYTES; 0 = 262144; ignored under SLOT */
    size_t       recv_cap;          /* inbound slot payload BYTES; 0 = 2048 (capped 65535) */

    /* ── capabilities ── */
    unsigned     want_caps;         /* required SEND-side caps (fail-loud, exact-fd) */
    unsigned     optional_caps;     /* grant-if-supported; the initializer ALWAYS adds CAP_CONNECTED */
    unsigned     want_rx_caps;      /* required RX-CAPTURE mask (KL_DGRAM_RX_PKTINFO / RX_TOS): init FAILS
                                     * (fd closed once) if the socket did not ACCEPT every bit: the
                                     * receive-side analog of want_caps (source-pinned reply needs
                                     * RX_PKTINFO; want_caps=SOURCE_PIN proves only the send half). */
} KlDatagramSocketConfig;

/* Create + configure + (optionally) bind + adopt into `dg` (a memset-zero handle). On success (0) the fd
 * is owned by KlDatagram (closed once at teardown). On failure (-1, kl_datagram_last_error set) the fd is
 * closed exactly once on every path (pre-adoption prep, required-RX-mask reject, init-before-adoption,
 * post-adoption multicast-join). The initializer always requests CONNECTED opportunistically so
 * kl_datagram_connect works wherever the provider supports it. */
int kl_datagram_socket_init(KlDatagram *dg, const KlDatagramSocketConfig *cfg);

/* Provider-neutral connect (first-time only). Requires the GRANTED kl_datagram_caps() CAP_CONNECTED bit;
 * calls the socket provider's connect seam and, on success, records the datagram's connected state so a
 * peerless send (KlDatagramMessage.peer == NULL) is admitted. Returns 0, or -1 with kl_datagram_last_
 * error(): KL_ERR_INVALID_ARG (bad args, or ALREADY connected: reconnect is unsupported),
 * KL_ERR_UNSUPPORTED (CONNECTED not granted), KL_ERR_CONNECT (the connect syscall failed: the datagram
 * stays unconnected + usable for destination-addressed sends). */
int kl_datagram_connect(KlDatagram *dg, const KlSockAddr *peer);

/* The KL_DGRAM_RX_* capture mask the socket actually accepted (M0 prep), for validating receive capture
 * (complement of the fail-loud want_rx_caps). 0 if `dg` is not initialized. */
unsigned kl_datagram_accepted_rx_caps(const KlDatagram *dg);

/* Graceful close: stop admission, drain queued output, then retire ops + close the fd once (§4). */
int  kl_datagram_close_begin(KlDatagram *dg);
/* Abortive close: discard queued output, then retire ops + close the fd once (§4). */
int  kl_datagram_close_cancel(KlDatagram *dg);

/* Release object memory AFTER on_close (state CLOSED). REFUSED with -1 before CLOSED. On QUARANTINED
 * the life-token-owned inbound storage is NOT reclaimed here (it stays pinned, fail-closed). */
int  kl_datagram_free(KlDatagram *dg);

/* ── Data plane ───────────────────────────────────────────────────────────────────────────────── */
/* One whole datagram out: atomic accept (payload + metadata COPIED before return) or refusal, never
 * partial, no ownership on refusal. Backpressure is a datagram COUNT (send_slots) under the default
 * SLOT policy (kl_datagram_init); with kl_datagram_init_ex(send_byte_budget > 0) it is count-AND-byte
 * bounded (BOTH). There is no pure byte-only policy. */
KlDatagramSendStatus kl_datagram_send(KlDatagram *dg, const KlDatagramMessage *m);

/* Begin receiving: deliver one datagram per `on_recv` call (serial). */
int  kl_datagram_recv_start(KlDatagram *dg, KlDatagramRecvFn on_recv, void *ud);
void kl_datagram_pause(KlDatagram *dg);   /* strict: completion holds one; readiness drops interest */
int  kl_datagram_resume(KlDatagram *dg);
void kl_datagram_recv_stop(KlDatagram *dg);   /* discard any held datagram + drop interest */

/* ── Edges + terminal result + queries ────────────────────────────────────────────────────────── */
void kl_datagram_on_writable(KlDatagram *dg, KlDatagramWritableFn cb, void *ud);
void kl_datagram_on_drain(KlDatagram *dg, KlDatagramDrainFn cb, void *ud);
void kl_datagram_on_close(KlDatagram *dg, KlDatagramCloseFn cb, void *ud);

unsigned              kl_datagram_caps(const KlDatagram *dg);          /* granted caps (== want_caps) */
unsigned              kl_datagram_provider_caps(const KlDatagram *dg); /* caps the provider supports on the fd */

/* Runtime multicast join/leave (extended-UDP layer). Gated on KL_DGRAM_CAP_MULTICAST. Returns 0, or
 * -1 with kl_datagram_last_error(): KL_ERR_UNSUPPORTED (provider lacks multicast), KL_ERR_INVALID_ARG
 * (malformed group literal), KL_ERR_IO (provider/syscall failure; incl. group/socket family mismatch).
 * The address family is derived from the group literal (dotted-quad → IPv4, colon → IPv6). */
int kl_datagram_multicast_join (KlDatagram *dg, const char *group, unsigned iface_index);
int kl_datagram_multicast_leave(KlDatagram *dg, const char *group, unsigned iface_index);

/* set the socket-DEFAULT outgoing TOS/Traffic-Class byte (IP_TOS / IPV6_TCLASS) applied to every
 * send that carries no per-message tos. Compose with KL_TOS() (keel/datagram.h). Gated on the per-fd
 * TOS capability (KL_DGRAM_CAP_TOS, exact-fd contract). Returns 0, or -1 with kl_datagram_last_error():
 *   - KL_ERR_INVALID_ARG: bad handle / tos ∉ [0,255] / the fd's local family is not IPv4 or IPv6
 *     (undeterminable family: the call refuses rather than guess a socket-option level);
 *   - KL_ERR_UNSUPPORTED: TOS is unavailable on this fd: EITHER the provider exposes no set_tos op OR
 *     it does not advertise KL_DGRAM_CAP_TOS for this fd (checked WITHOUT invoking the provider);
 *   - KL_ERR_SOCKET: getsockname / setsockopt failed.
 * Distinct from the per-message KlDatagramMessage.tos (which overrides this default on a single send). */
int kl_datagram_set_tos(KlDatagram *dg, int tos);

/* the received TOS/Traffic-Class byte of the datagram currently being delivered, or -1 if
 * unavailable (RX_TOS capture not enabled on the socket, or none present). Only meaningful while inside
 * the KlDatagramRecvFn / on_recv_segments callback. Requires the socket to have been prepared with TOS
 * capture (KlDatagramSocketConfig.recv_tos) and that bit carried in KlDatagramConfig.accepted_rx_caps. */
int kl_datagram_recv_tos(const KlDatagram *dg);
KlDgramCloseState     kl_datagram_close_state(const KlDatagram *dg);   /* OPEN/CLOSING/CLOSED */
KlDatagramCloseResult kl_datagram_close_result(const KlDatagram *dg);  /* terminal (NONE until CLOSED) */
size_t   kl_datagram_send_queued(const KlDatagram *dg);         /* occupied send slots (count) */
size_t   kl_datagram_send_queued_bytes(const KlDatagram *dg);  /* Σ queued+in-flight payload bytes */
size_t   kl_datagram_send_inflight(const KlDatagram *dg);
uint64_t kl_datagram_dropped(const KlDatagram *dg);
uint64_t kl_datagram_truncated(const KlDatagram *dg);
KlError  kl_datagram_last_error(const KlDatagram *dg);
KlSocketHandle kl_datagram_fd(const KlDatagram *dg);          /* the adopted fd, or KL_INVALID_SOCKET */
uint16_t       kl_datagram_local_port(const KlDatagram *dg);  /* bound local port, or 0 (via the socket seam) */

#ifdef __cplusplus
}
#endif

#endif /* KEEL_DATAGRAM_H */
