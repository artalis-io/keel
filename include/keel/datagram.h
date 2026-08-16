#ifndef KEEL_DATAGRAM_H
#define KEEL_DATAGRAM_H

/*
 * datagram.h — the public fixed-slot KlDatagram API (Phase B, step 7B-3).
 *
 * KlDatagram is the Tier-1 datagram primitive: a caller-owned, single-threaded, event-loop-driven
 * handle over a prepared UDP fd. It is a thin facade over the internal KlDgramCore assembly (7A) and is
 * exposed + validated INDEPENDENTLY of the legacy KlUdp byte-budget surface (D-COMPAT, §6): KlUdp is
 * NOT re-based onto fixed slots and keeps its exact semantics. The two coexist permanently.
 *
 * This header is also the single home for the public datagram data TYPES (KlDatagramMessage,
 * KlDatagramSendStatus, KlDatagramCloseResult, KlDgramCloseState, KL_DGRAM_CAP_*, KL_DGRAM_HAS_LOCAL/
 * TRUNCATED); the internal machine headers (src/datagram_*.h) include this header for them.
 *
 * STABILITY (STABLE as of 7B-10). The kl_datagram_* FUNCTION + TYPE CONTRACT is STABLE — validated LIVE
 * across every supported event backend: the completion loops pollcomp (7B-4), io_uring (7B-5), Windows
 * IOCP (7B-7), raw lwIP-raw (7B-8), and firmware EFI_UDP4 (7B-9), plus the readiness loops POSIX
 * epoll/kqueue/poll (7B-6) and Windows WSAPoll — each §10-validated (docs/datagram_contract.md). The
 * struct LAYOUT is NOT ABI-stable: a consumer that stack-/embed-allocates a KlDatagram opts into the
 * layout by including <keel/datagram_detail.h> and MUST recompile when it changes. A consumer that only
 * holds a `KlDatagram *` (created behind the API) is insulated from layout.
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

/* ── Send status: atomic accept-or-refuse, no ownership on refusal (invariant 4) ──────────────── */
typedef enum {
    KL_DATAGRAM_ACCEPTED = 0,   /* whole datagram taken (sent or slot-queued) */
    KL_DATAGRAM_WOULD_BLOCK,    /* transient: no free outbound slot; nothing taken */
    KL_DATAGRAM_TOO_LARGE,      /* permanent: datagram exceeds one slot; nothing taken */
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
    KL_DGRAM_DETACHED,         /* CONFIRMED physical retirement of every op — the strong guarantee */
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
    unsigned                 want_caps;     /* KL_DGRAM_CAP_* the consumer requires */
} KlDatagramConfig;

/* ── Lifecycle ────────────────────────────────────────────────────────────────────────────────── */
/* Initialize `dg` (a memset-zero handle) over the prepared fd. fd OWNERSHIP TRANSFERS to KlDatagram
 * ONLY when this returns 0; on failure nothing the caller must reclaim is touched (the caller retains
 * the fd). Returns 0, or -1 (kl_datagram_last_error carries why). */
int  kl_datagram_init(KlDatagram *dg, const KlDatagramConfig *cfg);

/* Graceful close: stop admission, drain queued output, then retire ops + close the fd once (§4). */
int  kl_datagram_close_begin(KlDatagram *dg);
/* Abortive close: discard queued output, then retire ops + close the fd once (§4). */
int  kl_datagram_close_cancel(KlDatagram *dg);

/* Release object memory AFTER on_close (state CLOSED). REFUSED with -1 before CLOSED. On QUARANTINED
 * the life-token-owned inbound storage is NOT reclaimed here (it stays pinned, fail-closed). */
int  kl_datagram_free(KlDatagram *dg);

/* ── Data plane ───────────────────────────────────────────────────────────────────────────────── */
/* One whole datagram out: atomic accept (payload + metadata COPIED before return) or refusal — never
 * partial, no ownership on refusal. Backpressure is a datagram COUNT (send_slots), NOT a byte budget. */
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

unsigned              kl_datagram_caps(const KlDatagram *dg);          /* KL_DGRAM_CAP_* */
KlDgramCloseState     kl_datagram_close_state(const KlDatagram *dg);   /* OPEN/CLOSING/CLOSED */
KlDatagramCloseResult kl_datagram_close_result(const KlDatagram *dg);  /* terminal (NONE until CLOSED) */
size_t   kl_datagram_send_queued(const KlDatagram *dg);
size_t   kl_datagram_send_inflight(const KlDatagram *dg);
uint64_t kl_datagram_dropped(const KlDatagram *dg);
uint64_t kl_datagram_truncated(const KlDatagram *dg);
KlError  kl_datagram_last_error(const KlDatagram *dg);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_DATAGRAM_H */
