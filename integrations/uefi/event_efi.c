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
#include <keel/allocator.h>            /* kl_malloc/kl_free — post_send buffer copy (S-4) */
#include <keel/connection.h>           /* KlConn — server post_recv/post_send targets (S-4) */
#include "../../src/socket.h"          /* KlSocketProvider, kl_sock_accept/send/recv, kl_handle_valid */
#include "../../src/completion.h"      /* KlCompletionOps, KlCompletionEvent, KL_COMP_* */
#include <keel/server.h>               /* KlServer.pool — accept backpressure (S-3) */

#include <stdint.h>
#include <stddef.h>

/* Bounds match the client's needs: it drives ONE connection (one connect op, a
 * handful of live watches). Small fixed arrays — no allocation in the loop. */
#define KL_EFI_MAX_CONN_OPS 8
#define KL_EFI_MAX_WATCHES  8
/* Server completion-native I/O ops (S-4). The accepted-child pool is KL_EFI_MAX_CONNS
 * (8) and the server completion driver keeps at most one recv OR one send outstanding
 * per conn, so 16 slots leave headroom without allocation in the loop. */
#define KL_EFI_MAX_IO_OPS   16

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
} EfiConnOp;

/* A registered tagged watch (add/mod). drain relays its armed mask as a
 * KL_COMP_WATCHER so the client retries the SYNC send/recv on the U-2 provider. */
typedef struct {
    int             in_use;
    KlSocketHandle  fd;
    KlEventMask     mask;
    void           *udata;          /* tagged KlWatcher pointer (LSB=1) */
} EfiWatch;

/* A posted server-side recv or send on an accepted child (S-4). The client rides the
 * watcher relay; the SERVER completion driver (completion_server.c) posts recv/send as
 * completion-native ops. drain services each via the U-2 SYNC socket provider and
 * surfaces KL_COMP_READ / KL_COMP_WRITE. The captured generation is the stale guard: an
 * op for a child that closed (generation bumped) or whose slot was reused (magic
 * cleared) is dropped, never delivered, mirroring the connect-op discipline. */
typedef struct {
    int             in_use;
    int             is_send;         /* 0 = recv (into conn->read_buf), 1 = send */
    KlConn         *conn;            /* completion target (ev->target) */
    KlSocketHandle  fd;
    uint64_t        generation;      /* captured at post; drain re-checks kl_uefi_conn_valid_h */
    /* send only: a private copy of the response bytes (the caller's iovec array is on
     * its stack and gone after post_send returns; the pointed-to c->res bytes stay valid
     * per the completion contract, but copying keeps the op self-contained + simple). */
    unsigned char  *sbuf;
    size_t          send_total;
    size_t          send_done;
    KlAllocator    *alloc;           /* frees sbuf on completion / cancel */
} EfiIoOp;

typedef struct {
    EFI_BOOT_SERVICES *bs;
    EFI_HANDLE         image;
    int                created;
    EfiConnOp          conns[KL_EFI_MAX_CONN_OPS];
    EfiWatch           watches[KL_EFI_MAX_WATCHES];
    EfiIoOp            io[KL_EFI_MAX_IO_OPS];   /* S-4 server recv/send ops */
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
    /* Preserve bs/image/created (set by kl_uefi_event_provider); clear per-run state. */
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS; i++) g_efi.conns[i].in_use = 0;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++) g_efi.watches[i].in_use = 0;
    /* Free any leftover send buffers before clearing (defensive; normally none). */
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++) {
        if (g_efi.io[i].in_use && g_efi.io[i].sbuf && g_efi.io[i].alloc)
            kl_free(g_efi.io[i].alloc, g_efi.io[i].sbuf,
                    g_efi.io[i].send_total ? g_efi.io[i].send_total : 1);
        g_efi.io[i].in_use = 0;
    }
    return 0;
}

/* Retire one server I/O op: free its send buffer (recv ops have none) and mark free. */
static void io_op_free(EfiIoOp *op) {
    if (op->sbuf && op->alloc)
        kl_free(op->alloc, op->sbuf, op->send_total ? op->send_total : 1);
    op->sbuf = NULL;
    op->in_use = 0;
}

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
static void el_close(KlEventLoop *loop) { (void)loop; }
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
    EfiConnOp *op = NULL;
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS; i++)
        if (!g_efi.conns[i].in_use) { op = &g_efi.conns[i]; break; }
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
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS; i++)
        if (g_efi.conns[i].in_use && g_efi.conns[i].fd == fd)
            g_efi.conns[i].in_use = 0;
    /* Also drop any watches on the fd (the client is done with it). */
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].fd == fd)
            g_efi.watches[i].in_use = 0;
    /* Drop any posted server recv/send ops on the fd (freeing the send buffer) so a
     * completion cannot fire at an about-to-be-freed conn (S-4). */
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++)
        if (g_efi.io[i].in_use && g_efi.io[i].fd == fd)
            io_op_free(&g_efi.io[i]);
}

/* prime_accepts: latch the server + its passive listen fd so drain can hand back
 * accepted children. The S-2 listen() already armed the Accept-token pool; this only
 * records what drain needs. Idempotent. */
static int el_prime_accepts(struct KlServer *s) {
    if (!s) return -1;
    g_efi.server = s;
    g_efi.listen_fd = s->listen_fd;
    g_efi.accept_primed = 1;
    return 0;
}

/* post_accept: the EFI Accept-token pool self-refills inside efi_sock_accept (it
 * re-arms each consumed token), so a refill is a no-op beyond staying primed. */
static int el_post_accept(struct KlServer *s) {
    return el_prime_accepts(s);
}

static EfiIoOp *io_op_alloc(void) {
    for (int i = 0; i < KL_EFI_MAX_IO_OPS; i++)
        if (!g_efi.io[i].in_use) return &g_efi.io[i];
    return NULL;
}

/* post_recv (S-4): queue a server-side receive on an accepted child. The bytes land in
 * conn->read_buf at the live read_len (computed in drain, unchanged between post and
 * completion — the conn parks with no other op), surfaced as KL_COMP_READ. */
static int el_post_recv(KlConn *c) {
    if (!c) return -1;
    if (c->read_cap <= c->read_len) return -1;   /* no header/body space — caller closes */
    EfiIoOp *op = io_op_alloc();
    if (!op) return -1;
    for (size_t b = 0; b < sizeof(*op); b++) ((unsigned char *)op)[b] = 0;
    op->in_use     = 1;
    op->is_send    = 0;
    op->conn       = c;
    op->fd         = c->fd;
    op->generation = (uint64_t)kl_uefi_conn_generation_h(c->fd);
    return 0;
}

/* post_send (S-4): queue a server-side send of the response iovec. The iovec array lives
 * on the caller's stack, so flatten a private copy; drain transmits it (looping over the
 * bounded EFI Transmit fragment) and surfaces one KL_COMP_WRITE when fully out. */
static int el_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    if (!c) return -1;
    EfiIoOp *op = io_op_alloc();
    if (!op) return -1;
    for (size_t b = 0; b < sizeof(*op); b++) ((unsigned char *)op)[b] = 0;
    op->alloc = c->alloc;
    op->sbuf  = kl_malloc(c->alloc, total ? total : 1);
    if (!op->sbuf) return -1;   /* op still !in_use — nothing to retire */
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        for (size_t j = 0; j < iov[i].len; j++)
            op->sbuf[off + j] = ((const unsigned char *)iov[i].base)[j];
        off += iov[i].len;
    }
    op->in_use     = 1;
    op->is_send    = 1;
    op->conn       = c;
    op->fd         = c->fd;
    op->generation = (uint64_t)kl_uefi_conn_generation_h(c->fd);
    op->send_total = total;
    op->send_done  = 0;
    return 0;
}

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
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS && count < max; i++) {
        EfiConnOp *op = &g_efi.conns[i];
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
        KlConn *c = op->conn;

        if (!kl_uefi_conn_valid_h(op->fd, op->generation)) {
            io_op_free(op);        /* stale (closed / slot reused) — drop, no event */
            continue;
        }

        if (!op->is_send) {
            /* RECV: only when the provider can return something now. */
            if (!kl_uefi_socket_recv_ready(op->fd)) continue;
            void  *buf = c->read_buf + c->read_len;      /* live: read_len unchanged since post */
            size_t cap = c->read_cap - c->read_len;
            ssize_t n = kl_sock_recv(ctx->sockets, op->fd, buf, cap);
            for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
            out[count].kind   = KL_COMP_READ;
            out[count].target = c;
            out[count].bytes  = (n > 0) ? (size_t)n : 0;  /* 0 / <0 → peer closed; driver closes */
            out[count].ok     = (n > 0) ? 1 : 0;
            count++;
            io_op_free(op);
            continue;
        }

        /* SEND: transmit the remainder synchronously (bounded fragments). */
        int fatal = 0, wouldblock = 0;
        while (op->send_done < op->send_total) {
            ssize_t n = kl_sock_send(ctx->sockets, op->fd,
                                     op->sbuf + op->send_done,
                                     op->send_total - op->send_done);
            if (n > 0) { op->send_done += (size_t)n; continue; }
            KlIoStatus st = kl_sock_io_status(ctx->sockets);
            if (st == KL_IO_WOULD_BLOCK || st == KL_IO_INTERRUPTED) { wouldblock = 1; break; }
            fatal = 1; break;
        }
        if (wouldblock) continue;   /* retry the remainder on a later drain */
        for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
        out[count].kind   = KL_COMP_WRITE;
        out[count].target = c;
        out[count].bytes  = fatal ? 0 : op->send_total;
        out[count].ok     = fatal ? 0 : 1;
        count++;
        io_op_free(op);
    }

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
    /* post_sendfile/post_udp_* = NULL: file responses (S-6) + UDP are out of scope for
     * the S-4 plaintext server. The CLIENT's send/recv still ride the SYNC socket
     * provider relayed as KL_COMP_WATCHER (post_connect + the drain watch loop), so
     * adding these server ops does not change the client path. */
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
