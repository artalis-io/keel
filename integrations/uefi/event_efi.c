/*
 * event_efi.c — completion-axis KlEventProvider over EFI_TCP4 tokens (U-3, F-8).
 * See event_efi.h for the design. This is the direct real-firmware analogue of the
 * mock completion loop in tests/freestanding_harness.c: connect rides the completion
 * axis (KL_COMP_CONNECT), send/recv ride the U-2 socket provider's SYNC ops driven by
 * an emulated-readiness watcher relay (KL_COMP_WATCHER). No libc, no errno.
 */

#include "event_efi.h"
#include "socket_efi_tcp4.h"          /* kl_uefi_socket_provider + async-connect prims */
#include "platform_uefi.h"            /* kl_uefi_after_ebs (F3 boot-services guard) */

#include <keel/event.h>
#include <keel/sockaddr.h>
#include <keel/connection.h>           /* KlConn — server post_recv/post_send targets (S-4) */
#include "../../src/socket.h"          /* KlSocketProvider, kl_sock_accept/send/recv, kl_handle_valid */
#include "../../src/completion.h"      /* KlCompletionOps, KlCompletionEvent, KL_COMP_* */

/* Datagram completion (EFI_UDP4) is an OPTIONAL transport capability, gated by
 * KEEL_UEFI_DATAGRAM: only the datagram builds (6.4c image, the mock harness, the strict
 * two-arch datagram gate) define it. A TCP-only EFI build (U-3/U-4/U-7, S-4/S-6/S-7) compiles
 * NONE of the datagram op storage / pumping / teardown / vtable wiring below, and therefore
 * references NO kl_uefi_udp_* / KlDgramLife symbols — completion is the execution model, UDP
 * is an optional transport, and the two do not link together unless asked. */
#ifdef KEEL_UEFI_DATAGRAM
#include "socket_efi_udp4.h"          /* 6.4b-3b: datagram completion primitives + KlUefiUdpOpResult */
#include "../../src/datagram_life.h"   /* KlDgramLife retain/release — B.6 stable-token transfer */
#endif
#include <keel/server.h>               /* KlServer.pool — accept backpressure (S-3) */

#include <stdint.h>
#include <stddef.h>

/* Bounds match the client's needs: it drives ONE connection (one connect op, a
 * handful of live watches). Small fixed arrays — no allocation in the loop. */
#define KL_EFI_MAX_CONNECT_OPS 8
#define KL_EFI_MAX_WATCHES     8
/* Server completion-native I/O ops (S-4). The accepted-child pool is KL_EFI_MAX_CONNS
 * (8) and the server completion driver keeps at most one recv OR one send outstanding
 * per conn, so 16 slots leave headroom without allocation in the loop. */
#define KL_EFI_MAX_IO_OPS   16
/* Alloc-free server send (S-4/S-7 review): post_send COPIES the whole response into an
 * inline per-op buffer — no heap. A copy (not a reference) is required because the send
 * contract lets the caller free its iovec bytes right after posting: comp_tls_post_encrypted
 * frees the encrypted buffer immediately (pollcomp likewise copies), so referencing it
 * would be a use-after-free. Bounded: a response whose framed size exceeds KL_EFI_SNDBUF is
 * refused (post_send returns -1 -> the conn is closed). 16 KiB comfortably fits the headers
 * + a small body or one TLS record; larger bodies belong on the (future) sendfile/stream
 * path. Firmware BSS cost: KL_EFI_MAX_IO_OPS * KL_EFI_SNDBUF. */
#define KL_EFI_SNDBUF       16384
/* Datagram completion ops (6.4b-3b). DNS drives one Receive + several Transmits per socket; a small
 * fixed pool, no allocation in the loop. Each op holds a B.6 KlDgramLife ref from post until it is
 * transferred to a completion event (DELIVERED), released (RETIRED/STALE_RETIRED), or RETAINED forever
 * (QUARANTINED/INVALID) — see el_drain.
 *
 * SEND SERIALIZATION (review-High): EFI_UDP4 allows only ONE outstanding Transmit token per socket, but
 * KlUdp's completion send lets multiple sends be posted before a drain (the resolver launches A + AAAA
 * synchronously). So the SEND ops form a per-socket PENDING QUEUE at THIS layer: el_post_dgram_send always
 * ACCEPTS (copies the payload+dest — the caller may free them right after), and only ONE send per fd is
 * posted to the substrate at a time; when it retires, the drain pumps the next queued send for that fd.
 * 1 recv + up to a handful of queued sends → 8 slots; a send larger than the inline buffer is refused. */
#ifdef KEEL_UEFI_DATAGRAM
#define KL_EFI_MAX_DGRAM_OPS 8
#define KL_EFI_DGRAM_SNDBUF  1500

typedef enum { EFI_DG_RECV = 0, EFI_DG_SEND = 1 } EfiDgramKind;
typedef struct {
    int                 in_use;
    EfiDgramKind        kind;
    KlSocketHandle      fd;
    unsigned long long  generation;   /* the op identity (captured when POSTED to the substrate) */
    void               *buf;          /* recv: the captured dg->recv_buf (copy target) */
    size_t              buflen;        /* recv: capacity */
    struct KlDgramLife *life;          /* B.6 token ref: retained at post; NULLed on transfer/release */
    /* send-only: the queued payload/dest (copied so it survives the caller freeing them) + state. */
    int                 posted;        /* send: 1 = an EFI Transmit token is outstanding for this op */
    int                 post_failed;   /* send: the deferred substrate post failed → emit ok=0 */
    int                 terminal_emitted; /* recv/send: 7B-9 — a QUARANTINE (borrowed) terminal was already
                                        * emitted for this op; it stays in_use (fail-closed, ref abandoned)
                                        * but must never be re-drained/re-emitted. Gates the drain re-scan. */
    int                 send_cancelled;   /* send: 7B-9 — a synchronous send-cancel was recorded; the drain
                                        * surfaces its terminal from send_cancel_res (poll_send would report
                                        * PENDING forever after the cancel cleared/left tx_posted). */
    KlUefiUdpOpResult   send_cancel_res;  /* send: the recorded cancel result — RETIRED (transfer+release) vs
                                        * QUARANTINED/INVALID (borrowed terminal, retain the ref). */
    unsigned long long  seq;           /* send: monotonic acceptance order → per-socket FIFO pumping */
    unsigned char       snd[KL_EFI_DGRAM_SNDBUF];
    size_t              snd_len;
    KlSockAddr          snd_dest;
} EfiDgramOp;
#endif /* KEEL_UEFI_DATAGRAM */

/* A queued outbound connect (post_connect). Its terminal result is surfaced as a
 * KL_COMP_CONNECT on a later drain (once the Connect token fires), then retired. The
 * captured generation is the stale guard: a completion for a conn that was closed
 * (generation bumped) or whose memory was reused (magic cleared) is dropped. */
typedef struct {
    int             in_use;
    KlSocketHandle  fd;              /* the KlUefiConn handle */
    uint64_t        generation;     /* captured at post time (kl_uefi_conn_generation_h) */
    void           *watcher_udata;  /* the tagged KlWatcher the client registered */
    int             failed;         /* Configure/Connect-post failed → deliver ok=0 */
} EfiConnectOp;

/* A registered tagged watch (add/mod). drain relays its armed mask as a
 * KL_COMP_WATCHER so the client retries the SYNC send/recv on the U-2 provider. */
typedef struct {
    int             in_use;
    KlSocketHandle  fd;
    KlEventMask     mask;
    void           *udata;          /* tagged KlWatcher pointer (LSB=1) */
} EfiWatch;

typedef enum { EFI_IO_RECV = 0, EFI_IO_SEND = 1 } EfiIoOpKind;

/* A posted server-side recv or send on an accepted child (S-4). The client rides the
 * watcher relay; the SERVER completion driver (completion_server.c) posts recv/send as
 * completion-native ops. drain services each via the U-2 SYNC socket provider and
 * surfaces KL_COMP_READ / KL_COMP_WRITE. The captured generation is the stale guard: an
 * op for a child that closed (generation bumped) or whose slot was reused (magic
 * cleared) is dropped, never delivered, mirroring the connect-op discipline. No heap:
 * the send payload is copied into the inline `sndbuf` (see KL_EFI_SNDBUF). */
typedef struct {
    int             in_use;
    EfiIoOpKind     kind;
    KlStream       *stream;          /* completion target (ev->target) — raw transport */
    KlSocketHandle  fd;
    void           *buf;             /* recv only: caller-chosen receive buffer */
    size_t          buf_cap;         /* recv only: capacity */
    uint64_t        generation;      /* captured at post; drain re-checks kl_uefi_conn_valid_h */
    /* send only: an inline copy of the framed response (the caller may free its iovec
     * bytes right after post_send — see KL_EFI_SNDBUF) + transmit progress. */
    unsigned char   sndbuf[KL_EFI_SNDBUF];
    size_t          send_total;
    size_t          send_done;
} EfiIoOp;

typedef struct {
    EFI_BOOT_SERVICES *bs;
    EFI_HANDLE         image;
    int                created;
    EfiConnectOp       connect_ops[KL_EFI_MAX_CONNECT_OPS];
    EfiWatch           watches[KL_EFI_MAX_WATCHES];
    EfiIoOp            io[KL_EFI_MAX_IO_OPS];   /* S-4 server recv/send ops */
#ifdef KEEL_UEFI_DATAGRAM
    EfiDgramOp         dgram[KL_EFI_MAX_DGRAM_OPS];  /* 6.4b-3b datagram recv/send ops */
    unsigned long long dgram_seq;                    /* monotonic send-acceptance counter (FIFO) */
#endif
    /* S-3 server accept: latched by prime_accepts; drain hands back each ready child
     * from the S-2 Accept-token pool as KL_COMP_ACCEPT, with KlConn-pool backpressure. */
    struct KlServer   *server;
    KlSocketHandle     listen_fd;
    int                accept_primed;
} EfiLoop;

static EfiLoop g_efi;

/* ── KlEventOps (readiness face — only tagged watchers register a relay) ────────── */

static int el_init(KlEventLoop *loop) {
    (void)loop;
    /* Preserve bs/image/created (set by kl_uefi_event_provider); clear per-run state.
     * No heap is owned (send iovecs are inline snapshots), so this just marks slots free. */
    for (int i = 0; i < KL_EFI_MAX_CONNECT_OPS; i++) g_efi.connect_ops[i].in_use = 0;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)      g_efi.watches[i].in_use = 0;
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++)       g_efi.io[i].in_use = 0;
#ifdef KEEL_UEFI_DATAGRAM
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++)    g_efi.dgram[i].in_use = 0;
#endif
    return 0;
}

/* Retire one server I/O op. No heap (the send iovec is a fixed array + inline snapshot),
 * so this only marks the slot free. */
static void io_op_free(EfiIoOp *op) { op->in_use = 0; }

static int el_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;
    if (!((uintptr_t)udata & 1)) return 0;   /* untagged (connection) fd — op-driven */
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].udata == udata) {
            g_efi.watches[i].mask = mask; g_efi.watches[i].fd = fd; return 0;
        }
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (!g_efi.watches[i].in_use) {
            g_efi.watches[i].in_use = 1;
            g_efi.watches[i].fd = fd;
            g_efi.watches[i].mask = mask;
            g_efi.watches[i].udata = udata;
            return 0;
        }
    return -1;
}

static int el_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].udata == udata) {
            g_efi.watches[i].mask = mask; g_efi.watches[i].fd = fd; return 0;
        }
    return el_add(loop, fd, mask, udata);
}

static int el_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].fd == fd)
            g_efi.watches[i].in_use = 0;
    return 0;
}

static int el_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* Completion loops drive via kl_comp_run/drain, not this readiness wait. */
    (void)loop; (void)out; (void)max; (void)timeout_ms; return 0;
}
/* el_close (S-7 review): retire ALL outstanding records so nothing dangles after the
 * event ctx is freed (kl_event_ctx_free calls this before the conn pool is freed in
 * kl_server_free). No heap is owned now (server send iovecs are inline snapshots — no
 * kl_malloc), so this clears state only: connect ops, watches, server I/O ops, and the
 * latched server/listener. Prevents a stale KlConn / watcher pointer from surviving into
 * a later run and closes the (former) leak window a pending send buffer could open. */
static void el_close(KlEventLoop *loop) {
    (void)loop;
    for (int i = 0; i < KL_EFI_MAX_CONNECT_OPS; i++) g_efi.connect_ops[i].in_use = 0;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)      g_efi.watches[i].in_use = 0;
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++)       g_efi.io[i].in_use = 0;
#ifdef KEEL_UEFI_DATAGRAM
    /* A still-in-flight datagram op at ctx teardown: consult the substrate to decide life-release,
     * so an ORDINARY teardown (sockets already cleanly closed → STALE_RETIRED) RELEASES the ref and
     * on_final frees the receive storage — rather than leaking it. Only an unconfirmed op (QUARANTINED,
     * INVALID, or a still-live/PENDING posted op we cannot reconcile here) RETAINS the ref. A send op
     * never posted to the substrate (queued / post-failed) touched no firmware → release it. */
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++) {
        EfiDgramOp *op = &g_efi.dgram[i];
        if (op->in_use && op->life) {
            KlUefiUdpOpResult st = (op->kind == EFI_DG_SEND && !op->posted)
                ? KL_UEFI_UDP_OP_STALE_RETIRED             /* never posted → no firmware ref → release */
                : kl_uefi_udp_op_state(op->fd, op->generation);
            if (st == KL_UEFI_UDP_OP_STALE_RETIRED || st == KL_UEFI_UDP_OP_RETIRED)
                kl_dgram_life_release(op->life);           /* confirmed retirement → release */
            /* else QUARANTINED / INVALID / PENDING → retain (abandon the ref) */
            op->life = NULL;
        }
        op->in_use = 0;
    }
#endif /* KEEL_UEFI_DATAGRAM */
    g_efi.server        = NULL;
    g_efi.listen_fd     = KL_INVALID_SOCKET;
    g_efi.accept_primed = 0;
}
static unsigned el_caps(const KlEventLoop *loop) { (void)loop; return KL_EVENT_CAP_COMPLETION; }

static const struct KlSocketProvider *el_native_provider(const KlEventLoop *loop) {
    (void)loop;
    /* Lazily create the U-2 socket provider over the stashed bs/image (single
     * instance — returns the same provider once created). */
    return kl_uefi_socket_provider(g_efi.bs, g_efi.image);
}

/* ── KlCompletionOps ────────────────────────────────────────────────────────────── */

static int el_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                           const KlSockAddr *addr, void *watcher_udata) {
    (void)ctx;
    EfiConnectOp *op = NULL;
    for (int i = 0; i < KL_EFI_MAX_CONNECT_OPS; i++)
        if (!g_efi.connect_ops[i].in_use) { op = &g_efi.connect_ops[i]; break; }
    if (!op) return -1;   /* no slot */

    op->in_use = 1;
    op->fd = fd;
    op->generation = (uint64_t)kl_uefi_conn_generation_h(fd);
    op->watcher_udata = watcher_udata;
    op->failed = 0;

    /* Configure (active/DHCP/remote) then issue the Connect token — both non-blocking
     * beyond the bounded DHCP settle. Any failure is recorded and delivered as an
     * ok=0 KL_COMP_CONNECT on the next drain (the completion model always reports the
     * connect result through the loop, never as a post-time error). */
    if (kl_uefi_socket_configure(fd, addr) != 0 || kl_uefi_socket_connect_post(fd) != 0)
        op->failed = 1;
    return 0;
}

static void el_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    (void)ctx;
    /* Drop any queued connect op on this fd so a stale KL_COMP_CONNECT cannot fire at
     * the (about-to-be-freed) watcher. The socket close's generation bump is the
     * second line of defense (drain re-checks kl_uefi_conn_valid_h). */
    for (int i = 0; i < KL_EFI_MAX_CONNECT_OPS; i++)
        if (g_efi.connect_ops[i].in_use && g_efi.connect_ops[i].fd == fd)
            g_efi.connect_ops[i].in_use = 0;
    /* Also drop any watches on the fd (the client is done with it). */
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].fd == fd)
            g_efi.watches[i].in_use = 0;
    /* Drop any posted server recv/send ops on the fd so a completion cannot fire at an
     * about-to-be-freed conn (S-4). No heap to release (inline snapshots). */
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++)
        if (g_efi.io[i].in_use && g_efi.io[i].fd == fd)
            io_op_free(&g_efi.io[i]);
}

/* prime_accepts: latch the server + its passive listen fd so drain can hand back
 * accepted children. The S-2 listen() already armed the Accept-token pool; this only
 * records what drain needs. Idempotent. Returns 0 = AUTONOMOUS accept model (6B-3 2b-ii):
 * EFI generates accepts inside drain under its own capacity gate (below), so NO completion
 * KlListener is installed and the server re-calls this each tick to top the gate up. */
static int el_prime_accepts(struct KlServer *s) {
    if (!s) return -1;
    g_efi.server = s;
    g_efi.listen_fd = s->listen_fd;
    g_efi.accept_primed = 1;
    /* Capacity-gated arming (Goal 8 backpressure): arm at most as many EFI Accept tokens
     * as there are FREE Keel pool slots. Runs every completion tick (this is called from
     * kl_io_engine_run_completion), so a freed slot (active_count drops on conn release)
     * re-arms on the next tick — and a full pool arms nothing, so EFI stops accepting
     * into child handles Keel can't service (they wait in the TCP backlog). */
    if (kl_handle_valid(s->listen_fd)) {
        int freeslots = s->pool.capacity - s->pool.active_count;
        if (freeslots < 0) freeslots = 0;
        (void)kl_uefi_socket_accept_arm(s->listen_fd, freeslots);
    }
    return 0;
}

/* post_accept: re-arm after the generic server consumed a slot. Identical to prime — the
 * arming is capacity-gated, so this just re-evaluates free capacity and tops up. */
static int el_post_accept(struct KlServer *s) {
    return el_prime_accepts(s);
}

static EfiIoOp *io_op_alloc(void) {
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++)
        if (!g_efi.io[i].in_use) return &g_efi.io[i];
    return NULL;
}

/* post_recv (S-4): queue a raw server-side receive on an accepted child into the
 * caller-supplied `buf` (drain does the actual recv, unchanged between post and completion —
 * the conn parks with no other op), surfaced as KL_COMP_READ. No TLS/state knowledge — the
 * HTTP adapter chose the buffer. */
static int el_post_recv(KlStream *stream, void *buf, size_t cap) {
    if (!stream || !buf || cap == 0) return -1;
    EfiIoOp *op = io_op_alloc();
    if (!op) return -1;
    for (size_t b = 0; b < sizeof(*op); b++) ((unsigned char *)op)[b] = 0;
    op->kind       = EFI_IO_RECV;
    op->stream     = stream;
    op->fd         = stream->fd;
    op->buf        = buf;
    op->buf_cap    = cap;
    op->generation = (uint64_t)kl_uefi_conn_generation_h(stream->fd);
    op->in_use     = 1;   /* set last */
    return 0;
}

/* post_send (S-4, alloc-free per the S-7 review): COPY the framed response into the op's
 * inline sndbuf. A copy is mandatory, not an optimization to avoid — the send contract
 * lets the caller free its iovec bytes right after posting (comp_tls_post_encrypted frees
 * the encrypted buffer immediately), so a reference would be a use-after-free. No heap;
 * bounded by KL_EFI_SNDBUF (a larger framed response is refused -> conn closed). drain
 * transmits sndbuf in bounded EFI Transmit fragments and surfaces one KL_COMP_WRITE. */
static int el_post_send(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total) {
    if (!stream || iovcnt < 0 || (iovcnt > 0 && !iov)) return -1;
    if (total > KL_EFI_SNDBUF) return -1;   /* response too large for the inline buffer */
    EfiIoOp *op = io_op_alloc();
    if (!op) return -1;
    for (size_t b = 0; b < sizeof(*op); b++) ((unsigned char *)op)[b] = 0;
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        /* Overflow-safe bound (KL_EFI_SNDBUF - off never underflows: off <= KL_EFI_SNDBUF
         * is the loop invariant). op still !in_use on the early return — nothing to retire. */
        if (iov[i].len > KL_EFI_SNDBUF - off) return -1;
        for (size_t j = 0; j < iov[i].len; j++)
            op->sndbuf[off + j] = ((const unsigned char *)iov[i].base)[j];
        off += iov[i].len;
    }
    if (off != total) return -1;   /* declared total must equal the bytes actually framed */
    op->kind       = EFI_IO_SEND;
    op->stream     = stream;
    op->fd         = stream->fd;
    op->generation = (uint64_t)kl_uefi_conn_generation_h(stream->fd);
    op->send_total = off;
    op->send_done  = 0;
    op->in_use     = 1;        /* set last: a mid-loop return -1 leaves the slot free */
    return 0;
}

#ifdef KEEL_UEFI_DATAGRAM
/* ── Datagram completion ops (6.4b-3b) — post an EFI_UDP4 Receive/Transmit token, retain a B.6
 *    life ref, and capture the op identity {fd, generation}; el_drain reaps via KlUefiUdpOpResult. */
static EfiDgramOp *dgram_op_alloc(void) {
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++)
        if (!g_efi.dgram[i].in_use) return &g_efi.dgram[i];
    return NULL;
}

static int el_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *rop) {
    (void)ctx;                          /* EFI reaches its substrate via file-scope g_efi, not the ctx */
    if (!rop) return -1;
    EfiDgramOp *op = dgram_op_alloc();
    if (!op) return -1;                 /* nothing taken → caller releases its retained token ref */
    for (size_t b = 0; b < sizeof(*op); b++) ((unsigned char *)op)[b] = 0;
    op->kind   = EFI_DG_RECV;
    op->fd     = rop->fd;
    op->buf    = rop->buf;              /* copy target — captured now; the token ref pins it */
    op->buflen = rop->cap;
    if (kl_uefi_udp_post_recv(rop->fd) != 0) return -1;  /* op still !in_use → nothing to retire */
    op->generation = kl_uefi_udp_generation_h(rop->fd);  /* the live slot's generation (op identity) */
    op->life = rop->life;               /* TRANSFERRED into the op (no retain — the caller retained) */
    op->in_use = 1;   /* set last */
    return 0;
}

/* Post the next QUEUED (accepted-but-unposted) send for @fd to the substrate, IFF no Transmit token is
 * currently outstanding for @fd (EFI allows one at a time). A substrate post failure flags the op so the
 * drain emits an ok=0 completion. Called from el_post_dgram_send (fast path) and after each send retires. */
static void efi_dgram_pump_sends(KlSocketHandle fd) {
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++)   /* a send already in flight on this fd? then wait */
        if (g_efi.dgram[i].in_use && g_efi.dgram[i].kind == EFI_DG_SEND &&
            g_efi.dgram[i].fd == fd && g_efi.dgram[i].posted)
            return;
    /* FIFO: choose the OLDEST unposted send for @fd by acceptance sequence — NOT the lowest array slot,
     * which would let a newly accepted send reuse a freed hole and jump ahead of older queued sends. */
    EfiDgramOp *op = NULL;
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++) {
        EfiDgramOp *c = &g_efi.dgram[i];
        if (!c->in_use || c->kind != EFI_DG_SEND || c->fd != fd || c->posted || c->post_failed) continue;
        if (!op || c->seq < op->seq) op = c;
    }
    if (op) {
        if (kl_uefi_udp_post_send(fd, op->snd, op->snd_len, &op->snd_dest) == 0) {
            op->generation = kl_uefi_udp_generation_h(fd);   /* op identity captured at the real post */
            op->posted = 1;
        } else {
            op->post_failed = 1;   /* real failure (not tx-busy — we checked) → drain reports ok=0 */
        }
        return;   /* one at a time */
    }
}

static int el_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *sop) {
    (void)ctx;                          /* EFI reaches its substrate via file-scope g_efi, not the ctx */
    if (!sop || sop->len > KL_EFI_DGRAM_SNDBUF) return -1;
    EfiDgramOp *op = dgram_op_alloc();
    if (!op) return -1;                 /* nothing taken → caller releases its ref */
    for (size_t b = 0; b < sizeof(*op); b++) ((unsigned char *)op)[b] = 0;
    op->kind = EFI_DG_SEND;
    op->fd   = sop->fd;
    /* COPY the payload + dest now — the caller may free them right after this returns. The op is queued
     * and posted to the substrate one-at-a-time by efi_dgram_pump_sends (EFI: one Tx token per socket). */
    for (size_t b = 0; b < sop->len; b++) op->snd[b] = ((const unsigned char *)sop->data)[b];
    op->snd_len = sop->len;
    if (sop->dest) op->snd_dest = *sop->dest;
    op->seq  = g_efi.dgram_seq++;         /* monotonic acceptance order → per-socket FIFO */
    op->life = sop->life;                 /* TRANSFERRED into the op (no retain) */
    op->in_use = 1;                       /* accepted (queued) */
    efi_dgram_pump_sends(sop->fd);        /* post it now if the socket's Tx token is free */
    return 0;
}

/* Cancel the outstanding datagram op(s) of `kind` for `life` (7B-2): request the firmware Cancel on the
 * matching POSTED op (a queued/unposted send touched no firmware — nothing to cancel). The return is
 * IGNORED: ref release stays with the drain/el_close op_state classifier (never here), so cancel is
 * idempotent + does not double-release. Confirmed retirement then surfaces via retire_dgram. */
static int el_cancel_dgram(struct KlEventCtx *ctx, KlDgramLife *life, KlDgramOpKind kind) {
    (void)ctx;
    EfiDgramKind want = (kind == KL_DGRAM_OP_SEND) ? EFI_DG_SEND : EFI_DG_RECV;
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++) {
        EfiDgramOp *op = &g_efi.dgram[i];
        if (!op->in_use || op->life != life || op->kind != want) continue;
        if (want == EFI_DG_SEND) {
            /* 7B-9: RECORD the synchronous cancel result. A posted Transmit is Cancel+drained (RETIRED =
             * confirmed, QUARANTINED = unconfirmed); a queued (never-posted) send touched no firmware, so
             * it retires immediately (RETIRED). Either way el_drain surfaces a KL_COMP_DGRAM_SEND terminal
             * so the send MACHINE retires (send_inflight → 0) — WITHOUT it an abortive close can never
             * reach backend_close (close_send_drained gates it), and poll_send stays PENDING forever after
             * the cancel. Idempotent: skip an op that already recorded / already emitted its terminal. */
            if (!op->send_cancelled && !op->terminal_emitted && !op->post_failed) {
                op->send_cancel_res = op->posted
                    ? kl_uefi_udp_cancel_send(op->fd, op->generation)
                    : KL_UEFI_UDP_OP_RETIRED;   /* unposted: nothing on the wire → confirmed retired */
                op->send_cancelled = 1;
            }
        } else {
            /* Cancel + drain the recv token. This retires the SUBSTRATE op (rx_posted→0) but NOT the
             * recv MACHINE — recv_inflight only reaches 0 once el_drain surfaces a KL_COMP_DGRAM_RECV
             * terminal (7B-9). After backend_close bumps the generation, el_drain observes the op as
             * STALE_RETIRED (clean) or QUARANTINED (unconfirmed) and emits that terminal. */
            (void)kl_uefi_udp_cancel_recv(op->fd, op->generation);
        }
    }
    return 0;
}

/* Classify retirement (§4.3, 7B-2) — the EFI-distinctive path. Mirrors el_close's per-op decision: a
 * never-posted send touched no firmware → RETIRED; else consult the substrate — STALE_RETIRED/RETIRED →
 * RETIRED, PENDING/DELIVERED (live, not yet reaped) → PENDING, QUARANTINED/INVALID (unconfirmed) →
 * QUARANTINED (fail-closed, the override no other backend needs). No matching op → already retired. */
static KlDgramRetireResult el_retire_dgram(struct KlEventCtx *ctx, KlDgramLife *life,
                                           KlDgramOpKind kind, int *transport_err) {
    (void)ctx;
    if (transport_err) *transport_err = 0;
    EfiDgramKind want = (kind == KL_DGRAM_OP_SEND) ? EFI_DG_SEND : EFI_DG_RECV;
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++) {
        EfiDgramOp *op = &g_efi.dgram[i];
        if (!op->in_use || op->life != life || op->kind != want) continue;
        KlUefiUdpOpResult st = (want == EFI_DG_SEND && !op->posted)
            ? KL_UEFI_UDP_OP_STALE_RETIRED
            : kl_uefi_udp_op_state(op->fd, op->generation);
        if (st == KL_UEFI_UDP_OP_STALE_RETIRED || st == KL_UEFI_UDP_OP_RETIRED)
            return KL_DGRAM_RETIRE_RETIRED;
        if (st == KL_UEFI_UDP_OP_PENDING || st == KL_UEFI_UDP_OP_DELIVERED)
            return KL_DGRAM_RETIRE_PENDING;
        return KL_DGRAM_RETIRE_QUARANTINED;   /* QUARANTINED / INVALID → fail-closed */
    }
    return KL_DGRAM_RETIRE_RETIRED;
}
#endif /* KEEL_UEFI_DATAGRAM */

/* drain: surface completed Connect tokens (stale-guarded) as KL_COMP_CONNECT, then
 * relay each armed watch as a KL_COMP_WATCHER (level-triggered — the client re-arms
 * while it still needs to send/recv). If nothing fired, Stall briefly so the firmware
 * keeps ticking without a 100%-CPU spin. */
static int el_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    (void)timeout_ms;
    /* F3: once boot services are gone, the EFI_TCP4 Poll/CheckEvent/connect_poll paths
     * are all invalid — stop driving the loop (fail-closed, no firmware calls). */
    if (kl_uefi_after_ebs()) return 0;
    int count = 0;

    /* Connect completions — one terminal edge each, then retire the op. */
    for (int i = 0; i < KL_EFI_MAX_CONNECT_OPS && count < max; i++) {
        EfiConnectOp *op = &g_efi.connect_ops[i];
        if (!op->in_use) continue;

        int ok = 0;
        if (op->failed) {
            ok = 0;                                    /* Configure/post failed at post time */
        } else if (!kl_uefi_conn_valid_h(op->fd, op->generation)) {
            op->in_use = 0; continue;                  /* stale (closed / reused) — drop */
        } else {
            int connected = 0;
            int r = kl_uefi_socket_connect_poll(op->fd, &connected);
            if (r == 0) continue;                      /* still pending — keep pumping */
            ok = (r == 1 && connected);                /* r<0 → ok=0 */
        }

        for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
        out[count].kind   = KL_COMP_CONNECT;
        out[count].target = op->watcher_udata;
        out[count].bytes  = ok ? (size_t)KL_EVENT_WRITE : 0;
        out[count].ok     = ok;
        count++;
        op->in_use = 0;   /* retire: exactly-once terminal result */
    }

    /* Drain-driven readiness relay (U-6): for each armed watch, emit a
     * KL_COMP_WATCHER ONLY when it is actually ready. WRITE stays always-ready —
     * send is a short synchronous Transmit. READ is relayed only when the
     * provider's non-blocking recv can return something now
     * (kl_uefi_socket_recv_ready posts/polls a Receive without pumping) — no more
     * busy-relay that spun a blocking recv, and the loop is never stalled for a
     * whole recv (timers fire, other ops interleave). */
    for (int i = 0; i < KL_EFI_MAX_WATCHES && count < max; i++) {
        if (!g_efi.watches[i].in_use) continue;
        KlEventMask mask = g_efi.watches[i].mask;
        int ready = 0;
        if (mask & KL_EVENT_WRITE) ready |= KL_EVENT_WRITE;
        if ((mask & KL_EVENT_READ) && kl_uefi_socket_recv_ready(g_efi.watches[i].fd))
            ready |= KL_EVENT_READ;
        if (ready == 0) continue;   /* nothing ready → don't relay (no spin) */
        for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
        out[count].kind   = KL_COMP_WATCHER;
        out[count].target = g_efi.watches[i].udata;
        out[count].bytes  = (size_t)ready;
        out[count].ok     = 1;
        count++;
    }

    /* Accept relay (S-3): hand back each child the S-2 Accept-token pool has ready as
     * KL_COMP_ACCEPT (peer already neutralized by efi_sock_accept via GetModeData).
     * Backpressure — stop when the server's KlConn pool is full (don't accept into a
     * drop; the completion analogue of reducing readiness interest, Goal 8). The
     * socket-axis efi_sock_accept re-arms each consumed Accept token internally. */
    if (g_efi.accept_primed && g_efi.server && kl_handle_valid(g_efi.listen_fd)) {
        while (count < max && g_efi.server->pool.free_list) {
            KlSockAddr peer;
            for (size_t b = 0; b < sizeof(peer); b++) ((unsigned char *)&peer)[b] = 0;
            KlSocketHandle a = kl_sock_accept(ctx->sockets, g_efi.listen_fd, &peer);
            if (!kl_handle_valid(a)) break;   /* none ready (would-block) */
            for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
            out[count].kind        = KL_COMP_ACCEPT;
            out[count].target      = NULL;   /* ACCEPT: server recovered from ctx at dispatch (6B-3) */
            out[count].ok          = 1;
            out[count].accepted_fd = a;
            out[count].peer        = peer;
            count++;
        }
    }

    /* Server I/O ops (S-4): service each posted recv/send on an accepted child via the
     * U-2 SYNC socket provider and surface KL_COMP_READ / KL_COMP_WRITE. Stale-guarded
     * (a child that closed mid-op is dropped, never delivered). RECV is gated on the
     * non-blocking readiness probe (no blocking recv); SEND transmits synchronously in
     * bounded fragments (the U-2 model: a short Transmit), leaving the op pending on a
     * would-block to retry next drain. */
    for (int i = 0; i < KL_EFI_MAX_IO_OPS && count < max; i++) {
        EfiIoOp *op = &g_efi.io[i];
        if (!op->in_use) continue;

        if (!kl_uefi_conn_valid_h(op->fd, op->generation)) {
            io_op_free(op);        /* stale (closed / slot reused) — drop, no event */
            continue;
        }

        if (op->kind == EFI_IO_RECV) {
            /* RECV: only when the provider can return something now. Raw recv into the
             * caller-supplied buffer — no TLS/state knowledge. For a TLS conn that buffer is
             * the HTTP adapter's ciphertext scratch, which comp_on_read feeds to the engine. */
            if (!kl_uefi_socket_recv_ready(op->fd)) continue;
            ssize_t n = kl_sock_recv(ctx->sockets, op->fd, op->buf, op->buf_cap);
            for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
            out[count].kind   = KL_COMP_READ;
            out[count].target = op->stream;
            out[count].bytes  = (n > 0) ? (size_t)n : 0;  /* 0 / <0 → peer closed; driver closes */
            out[count].ok     = (n > 0) ? 1 : 0;
            count++;
            io_op_free(op);
            continue;
        }

        /* SEND: transmit the remainder of sndbuf synchronously (bounded EFI fragments),
         * from wherever the last drain left off (send_done). */
        int fatal = 0, wouldblock = 0;
        while (op->send_done < op->send_total) {
            ssize_t n = kl_sock_send(ctx->sockets, op->fd,
                                     op->sndbuf + op->send_done,
                                     op->send_total - op->send_done);
            if (n > 0) { op->send_done += (size_t)n; continue; }
            KlIoStatus st = kl_sock_io_status(ctx->sockets);
            if (st == KL_IO_WOULD_BLOCK || st == KL_IO_INTERRUPTED) { wouldblock = 1; break; }
            fatal = 1; break;
        }
        if (wouldblock) continue;   /* retry the remainder on a later drain */
        for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
        out[count].kind   = KL_COMP_WRITE;
        out[count].target = op->stream;
        out[count].bytes  = fatal ? 0 : op->send_total;
        out[count].ok     = fatal ? 0 : 1;
        count++;
        io_op_free(op);
    }

#ifdef KEEL_UEFI_DATAGRAM
    /* Datagram ops (6.4b-3b): poll each posted Receive/Transmit by its {fd, captured_generation}
     * identity and apply the KlUefiUdpOpResult contract. Every recv terminal (7B-9) emits a
     * KL_COMP_DGRAM_RECV so the recv MACHINE retires (recv_inflight → 0 — required by the
     * confirmed-detachment close for EVERY outcome, not just a delivery):
     *   DELIVERED / STALE_RETIRED → emit + TRANSFER the B.6 life ref (dispatch releases it, retain_life=0;
     *     the final release runs on_final). STALE_RETIRED is the close path (child cancel-drained + cleanly
     *     closed by backend_close before we polled) → ok=0, no delivery.
     *   QUARANTINED / INVALID → emit a BORROWED terminal (retain_life=1: no site releases it; op stays
     *     in_use, abandoned) so recv_inflight retires but on_final never frees storage the firmware may
     *     still touch. Both are unconfirmed/fail-safe → same handling (retire the machine, keep the ref +
     *     classification).
     * SEND terminals are SYMMETRIC (7B-9). A normal completion (poll_send DELIVERED/STALE_RETIRED) or a
     * recorded synchronous cancel result (send_cancelled: RETIRED) TRANSFERS the ref + retires the record;
     * an unconfirmed result (QUARANTINED/INVALID, whether from poll_send or a recorded cancel) emits a
     * BORROWED terminal (retire the send machine, keep the ref + record). The recorded-cancel path is the
     * abortive-close case: poll_send stays PENDING after a cancel, and backend_close is gated behind the
     * send retiring, so the terminal MUST come from the recorded result. The backend never inspects owner
     * liveness — a live-but-owner-dead delivery is dropped by generic dispatch via ev->life. */
    for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS && count < max; i++) {
        EfiDgramOp *op = &g_efi.dgram[i];
        if (!op->in_use) continue;
        /* 7B-9: a borrowed QUARANTINE terminal (recv OR send) was already surfaced for this op; it stays
         * in_use (abandoned, ref retained) so retire_dgram keeps reporting QUARANTINED at close-join.
         * Never re-drain/re-emit. */
        if (op->terminal_emitted) continue;

        if (op->kind == EFI_DG_RECV) {
            KlSockAddr peer, local; int trunc = 0; size_t nb = 0; int ok = 0;
            for (size_t b = 0; b < sizeof(peer); b++) { ((unsigned char *)&peer)[b] = 0; ((unsigned char *)&local)[b] = 0; }
            KlUefiUdpOpResult r = kl_uefi_udp_poll_recv(op->fd, op->generation, op->buf, op->buflen,
                                                        &peer, &local, &trunc, &nb, &ok);
            if (r == KL_UEFI_UDP_OP_PENDING) continue;
            if (r == KL_UEFI_UDP_OP_DELIVERED || r == KL_UEFI_UDP_OP_STALE_RETIRED) {
                /* A terminal that RETIRES the recv machine (recv_inflight → 0). DELIVERED may carry a
                 * real datagram (ok=1) or a signalled-aborted token (ok=0); STALE_RETIRED (7B-9) is the
                 * confirmed-detachment close path — the child was cancel-drained AND cleanly closed by
                 * backend_close (generation bumped) BEFORE we polled, so there is nothing to deliver
                 * (ok=0). Both TRANSFER the ref op → event; dispatch releases it (retain_life=0), whose
                 * final release runs on_final. */
                int delivered = (r == KL_UEFI_UDP_OP_DELIVERED);
                for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
                out[count].kind      = KL_COMP_DGRAM_RECV;
                out[count].life      = op->life; op->life = NULL;   /* TRANSFER ref op → event */
                out[count].ok        = delivered ? ok : 0;
                out[count].bytes     = delivered ? nb : 0;
                out[count].buf       = op->buf;
                if (delivered) {
                    if (kl_sockaddr_family(&peer)  != KL_AF_UNSPEC) out[count].peer  = peer;
                    if (kl_sockaddr_family(&local) != KL_AF_UNSPEC) out[count].local = local;
                    out[count].truncated = trunc;
                }
                count++;
                op->in_use = 0;   /* retire the record (life transferred) */
            } else {   /* QUARANTINED or INVALID — both UNCONFIRMED / fail-safe (7B-9) */
                /* QUARANTINE terminal: retire the recv MACHINE (recv_inflight → 0 so the
                 * confirmed-detachment close can classify) WITHOUT releasing the life ref — the
                 * abandoned firmware RxToken may still write op->buf, so on_final must never run and free
                 * that storage. BORROW the ref (retain_life=1: neither the router nor dispatch releases
                 * it; op->life STAYS set), KEEP the record in_use (retire_dgram keeps reporting
                 * QUARANTINED at join), and gate re-emission with terminal_emitted. ok=0 → no delivery.
                 * INVALID (unreachable for a posted op) is treated identically — it is explicitly an
                 * unconfirmed/fail-safe result, so it too must retire the machine without releasing the
                 * (protected) ref and must preserve its classification, not silently drop the record. */
                for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
                out[count].kind        = KL_COMP_DGRAM_RECV;
                out[count].life        = op->life;   /* BORROW — do NOT NULL op->life */
                out[count].retain_life = 1;
                out[count].ok          = 0;
                out[count].buf         = op->buf;
                count++;
                op->terminal_emitted = 1;   /* keep in_use (abandoned) — never re-drained/re-emitted */
            }
        } else {   /* EFI_DG_SEND */
            KlSocketHandle sfd = op->fd;
            if (op->post_failed) {   /* the deferred substrate post failed → emit a failed completion */
                for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
                out[count].kind = KL_COMP_DGRAM_SEND;
                out[count].life = op->life; op->life = NULL;   /* TRANSFER ref op → event */
                out[count].ok   = 0;
                /* Release the FULL q_bytes reservation (udp.c reserved snd_len at post, releases
                 * ev->bytes) even on failure — else a failed send permanently inflates the send queue. */
                out[count].bytes = op->snd_len;
                count++;
                op->in_use = 0;
                efi_dgram_pump_sends(sfd);   /* let a queued send take the freed Tx token */
                continue;
            }
            /* 7B-9: a recorded synchronous send-cancel (abortive close). poll_send would report PENDING
             * (the cancel left/cleared tx_posted and the slot is not yet stale — backend_close is GATED
             * behind this send retiring), so surface the terminal from the recorded result instead so the
             * send MACHINE retires (send_inflight → 0) and the abortive close can reach backend_close. */
            if (op->send_cancelled) {
                int quar = (op->send_cancel_res == KL_UEFI_UDP_OP_QUARANTINED ||
                            op->send_cancel_res == KL_UEFI_UDP_OP_INVALID);
                for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
                out[count].kind  = KL_COMP_DGRAM_SEND;
                out[count].ok    = 0;
                out[count].bytes = op->snd_len;   /* release the FULL q_bytes reservation */
                if (quar) {
                    /* UNCONFIRMED: borrowed terminal — retire the send machine, RETAIN the ref (the
                     * abandoned firmware Tx op may still read op->snd), keep the record in_use so
                     * retire_dgram(SEND) reports QUARANTINED once backend_close quarantines the slot. */
                    out[count].life        = op->life;   /* BORROW — do NOT NULL */
                    out[count].retain_life = 1;
                    count++;
                    op->terminal_emitted = 1;
                } else {   /* RETIRED — confirmed: transfer the ref (dispatch releases) + retire the record */
                    out[count].life = op->life; op->life = NULL;   /* TRANSFER ref op → event */
                    count++;
                    op->in_use = 0;
                    efi_dgram_pump_sends(sfd);
                }
                continue;
            }
            if (!op->posted) continue;   /* queued but not yet posted (another send holds the Tx token) */
            size_t nb = 0; int ok = 0;
            KlUefiUdpOpResult r = kl_uefi_udp_poll_send(op->fd, op->generation, &nb, &ok);
            if (r == KL_UEFI_UDP_OP_PENDING) continue;
            if (r == KL_UEFI_UDP_OP_DELIVERED || r == KL_UEFI_UDP_OP_STALE_RETIRED) {
                (void)nb;
                /* A terminal that RETIRES the send machine. DELIVERED = the Transmit completed (ok as
                 * reported); STALE_RETIRED (7B-9, symmetric with recv) = the child was cleanly closed by
                 * backend_close before we polled → ok=0. Both TRANSFER the ref (dispatch releases it). */
                for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
                out[count].kind  = KL_COMP_DGRAM_SEND;
                out[count].life  = op->life; op->life = NULL;   /* TRANSFER ref op → event */
                out[count].ok    = (r == KL_UEFI_UDP_OP_DELIVERED) ? ok : 0;
                /* Emit the RESERVED length (== snd_len), not the reported byte count, so udp.c releases
                 * the full q_bytes reservation whether the Transmit succeeded or failed (ok=0). */
                out[count].bytes = op->snd_len;
                count++;
                op->in_use = 0;
                efi_dgram_pump_sends(sfd);   /* post the next queued send for this socket */
            } else {   /* QUARANTINED or INVALID — unconfirmed: borrowed terminal, retain ref + classify */
                for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
                out[count].kind        = KL_COMP_DGRAM_SEND;
                out[count].life        = op->life;   /* BORROW — do NOT NULL op->life */
                out[count].retain_life = 1;
                out[count].ok          = 0;
                out[count].bytes       = op->snd_len;
                count++;
                op->terminal_emitted = 1;   /* keep in_use (abandoned) — never re-drained/re-emitted */
            }
        }
    }

    /* When no send op remains, restart the FIFO acceptance counter — so op->seq comparisons can never
     * be defeated by a (2^64) wrap over the loop's lifetime (formally completes the FIFO invariant). */
    {
        int any_send = 0;
        for (int i = 0; i < KL_EFI_MAX_DGRAM_OPS; i++)
            if (g_efi.dgram[i].in_use && g_efi.dgram[i].kind == EFI_DG_SEND) { any_send = 1; break; }
        if (!any_send) g_efi.dgram_seq = 0;
    }
#endif /* KEEL_UEFI_DATAGRAM */

    if (count == 0 && g_efi.bs)
        g_efi.bs->Stall(1000);   /* 1 ms — idle tick while a connect settles / no work */
    return count;
}

static const KlCompletionOps EFI_COMP_OPS = {
    .drain = el_drain,
    .post_connect = el_post_connect,
    .cancel = el_cancel,
    .prime_accepts = el_prime_accepts,   /* S-3: inbound server accept */
    .post_accept = el_post_accept,
    .post_recv = el_post_recv,           /* S-4: server completion-native recv */
    .post_send = el_post_send,           /* S-4: server completion-native send */
#ifdef KEEL_UEFI_DATAGRAM
    .post_dgram_recv = el_post_dgram_recv,   /* 6.4b-3b: datagram completion recv (EFI_UDP4 Receive) */
    .post_dgram_send = el_post_dgram_send,   /* 6.4b-3b: datagram completion send (EFI_UDP4 Transmit) */
    .cancel_dgram    = el_cancel_dgram,      /* 7B-2: request firmware Cancel (release stays with drain/close) */
    .retire_dgram    = el_retire_dgram,      /* 7B-2: §4.3 classifier — the EFI QUARANTINE override */
    /* TCP-only builds leave the datagram ops (post/cancel/retire) NULL — no datagram socket is ever
     * created there, so the completion core never dispatches to them (KEEL_UEFI_DATAGRAM off). */
#endif
    /* post_sendfile = NULL: file responses (S-6) are out of scope for the S-4 plaintext
     * server. The CLIENT's stream send/recv still ride the SYNC socket provider relayed as
     * KL_COMP_WATCHER (post_connect + the drain watch loop); the datagram ops above serve
     * KlUdp/dns_resolver over the completion axis without changing the stream client path. */
};

static const KlEventOps EFI_EVENT_OPS = {
    .init = el_init, .add = el_add, .mod = el_mod, .del = el_del,
    .wait = el_wait, .close = el_close, .caps = el_caps,
    .native_provider = el_native_provider,
    .completion = &EFI_COMP_OPS,
};

static const KlEventProvider EFI_EVENT_PROVIDER = { &EFI_EVENT_OPS, "efi-tcp4-completion" };

const KlEventProvider *kl_uefi_event_provider(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    if (g_efi.created) return &EFI_EVENT_PROVIDER;
    if (!bs) return NULL;
    for (size_t b = 0; b < sizeof(g_efi); b++) ((unsigned char *)&g_efi)[b] = 0;
    g_efi.bs = bs;
    g_efi.image = image;
    g_efi.created = 1;
    return &EFI_EVENT_PROVIDER;
}

void kl_uefi_event_provider_reset(void) {
    for (size_t b = 0; b < sizeof(g_efi); b++) ((unsigned char *)&g_efi)[b] = 0;
}
