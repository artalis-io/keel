/*
 * event_iouring.c: a COMPLETION-native io_uring backend.
 *
 * The completion event axis (completion.h) is natively a completion engine's home:
 * post an async op, drain finished ops. io_uring is a completion engine by construction
 * (submit SQEs, reap CQEs), so it is the truest fit for the axis, more than IOCP
 * (per-op OVERLAPPED bookkeeping) or the pollcomp facade (completion faked over poll()).
 *
 * This implements the same completion.h contract as event_iocp.c
 * (Windows) and event_pollcomp.c (portable poll facade). It reuses the platform-independent
 * completion driver (completion_core.c) VERBATIM (no #ifdef, no io_uring symbol leaks
 * into the driver) and is the first PRODUCTION (not test-facade) completion backend that
 * is CI-testable on Linux.
 *
 * This is the ONLY io_uring backend: BACKEND=iouring selects it. It is completion-native
 * (submit SQEs, reap CQEs), which benchmarks ~2–2.3× faster than a readiness-adapted
 * io_uring poll-multiplexer on both x86 and ARM (see
 * docs/archive/phases/phase8f5_iouring_default_migration_design.md).
 *
 * Model: each kl_comp_post_* prepares an SQE (recv/send/accept/…) with the op pointer as
 * user_data and queues it; kl_comp_drain submits the batch and waits, then for each CQE
 * recovers the op, maps cqe->res to a platform-independent KlCompletionEvent, and (for a
 * short write) re-prepares the remainder rather than completing. The kernel performs the
 * I/O into the op's buffers; the driver sees only high-level, completed events. A
 * readiness FD watch (kl_event_add with a tagged KlWatcher udata: thread-pool wakeup,
 * timer) is armed as a single-shot IORING_OP_POLL_ADD and surfaced as KL_COMP_WATCHER,
 * re-armed on each fire; how a completion backend watches readiness fds alongside
 * its completions is its own business, per the abstract contract.
 *
 * Two refinements: zero-copy sendfile via IORING_OP_SPLICE (file → pipe →
 * socket, no userspace copy of the body) and a registered send-buffer pool (small responses
 * go out via IORING_OP_WRITE_FIXED from kernel-pinned buffers, avoiding a per-send malloc).
 * Both degrade gracefully: a kernel without SPLICE falls back to pread+SEND, and a failed
 * buffer registration falls back to malloc+SEND.
 *
 * Kernel-feature note: single-shot accept + single-shot poll (re-armed) are used so the
 * backend runs on older kernels (multishot accept 5.19+, multishot poll 5.13+); prep_recv/
 * send/accept/cancel are 5.5–5.6+, splice 5.7+, registered buffers 5.1+. See
 * docs/archive/phases/phase8f_iouring_completion_design.md.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE               /* pipe2 / splice (zero-copy sendfile) */
#endif
#include <keel/event.h>
#include "event_builtin.h"
#include <keel/event_ctx.h>      /* KlEventCtx (->loop._backend): neutral accept/dgram ctx */
#include <keel/stream_detail.h>  /* KlStream layout: fd / alloc / ctx (recv/send/sendfile) */
#include "udp_cmsg.h"            /* KL_UDP_RX_CTRL_SIZE, kl_udp_parse_local: pktinfo local addr (POSIX) */
#include "sockaddr_native.h"     /* KlSockAddr -> host sockaddr for the overlapped UDP send */
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED + seam */
#include "completion.h"          /* the abstract axis this TU implements */
#include "datagram_life.h"       /* stable-liveness token for datagram completion ops (neutral) */
#include "platform.h"            /* kl_plat_file_pread: file body into the send buffer */

#include <liburing.h>
#include <poll.h>                /* POLLIN/POLLOUT: poll-add mask + CQE revents */
#include <sys/socket.h>          /* struct msghdr: UDP recvmsg/sendmsg */
#include <sys/uio.h>             /* struct iovec */
#include <sys/resource.h>        /* getrlimit(RLIMIT_MEMLOCK): gate the registered pool */
#include <fcntl.h>               /* pipe2, O_NONBLOCK: splice pipe */
#include <unistd.h>              /* close: splice pipe */
#include <string.h>
#include <errno.h>
#include <stdint.h>              /* SIZE_MAX: sendfile buffer overflow guard */
#include <time.h>

#define KL_IOU_RING_SIZE  1024                /* SQ/CQ depth: 256 conns × ~1 in-flight op + slack */

/* Registered send-buffer pool: small responses copy into a pre-registered,
 * kernel-pinned buffer and go out via IORING_OP_WRITE_FIXED: no per-send malloc, no
 * per-op buffer mapping. Larger responses fall back to a malloc'd buffer + IORING_OP_SEND.
 * The pool is pinned memlock (RLIMIT_MEMLOCK), so keep it modest: 256 KiB covers the common
 * small-response hot path without starving a process that spins up many loops (an oversized
 * pool makes io_uring_queue_init fail for later loops in a test process). Registration
 * failure degrades to malloc + SEND. */
#define KL_IOU_REG_BUFS    16u                /* number of fixed send buffers */
#define KL_IOU_REG_BUFSZ   (16u * 1024u)      /* size of each fixed send buffer (→ 256 KiB pool) */
/* Register the pinned send-buffer pool only when RLIMIT_MEMLOCK is at least this generous
 * (or unlimited). Below it, the environment is memlock-constrained (containers/k8s/CI) and
 * pinning would risk io_uring_queue_init ENOMEM across many loops; skip the pool instead. */
#define KL_IOU_REGBUF_MIN_MEMLOCK  (64u * 1024u * 1024u)   /* 64 MiB */
#define KL_IOU_SPLICE_CHUNK (64u * 1024u)     /* file→pipe splice chunk (default pipe capacity) */

/* Op kind. The FIRST field of both KlIouOp and KlIouWatch is this enum, so a CQE's
 * user_data can be discriminated by reading *(IouOpType*). IOU_WATCH tags a poll-add. */
typedef enum {
    IOU_ACCEPT, IOU_READ, IOU_WRITE, IOU_SENDFILE,
    IOU_DGRAM_RECV, IOU_DGRAM_SEND, IOU_WATCH,
    IOU_CONNECT   /* outbound connect: IORING_OP_CONNECT */
} IouOpType;

/* One in-flight async op: its buffers stay valid until the CQE arrives. */
typedef struct KlIouOp {
    IouOpType      type;                  /* MUST be first (CQE discriminator) */
    struct KlIouOp *next;
    KlAllocator   *alloc;
    KlSocketHandle fd;
    KlStream      *stream;               /* READ / WRITE / SENDFILE target (raw transport) */
    /* Datagram ops (IOU_DGRAM_RECV/_SEND): the transport-neutral stable-liveness token, retained at
     * post so the op NEVER dereferences the transport owner afterwards (the owner may be freed while this op is
     * in flight). The recv buffer + capture flags are COPIED at post (into buf/buflen/dg_pktinfo/
     * dg_gro) so the completion touches only the op. */
    KlDgramLife   *life;
    int            dg_pktinfo;            /* UDP_RECV: capture pktinfo local addr */
    int            dg_gro;                /* UDP_RECV: capture GRO segment size */
    int            dg_tos;                /* UDP_RECV: capture received TOS byte */
    void          *buf;                   /* READ / UDP_RECV: receive buffer (pinned by `life`) */
    size_t         buflen;
    char          *sendbuf;               /* WRITE/UDP_SEND: owned copy.
                                           * With reg_idx >= 0 it borrows a registered buffer. */
    size_t         sendcap;               /* malloc'd size of sendbuf (0 if borrowed/registered) */
    size_t         send_total, send_done; /* partial-send tracking (<= sendcap) */
    int            reg_idx;               /* registered send-buffer index, or -1 */
    /* SENDFILE splice state: head SEND, then file→pipe→socket zero-copy. */
    int            sf_stage;              /* 0 = head, 1 = splice-in, 2 = splice-out */
    int            file_fd;               /* borrowed (response owns/closes it) */
    int            pipe_rd, pipe_wr;      /* per-op pipe (-1 until first splice) */
    uint64_t       file_count, file_off;  /* file bytes total + progress */
    size_t         pipe_len;              /* bytes currently buffered in the pipe */
    size_t         sent_total;            /* total bytes to report on WRITE completion */
    struct msghdr  msgh;                  /* UDP: recvmsg/sendmsg header */
    struct iovec   msgiov;                /* UDP: single iovec */
    /* _Alignas: typed struct cmsghdr accesses (recv pktinfo parse + send control build) require
     * cmsghdr alignment; a plain unsigned char[] is only byte-aligned. Dual-use (recv + send). */
    _Alignas(struct cmsghdr) unsigned char udp_ctrl[KL_UDP_RX_CTRL_SIZE]; /* UDP recv pktinfo / send cmsg */
    struct sockaddr_storage peer;         /* ACCEPT peer / UDP addr (msg_name) / CONNECT dest */
    socklen_t      peer_len;
    union {
        void              *watcher_udata;   /* CONNECT: the client's tagged KlWatcher */
    };
    int            aborted;               /* cancelled (idle timeout): deliver as error */
} KlIouOp;

/* A registered readiness watch. Armed as a single-shot POLL_ADD; re-armed on each
 * fire. Its FIRST field is IOU_WATCH so a poll CQE's user_data is recognised. */
typedef struct KlIouWatch {
    IouOpType         type;               /* == IOU_WATCH (MUST be first) */
    struct KlIouWatch *next;
    KlSocketHandle     fd;
    KlEventMask        mask;
    void              *udata;             /* the tagged KlWatcher pointer */
    int                armed;             /* a POLL_ADD is outstanding for this watch */
    int                removed;           /* kl_event_del pending: free on its CQE */
} KlIouWatch;

typedef struct {
    struct io_uring ring;
    KlAllocator    *alloc;
    KlIouOp        *ops;                   /* in-flight op list (for cancel + cleanup) */
    KlIouWatch     *watches;               /* registered readiness watches */
    KlSocketHandle  listen_fd;
    int             accept_pending;        /* one accept op outstanding at a time */
    int             primed;
    /* Registered send-buffer pool: WRITE_FIXED for small responses. */
    unsigned char  *reg_block;             /* one contiguous KL_IOU_REG_BUFS × BUFSZ block */
    struct iovec    reg_iov[KL_IOU_REG_BUFS];
    int             reg_free[KL_IOU_REG_BUFS];  /* free-list stack of buffer indices */
    int             reg_free_count;
    int             reg_ok;                /* buffers registered; else always malloc+SEND */
    int             has_splice;            /* IORING_OP_SPLICE supported; else pread+SEND */
} KlIouState;

/* ── SQE helper: flush the SQ if it is full, then retry once. ─────────── */
static struct io_uring_sqe *iou_sqe(KlIouState *st) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&st->ring);
    if (!sqe) {
        io_uring_submit(&st->ring);        /* drain queued SQEs to free ring slots */
        sqe = io_uring_get_sqe(&st->ring);
    }
    return sqe;
}

static unsigned iou_poll_mask(KlEventMask mask) {
    unsigned e = 0;
    if (mask & KL_EVENT_READ)  e |= POLLIN;
    if (mask & KL_EVENT_WRITE) e |= POLLOUT;
    return e;
}

/* ── KlEventLoop lifecycle ───────────────────────────────────────────── */

/* Register the fixed send-buffer pool + probe splice support. Best-effort: on
 * failure the backend falls back to malloc+SEND / pread+SEND, so this never fails init.
 *
 * The registered pool (WRITE_FIXED fast path for small responses) pins
 * RLIMIT_MEMLOCK-accounted memory. In memlock-constrained environments (containers,
 * Kubernetes, and CI commonly cap it at a few MB) that pinning (multiplied across many
 * short-lived event loops, e.g. a test suite that builds a fresh KlEventCtx per case)
 * exhausts the limit, and since io_uring teardown reclaims memlock asynchronously, later
 * io_uring_queue_init() calls fail with ENOMEM. So only register the pool when memlock is
 * generous (unlimited, or comfortably above the pool); otherwise skip it and use the
 * malloc+SEND fallback (identical behavior, minus the WRITE_FIXED micro-optimization). */
static void iou_init_optim(KlIouState *st) {
    struct rlimit ml;
    int memlock_ample = getrlimit(RLIMIT_MEMLOCK, &ml) == 0 &&
                        (ml.rlim_cur == RLIM_INFINITY ||
                         ml.rlim_cur >= KL_IOU_REGBUF_MIN_MEMLOCK);
    st->reg_block = memlock_ample
        ? kl_malloc(st->alloc, (size_t)KL_IOU_REG_BUFS * KL_IOU_REG_BUFSZ)
        : NULL;
    if (st->reg_block) {
        for (unsigned i = 0; i < KL_IOU_REG_BUFS; i++) {
            st->reg_iov[i].iov_base = st->reg_block + (size_t)i * KL_IOU_REG_BUFSZ;
            st->reg_iov[i].iov_len  = KL_IOU_REG_BUFSZ;
        }
        if (io_uring_register_buffers(&st->ring, st->reg_iov, KL_IOU_REG_BUFS) == 0) {
            st->reg_ok = 1;
            for (unsigned i = 0; i < KL_IOU_REG_BUFS; i++)
                st->reg_free[i] = (int)(KL_IOU_REG_BUFS - 1 - i);   /* stack: pop 0 first */
            st->reg_free_count = (int)KL_IOU_REG_BUFS;
        } else {
            kl_free(st->alloc, st->reg_block, (size_t)KL_IOU_REG_BUFS * KL_IOU_REG_BUFSZ);
            st->reg_block = NULL;
        }
    }
    struct io_uring_probe *probe = io_uring_get_probe_ring(&st->ring);
    st->has_splice = probe && io_uring_opcode_supported(probe, IORING_OP_SPLICE);
    if (probe) io_uring_free_probe(probe);
}

int kl_event_init_builtin(KlEventLoop *loop) {
    KlIouState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = loop->alloc;
    st->listen_fd = KL_INVALID_SOCKET;
    if (io_uring_queue_init(KL_IOU_RING_SIZE, &st->ring, 0) < 0) {
        kl_free(loop->alloc, st, sizeof(*st));
        return -1;
    }
    iou_init_optim(st);
    loop->_backend = st;
    loop->fd = -1;
    return 0;
}

/* Connection fds are driven by posted ops, not readiness; for those (untagged udata)
 * add/mod/del are inert, as on IOCP/pollcomp. A TAGGED udata is a KlWatcher (thread-pool
 * wakeup, timer, generic FD watcher); register it and arm a single-shot POLL_ADD so
 * kl_comp_drain relays its readiness as a KL_COMP_WATCHER. Keys on the shared LSB
 * watcher tag, not on anything io_uring-specific. */
/* Arm a single-shot POLL_ADD for the watch. Exactly one poll is outstanding per watch:
 * skip if one already is (a mask change while armed keeps the old poll, acceptable, our
 * completion watchers use a constant READ mask). The re-arm after each fire comes from
 * kl_watcher_rearm → kl_event_mod (kl_event_dispatch calls it), not from the drain. */
static int iou_arm_watch(KlIouState *st, KlIouWatch *w) {
    if (w->armed) return 0;
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) return -1;
    io_uring_prep_poll_add(sqe, w->fd, iou_poll_mask(w->mask));
    io_uring_sqe_set_data(sqe, w);
    w->armed = 1;
    return 0;
}

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;   /* connection: posted ops, no readiness watch */
    KlIouState *st = loop->_backend;
    for (KlIouWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd && !w->removed) { w->mask = mask; w->udata = udata; return 0; }  /* idempotent */
    KlIouWatch *w = kl_malloc(st->alloc, sizeof(*w));
    if (!w) return -1;
    memset(w, 0, sizeof(*w));
    w->type = IOU_WATCH;
    w->fd = fd;
    w->mask = mask;
    w->udata = udata;
    w->next = st->watches;
    st->watches = w;
    if (iou_arm_watch(st, w) < 0) { st->watches = w->next; kl_free(st->alloc, w, sizeof(*w)); return -1; }
    return 0;
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;
    KlIouState *st = loop->_backend;
    for (KlIouWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd && !w->removed) {
            KlEventMask old = w->mask;
            w->mask = mask; w->udata = udata;
            if (!w->armed)                       /* no poll in flight: just arm the new mask */
                return iou_arm_watch(st, w);
            if (old == mask)                     /* mask unchanged: nothing to re-arm */
                return 0;
            /* An in-flight single-shot poll carries the OLD mask; iou_arm_watch() would
             * no-op (its `if (w->armed) return 0`), leaving the new interest inert until
             * the old condition happens to fire, which for e.g. READ→WRITE on a
             * not-readable-but-writable fd is never. Atomically retarget the poll's event
             * mask (IORING_POLL_UPDATE_EVENTS, kernel ≥5.13): the poll keeps user_data = w
             * and will fire with the NEW mask. The update op's own CQE is tagged NULL so the
             * drain skips it (ud == 0). On an older kernel the update SQE is rejected with a
             * NULL-data (ignored) CQE and the poll keeps its old mask: the prior behaviour,
             * no regression. */
            struct io_uring_sqe *sqe = iou_sqe(st);
            if (!sqe) return -1;
            io_uring_prep_poll_update(sqe, (__u64)(uintptr_t)w, 0,
                                      iou_poll_mask(mask), IORING_POLL_UPDATE_EVENTS);
            io_uring_sqe_set_data(sqe, NULL);    /* sentinel: ignore the update's completion */
            return 0;
        }
    return kl_event_add_builtin(loop, fd, mask, udata);   /* not yet watched: add it */
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    KlIouState *st = loop->_backend;
    for (KlIouWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd && !w->removed) {
            w->removed = 1;   /* add/mod skip it; a re-add of this fd allocates a fresh watch */
            if (w->armed) {
                /* A POLL_ADD is in the kernel. Cancel it; the poll's –ECANCELED CQE
                 * (data = w) unlinks + frees the watch in the drain. Freeing here would leave
                 * the kernel writing a CQE against freed memory. The watch stays LINKED so
                 * that if that CQE never arrives (kl_event_close → io_uring_queue_exit at
                 * shutdown drops it), kl_event_close still frees it: no leak. */
                struct io_uring_sqe *sqe = iou_sqe(st);
                if (sqe) {
                    io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)w);
                    io_uring_sqe_set_data(sqe, NULL);   /* sentinel: ignore this CQE */
                }
            } else {
                /* No poll outstanding: unlink + free now. */
                for (KlIouWatch **link = &st->watches; *link; link = &(*link)->next)
                    if (*link == w) { *link = w->next; break; }
                kl_free(st->alloc, w, sizeof(*w));
            }
            return 0;
        }
    return 0;   /* not watched (a connection fd): nothing to do */
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* The server drives a completion loop via kl_comp_drain (completion seam), not this
     * readiness call. Defined because the KlEventLoop API requires it. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0) {
        struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void iou_op_free(KlIouOp *op) {
    /* A datagram op that still owns its token reference (dropped WITHOUT emitting an event: post-
     * failure unwind, an aborted op freed in the drain loop, or loop teardown after queue_exit)
     * releases it here. Its final release frees the receive storage. iou_complete transfers the ref to
     * the event and NULLs op->life first, so an emitted op does not double-release. Non-datagram ops
     * carry op->life == NULL. */
    if (op->life) kl_dgram_life_release(op->life);
    /* A registered send buffer (reg_idx >= 0) is borrowed: its index is returned to the
     * pool at the completion/error site, not here; only a malloc'd sendbuf is freed. */
    if (op->sendbuf && op->reg_idx < 0) kl_free(op->alloc, op->sendbuf, op->sendcap);
    if (op->pipe_rd >= 0) close(op->pipe_rd);
    if (op->pipe_wr >= 0) close(op->pipe_wr);
    kl_free(op->alloc, op, sizeof(*op));
}

/* Allocate a zeroed op with the sentinel fields set (reg_idx / pipe / file fds are all
 * "unset" = -1, distinct from the valid 0 that memset would leave). */
static KlIouOp *iou_op_alloc(KlAllocator *alloc) {
    KlIouOp *op = kl_malloc(alloc, sizeof(*op));
    if (!op) return NULL;
    memset(op, 0, sizeof(*op));
    op->alloc = alloc;
    op->reg_idx = -1;
    op->pipe_rd = op->pipe_wr = -1;
    op->file_fd = -1;
    return op;
}

/* Registered send-buffer pool: pop/push a buffer index, or -1 if none free. */
static int iou_reg_acquire(KlIouState *st) {
    if (!st->reg_ok || st->reg_free_count == 0) return -1;
    return st->reg_free[--st->reg_free_count];
}
static void iou_reg_release(KlIouState *st, int idx) {
    if (idx >= 0 && st->reg_free_count < (int)KL_IOU_REG_BUFS)
        st->reg_free[st->reg_free_count++] = idx;
}

/* Relinquish an op's send buffer (return a registered buffer to the pool, or free a
 * malloc'd one) and null it so iou_op_free won't touch it. Idempotent. Called when a
 * send finishes (WRITE/SENDFILE) or when the SENDFILE head is done and the file phase
 * begins (the head buffer isn't needed during the splice). */
static void iou_send_release(KlIouState *st, KlIouOp *op) {
    if (op->reg_idx >= 0) { iou_reg_release(st, op->reg_idx); op->reg_idx = -1; }
    else if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->sendcap);
    op->sendbuf = NULL;
    op->sendcap = 0;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlIouState *st = loop->_backend;
    if (!st) return;
    io_uring_queue_exit(&st->ring);        /* kernel drops in-flight ops + registered bufs */
    KlIouOp *op = st->ops;
    while (op) { KlIouOp *n = op->next; iou_op_free(op); op = n; }
    KlIouWatch *w = st->watches;
    while (w) { KlIouWatch *n = w->next; kl_free(st->alloc, w, sizeof(*w)); w = n; }
    if (st->reg_block) kl_free(st->alloc, st->reg_block,
                               (size_t)KL_IOU_REG_BUFS * KL_IOU_REG_BUFSZ);
    kl_free(st->alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}

/* A completion loop over native fds: COMPLETION makes the capability negotiation require an
 * OVERLAPPED provider (kl_socket_provider_iouring), which kl_event_native_provider auto-
 * wires so a default-provider server/client is a drop-in. */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped provider this completion loop needs, so a server/client that
 * configured no provider auto-wires it and a completion backend is a drop-in. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_iouring();
}

/* The overlapped socket provider: reuse the POSIX control-plane ops (close, the direct
 * send comp_tls_flush uses, tcp_nodelay, …) and add the OVERLAPPED capability the
 * negotiation keys on, so completion_core.c's kl_sock_* calls behave normally while the
 * loop advertises completion. io_uring uses real fds + the POSIX control plane. */
const KlSocketProvider *kl_socket_provider_iouring(void) {
    static KlSocketProvider prov;
    const KlSocketProvider *posix = kl_socket_provider_posix();
    prov.ops = posix->ops;
    prov.context = posix->context;
    prov.capabilities = posix->capabilities | KL_SOCK_CAP_OVERLAPPED;
    prov.dgram = posix->dgram;   /* UDP config/opts (readiness dgram data-plane unused on this loop) */
    return &prov;
}

/* ── completion.h implementation (io_uring SQE/CQE mechanics) ─────────── */

static void iou_op_push(KlIouState *st, KlIouOp *op) {
    op->next = st->ops;
    st->ops = op;
}

static void iou_op_unlink(KlIouState *st, KlIouOp *op) {
    for (KlIouOp **link = &st->ops; *link; link = &(*link)->next)
        if (*link == op) { *link = op->next; return; }
}

static KlIouState *iou_state(KlStream *stream) { return stream->ctx->loop._backend; }

/* Raw receive: recv up to `cap` bytes into the caller-supplied `buf` on `stream`. No TLS /
 * connection-state knowledge; the HTTP adapter chose the buffer. */
static int iou_comp_post_recv(KlStream *stream, void *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    KlIouState *st = stream->ctx->loop._backend;
    KlIouOp *op = iou_op_alloc(stream->alloc);
    if (!op) return -1;
    op->stream = stream;
    op->fd = stream->fd;
    op->type = IOU_READ;
    op->buf = buf;
    op->buflen = cap;

    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_recv(sqe, op->fd, op->buf, op->buflen, 0);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    return 0;
}

/* Prepare a send SQE for the unsent tail of a WRITE op. A registered send buffer
 * (reg_idx >= 0) goes out via WRITE_FIXED (kernel-pinned, zero per-op mapping); a malloc'd
 * buffer via plain SEND. Both handle partial sends by re-prep from send_done. */
static void iou_prep_send_tail(KlIouState *st, KlIouOp *op) {
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { op->aborted = 1; return; }   /* no slot: surface as error next drain */
    void *p = op->sendbuf + op->send_done;
    size_t n = op->send_total - op->send_done;
    if (op->reg_idx >= 0)
        io_uring_prep_write_fixed(sqe, op->fd, p, (unsigned)n, 0, op->reg_idx);
    else
        io_uring_prep_send(sqe, op->fd, p, n, 0);
    io_uring_sqe_set_data(sqe, op);
}

/* ── zero-copy sendfile via splice (file → pipe → socket) ────────── */

static int iou_open_pipe(KlIouOp *op) {
    int pfd[2];
    if (pipe2(pfd, O_NONBLOCK) < 0) return -1;
    op->pipe_rd = pfd[0];
    op->pipe_wr = pfd[1];
    return 0;
}

/* Splice-in: file_fd (at file_off) → pipe_wr, up to one pipe-capacity chunk. flags=0
 * so io_uring waits asynchronously if the pipe is momentarily full (no busy error). */
static void iou_prep_splice_in(KlIouState *st, KlIouOp *op) {
    uint64_t chunk = op->file_count - op->file_off;
    if (chunk > KL_IOU_SPLICE_CHUNK) chunk = KL_IOU_SPLICE_CHUNK;
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { op->aborted = 1; return; }
    io_uring_prep_splice(sqe, op->file_fd, (int64_t)op->file_off, op->pipe_wr, -1,
                         (unsigned)chunk, 0);
    io_uring_sqe_set_data(sqe, op);
    op->sf_stage = 1;
}

/* Splice-out: pipe_rd → socket, up to the bytes currently buffered in the pipe.
 * flags=0 so io_uring waits for socket writability instead of returning EAGAIN. */
static void iou_prep_splice_out(KlIouState *st, KlIouOp *op) {
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { op->aborted = 1; return; }
    io_uring_prep_splice(sqe, op->pipe_rd, -1, op->fd, -1, (unsigned)op->pipe_len, 0);
    io_uring_sqe_set_data(sqe, op);
    op->sf_stage = 2;
}

static int iou_comp_post_send(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total) {
    KlIouState *st = iou_state(stream);
    KlIouOp *op = iou_op_alloc(stream->alloc);
    if (!op) return -1;
    op->type = IOU_WRITE;
    op->stream = stream;
    op->fd = stream->fd;
    op->send_total = total;

    /* Small response → a registered, kernel-pinned buffer + WRITE_FIXED (no per-send
     * malloc). Larger than a pooled buffer, or pool exhausted → malloc + SEND. */
    int idx = (total <= KL_IOU_REG_BUFSZ) ? iou_reg_acquire(st) : -1;
    if (idx >= 0) {
        op->reg_idx = idx;
        op->sendbuf = (char *)st->reg_iov[idx].iov_base;   /* borrowed */
    } else {
        op->sendcap = total ? total : 1;
        op->sendbuf = kl_malloc(stream->alloc, op->sendcap);
        if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }
    }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(op->sendbuf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }
    iou_op_push(st, op);
    iou_prep_send_tail(st, op);
    return 0;
}

/* Fallback for kernels without IORING_OP_SPLICE: pread the file body into a malloc'd send
 * buffer after the head, and send it as a plain WRITE. The response
 * owns file_fd and closes it (http_response.c), so pread is ownership-neutral. */
static int iou_post_sendfile_copy(KlStream *stream, KlIouState *st, const KlIoVec *head_iov,
                                  int head_n, size_t head_total, int file_fd, uint64_t count) {
    KlIouOp *op = iou_op_alloc(stream->alloc);
    if (!op) return -1;
    op->type = IOU_WRITE;
    op->stream = stream;
    op->fd = stream->fd;
    op->send_total = head_total + (size_t)count;
    op->sendcap = op->send_total ? op->send_total : 1;
    op->sendbuf = kl_malloc(stream->alloc, op->sendcap);
    if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }
    size_t got = 0;
    while (got < (size_t)count) {
        ssize_t nr = kl_plat_file_pread(file_fd, op->sendbuf + head_total + got,
                                        (size_t)count - got, (long long)got);
        if (nr < 0) { if (errno == EINTR) continue; iou_op_free(op); return -1; }
        if (nr == 0) break;                          /* short file: send what we have */
        got += (size_t)nr;
    }
    op->send_total = head_total + got;
    iou_op_push(st, op);
    iou_prep_send_tail(st, op);
    return 0;
}

/* Post a response-head + file-body send. Zero-copy via splice: send the head, then
 * loop file → pipe → socket (no userspace copy of the file bytes). Falls back to
 * iou_post_sendfile_copy when the kernel lacks IORING_OP_SPLICE. */
static int iou_comp_post_sendfile(KlStream *stream, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    KlIouState *st = iou_state(stream);
    if (count > (uint64_t)(SIZE_MAX / 2) || head_total > SIZE_MAX / 2)
        return -1;                                   /* overflow guard */
    if (!st->has_splice)
        return iou_post_sendfile_copy(stream, st, head_iov, head_n, head_total, file_fd, count);

    KlIouOp *op = iou_op_alloc(stream->alloc);
    if (!op) return -1;
    op->type = IOU_SENDFILE;
    op->stream = stream;
    op->fd = stream->fd;
    op->file_fd = file_fd;
    op->file_count = count;
    op->sf_stage = 0;                                /* head first */

    /* The head rides a registered/malloc buffer as a normal partial-capable send. */
    op->send_total = head_total;
    int idx = (head_total <= KL_IOU_REG_BUFSZ) ? iou_reg_acquire(st) : -1;
    if (idx >= 0) { op->reg_idx = idx; op->sendbuf = (char *)st->reg_iov[idx].iov_base; }
    else {
        op->sendcap = head_total ? head_total : 1;
        op->sendbuf = kl_malloc(stream->alloc, op->sendcap);
        if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }
    }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }
    iou_op_push(st, op);
    iou_prep_send_tail(st, op);                      /* stage 0: send the head */
    return 0;
}

static int iou_comp_post_accept(struct KlEventCtx *ctx) {
    if (!ctx) return -1;
    KlIouState *st = ctx->loop._backend;
    if (st->accept_pending) return 0;                /* one accept outstanding is enough */
    KlIouOp *op = iou_op_alloc(st->alloc);
    if (!op) return -1;
    op->type = IOU_ACCEPT;
    op->fd = st->listen_fd;
    op->peer_len = sizeof(op->peer);
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_accept(sqe, op->fd, (struct sockaddr *)&op->peer, &op->peer_len, 0);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    st->accept_pending = 1;
    return 0;
}

static int iou_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *rop) {
    KlIouState *st = ctx->loop._backend;
    KlIouOp *op = iou_op_alloc(st->alloc);
    if (!op) return -1;                 /* nothing taken → caller releases its retained token ref */
    op->type = IOU_DGRAM_RECV;
    op->fd = rop->fd;
    op->peer_len = sizeof(op->peer);
    /* The receive buffer + capture flags travel in the descriptor; the op never dereferences a transport.
     * The buffer stays valid because the token ref (transferred below) pins it past the op's lifetime. */
    op->buf        = rop->buf;
    op->buflen     = rop->cap;
    op->dg_pktinfo = (rop->capture & KL_DGRAM_RX_PKTINFO) != 0;
    op->dg_gro     = (rop->capture & KL_DGRAM_RX_GRO)     != 0;
    op->dg_tos     = (rop->capture & KL_DGRAM_RX_TOS)     != 0;
    op->msgiov.iov_base = op->buf;
    op->msgiov.iov_len = op->buflen;
    op->msgh.msg_name = &op->peer;
    op->msgh.msg_namelen = sizeof(op->peer);
    op->msgh.msg_iov = &op->msgiov;
    op->msgh.msg_iovlen = 1;
    op->msgh.msg_control = op->udp_ctrl;          /* capture pktinfo (local addr) cmsg */
    op->msgh.msg_controllen = sizeof(op->udp_ctrl);
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }      /* life unset → no release */
    io_uring_prep_recvmsg(sqe, op->fd, &op->msgh, 0);
    io_uring_sqe_set_data(sqe, op);
    op->life = rop->life;               /* TRANSFERRED into the op (no retain, the caller retained) */
    iou_op_push(st, op);
    return 0;
}

static int iou_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *sop) {
    KlIouState *st = ctx->loop._backend;
    KlIouOp *op = iou_op_alloc(st->alloc);
    if (!op) return -1;                 /* nothing taken → caller releases its ref */
    op->type = IOU_DGRAM_SEND;
    op->fd = sop->fd;
    op->send_total = sop->len;
    op->sendcap = sop->len ? sop->len : 1;
    op->sendbuf = kl_malloc(st->alloc, op->sendcap);
    if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }   /* life unset → caller releases */
    memcpy(op->sendbuf, sop->data, sop->len);   /* COPY payload before accept */
    /* Marshal the neutral dest to a host sockaddr for the overlapped sendmsg. */
    if (sop->dest && kl_sockaddr_family(sop->dest) != KL_AF_UNSPEC) {
        op->peer_len = kl_sockaddr_to_native(sop->dest, &op->peer);
        if (op->peer_len) {
            op->msgh.msg_name = &op->peer;
            op->msgh.msg_namelen = op->peer_len;
        }
    }
    op->msgiov.iov_base = op->sendbuf;
    op->msgiov.iov_len = sop->len;
    op->msgh.msg_iov = &op->msgiov;
    op->msgh.msg_iovlen = 1;
    /* Source-pin (src) / per-packet TOS control message: built into the op's control buffer (COPIED
     * before post returns; the overlapped sendmsg carries it, so no synchronous fallthrough / watcher). */
    if ((sop->src && kl_sockaddr_family(sop->src) != KL_AF_UNSPEC) || sop->tos >= 0) {
        struct sockaddr_storage sss;
        const struct sockaddr *src_sa = NULL;
        if (sop->src && kl_sockaddr_family(sop->src) != KL_AF_UNSPEC &&
            kl_sockaddr_to_native(sop->src, &sss))
            src_sa = (const struct sockaddr *)&sss;
        int fam = kl_udp_send_family((int)op->fd,
                                     op->peer_len ? (struct sockaddr *)&op->peer : NULL, src_sa);
        if (fam < 0) { iou_op_free(op); return -1; }   /* undeterminable family → fail the post */
        size_t clen;
        if (kl_udp_build_control(op->udp_ctrl, sizeof(op->udp_ctrl), src_sa, sop->tos, fam, &clen) != 0) {
            iou_op_free(op); return -1;   /* requested source-pin/TOS could not be built → fail the post */
        }
        if (clen) { op->msgh.msg_control = op->udp_ctrl; op->msgh.msg_controllen = clen; }
    }
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }       /* life unset → caller releases */
    io_uring_prep_sendmsg(sqe, op->fd, &op->msgh, 0);
    io_uring_sqe_set_data(sqe, op);
    op->life = sop->life;               /* TRANSFERRED into the op (no retain) */
    iou_op_push(st, op);
    return 0;
}

/* Cancel the outstanding datagram op(s) of `kind` for `life`: mark aborted + post an
 * IORING_OP_ASYNC_CANCEL SQE (mirrors iou_comp_cancel). The op stays tracked until its (cancelled)
 * CQE drains and releases the token ref, so this is idempotent and does NOT release the ref here. */
static int iou_comp_cancel_dgram(struct KlEventCtx *ctx, KlDgramLife *life, KlDgramOpKind kind) {
    KlIouState *st = ctx->loop._backend;
    IouOpType want = (kind == KL_DGRAM_OP_SEND) ? IOU_DGRAM_SEND : IOU_DGRAM_RECV;
    for (KlIouOp *o = st->ops; o; o = o->next)
        if (o->life == life && o->type == want && !o->aborted) {
            o->aborted = 1;
            struct io_uring_sqe *sqe = iou_sqe(st);
            if (sqe) {
                io_uring_prep_cancel(sqe, o, 0);
                io_uring_sqe_set_data(sqe, NULL);   /* sentinel: ignore the cancel CQE */
            }
        }
    return 0;
}

/* Classify retirement (§4.3): a matching op still tracked in st->ops is PENDING (its cancelled CQE
 * has not yet drained + released); none tracked means it physically retired. io_uring never
 * quarantines: a posted op always yields a terminal CQE the drain reaps. */
static KlDgramRetireResult iou_comp_retire_dgram(struct KlEventCtx *ctx, KlDgramLife *life,
                                                 KlDgramOpKind kind, int *transport_err) {
    const KlIouState *st = ctx->loop._backend;
    IouOpType want = (kind == KL_DGRAM_OP_SEND) ? IOU_DGRAM_SEND : IOU_DGRAM_RECV;
    if (transport_err) *transport_err = 0;
    for (const KlIouOp *o = st->ops; o; o = o->next)
        if (o->life == life && o->type == want) return KL_DGRAM_RETIRE_PENDING;
    return KL_DGRAM_RETIRE_RETIRED;
}

/* Post an outbound connect via IORING_OP_CONNECT. The dest is marshalled into the op's
 * peer storage (kernel reads it until the CQE), which must stay valid; it lives in the op, so
 * it does. On completion iou_complete surfaces KL_COMP_CONNECT against the client's tagged
 * watcher; the client's he_on_writable re-checks SO_ERROR (getsockopt works after a connect
 * CQE regardless of success). Returns 0 on posted, -1 on a hard local failure. */
static int iou_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                                 const KlSockAddr *addr, void *watcher_udata) {
    KlIouState *st = ctx->loop._backend;
    KlIouOp *op = iou_op_alloc(st->alloc);
    if (!op) return -1;
    op->type = IOU_CONNECT;
    op->fd = fd;
    op->watcher_udata = watcher_udata;
    op->peer_len = (addr && kl_sockaddr_family(addr) != KL_AF_UNSPEC)
                       ? kl_sockaddr_to_native(addr, &op->peer) : 0;
    if (op->peer_len == 0) { iou_op_free(op); return -1; }
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_connect(sqe, fd, (struct sockaddr *)&op->peer, op->peer_len);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    return 0;
}

static int iou_comp_prime_accepts(struct KlEventCtx *ctx, KlSocketHandle listen_fd) {
    if (!ctx) return -1;
    KlIouState *st = ctx->loop._backend;
    if (st->primed) return 1;              /* setup already done: report the window */
    st->listen_fd = listen_fd;
    st->primed = 1;
    /* Post-driven, window 1: latch the listen fd and let the completion KlListener post the single
     * accept (reserve-before-post backpressure). No accept posted here; the listener arms it. */
    return 1;
}

/* Cancel pending ops on `fd` (idle-timeout sweep): mark aborted + prep_cancel so the
 * in-flight op completes promptly (–ECANCELED) and the driver releases the connection
 * through its normal completion path. Marking (not freeing) preserves the invariant that
 * a conn is released only from a completion; the op the driver waits on still completes,
 * and the kernel is done with its buffer before we free it. */
static void iou_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    KlIouState *st = ctx->loop._backend;
    for (KlIouOp *o = st->ops; o; o = o->next)
        if (o->fd == fd && !o->aborted) {
            o->aborted = 1;
            struct io_uring_sqe *sqe = iou_sqe(st);
            if (sqe) {
                io_uring_prep_cancel(sqe, o, 0);
                io_uring_sqe_set_data(sqe, NULL);   /* sentinel: ignore the cancel CQE */
            }
        }
}

/* ── drain: submit the batch, reap CQEs, complete/re-prep ─────────────── */

/* Fill a completion event from a finished op + its cqe result. Returns 1 if `ev` was
 * written (op done → remove/free), 0 if the op made partial progress and was re-prepared
 * (keep it in-flight, no event). */
static int iou_complete(KlIouState *st, KlIouOp *op, int res, KlCompletionEvent *ev) {
    memset(ev, 0, sizeof(*ev));
    switch (op->type) {
    case IOU_ACCEPT:
        st->accept_pending = 0;
        ev->kind = KL_COMP_ACCEPT;
        ev->target = NULL;   /* ACCEPT: server recovered from ctx at dispatch */
        /* Key ONLY on res: a cancelled accept usually completes -ECANCELED (res<0, no fd → ok=0),
         * but a cancel that races a successful accept still yields a real fd (res>=0). Deliver that
         * fd even though op->aborted is set, so the completion listener disposes it on its CLOSING
         * path (total accepted-fd ownership during shutdown) instead of leaking it. */
        if (res < 0) { ev->ok = 0; return 1; }
        ev->ok = 1;
        ev->accepted_fd = res;
        if (op->peer_len > 0)                    /* native → neutral once, at the seam */
            (void)kl_sockaddr_from_native(&ev->peer, (struct sockaddr *)&op->peer,
                                          op->peer_len);
        return 1;

    case IOU_READ:
        ev->kind = KL_COMP_READ;
        ev->target = op->stream;               /* raw bytes landed in the caller's buffer */
        if (op->aborted || res <= 0) { ev->ok = 0; return 1; }  /* EOF/error/abort → close */
        ev->ok = 1;
        ev->bytes = (size_t)res;
        return 1;

    case IOU_WRITE:
        if (op->aborted || res < 0) {
            iou_send_release(st, op);
            ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 0;
            return 1;
        }
        op->send_done += (size_t)res;
        if (op->send_done < op->send_total) {         /* short write: send the tail */
            iou_prep_send_tail(st, op);
            return 0;                                 /* still in flight, no event */
        }
        iou_send_release(st, op);                     /* return the registered buffer */
        ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;
        ev->bytes = op->send_total;
        return 1;

    case IOU_SENDFILE:
        if (op->aborted || res < 0) {
            iou_send_release(st, op);                 /* free head buf if error hit in stage 0 */
            ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 0;
            return 1;
        }
        if (op->sf_stage == 0) {                      /* head SEND (partial-capable) */
            op->send_done += (size_t)res;
            if (op->send_done < op->send_total) { iou_prep_send_tail(st, op); return 0; }
            op->sent_total = op->send_total;          /* head bytes accounted */
            iou_send_release(st, op);                 /* head done: release its buffer */
            if (op->file_off >= op->file_count) {     /* empty body: response complete */
                ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;
                ev->bytes = op->sent_total; return 1;
            }
            if (iou_open_pipe(op) < 0) {              /* splice pipe */
                ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 0; return 1;
            }
            iou_prep_splice_in(st, op);               /* → stage 1 */
            return 0;
        }
        if (op->sf_stage == 1) {                      /* file → pipe (res = bytes buffered) */
            if (res == 0) {                           /* short/empty file: done */
                ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;
                ev->bytes = op->sent_total; return 1;
            }
            op->file_off += (uint64_t)res;
            op->pipe_len = (size_t)res;
            iou_prep_splice_out(st, op);              /* → stage 2 */
            return 0;
        }
        /* sf_stage == 2: pipe → socket (res = bytes delivered to the socket) */
        if (res == 0) {                               /* socket closed mid-transfer: stop */
            ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;
            ev->bytes = op->sent_total; return 1;
        }
        op->pipe_len -= (size_t)res;
        op->sent_total += (size_t)res;
        if (op->pipe_len > 0) { iou_prep_splice_out(st, op); return 0; }  /* drain the pipe */
        if (op->file_off < op->file_count) { iou_prep_splice_in(st, op); return 0; }  /* next chunk */
        ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;     /* file fully sent */
        ev->bytes = op->sent_total;
        return 1;

    case IOU_DGRAM_RECV:
        /* The buffer + flags were COPIED at post; the token ref pins the buffer past this op. Transfer
         * the token ref op → event (released after dispatch); NULL op->life so iou_op_free does not
         * double-release. Never dereference the transport owner. */
        ev->kind = KL_COMP_DGRAM_RECV;
        ev->life = op->life; op->life = NULL;
        ev->ok = (res >= 0);
        ev->bytes = (res > 0) ? (size_t)res : 0;
        ev->buf = op->buf;
        if (res >= 0 && op->msgh.msg_namelen > 0) /* source: native → neutral at the seam */
            (void)kl_sockaddr_from_native(&ev->peer, (struct sockaddr *)&op->peer,
                                          op->msgh.msg_namelen);
        if (res >= 0 && op->dg_pktinfo) {        /* local (dest) addr via pktinfo cmsg */
            struct sockaddr_storage local_ss;
            socklen_t local_len = kl_udp_parse_local(&op->msgh, &local_ss);
            if (local_len)
                (void)kl_sockaddr_from_native(&ev->local,
                                              (struct sockaddr *)&local_ss, local_len);
        }
        ev->tos = -1;                            /* default "none"; parse only if requested */
        if (res >= 0) {
            if (op->dg_gro)                      /* GRO coalesced segment size */
                ev->gro_seg = kl_udp_parse_gro(&op->msgh);
            if (op->dg_tos)                      /* received TOS/Traffic-Class byte */
                ev->tos = kl_udp_parse_tos(&op->msgh);
            if (op->msgh.msg_flags & MSG_TRUNC)   /* datagram truncated to recv_buf */
                ev->truncated = 1;
        }
        return 1;

    case IOU_DGRAM_SEND:
        ev->kind = KL_COMP_DGRAM_SEND;
        ev->life = op->life; op->life = NULL;      /* transfer token ref op → event */
        ev->ok = (res >= 0);
        ev->bytes = (res > 0) ? (size_t)res : 0;
        return 1;

    case IOU_CONNECT:
        /* Connect finished. res == 0 → connected; res < 0 → -errno (e.g. -ECONNREFUSED).
         * Surface KL_COMP_CONNECT against the client's tagged watcher (aborted ops are dropped
         * in the drain loop, before this). io_uring reports the result in `res`, and SO_ERROR is
         * NOT reliably preserved after a failed IORING_OP_CONNECT, so encode the result in the
         * delivered mask (KL_EVENT_WRITE = connected, 0 = failed); the client's connect watcher
         * trusts this rather than re-reading SO_ERROR. */
        ev->kind = KL_COMP_CONNECT;
        ev->target = op->watcher_udata;
        ev->ok = (res == 0);
        ev->bytes = ev->ok ? (size_t)KL_EVENT_WRITE : 0;
        return 1;

    case IOU_WATCH:                                   /* handled in the drain loop */
        return 0;
    }
    return 0;
}

static int iou_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    KlIouState *st = ctx->loop._backend;

    struct __kernel_timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        tsp = &ts;
    }

    /* Submit the queued SQEs and wait for at least one completion (or the timeout). */
    struct io_uring_cqe *cqe = NULL;
    int r = io_uring_submit_and_wait_timeout(&st->ring, &cqe, 1, tsp, NULL);
    if (r < 0 && r != -ETIME && r != -EINTR)
        return -1;

    int count = 0;
    unsigned seen = 0, head;
    io_uring_for_each_cqe(&st->ring, head, cqe) {
        if (count >= max) break;                      /* leave the rest for the next drain */
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        int res = cqe->res;
        seen++;
        /* Skip sentinels: our cancel / poll_remove CQEs (user_data 0), and the internal
         * IORING_OP_TIMEOUT that submit_and_wait_timeout injects (LIBURING_UDATA_TIMEOUT
         * == (uint64_t)-1); dereferencing either as an op would crash. */
        if (ud == 0 || ud == (uint64_t)-1) continue;
        void *data = (void *)(uintptr_t)ud;

        if (*(IouOpType *)data == IOU_WATCH) {
            KlIouWatch *w = data;
            w->armed = 0;                             /* this single-shot poll is consumed */
            if (w->removed) {                         /* del's poll-cancel CQE: unlink + free */
                for (KlIouWatch **link = &st->watches; *link; link = &(*link)->next)
                    if (*link == w) { *link = w->next; break; }
                kl_free(st->alloc, w, sizeof(*w));
                continue;
            }
            if (res > 0) {                            /* poll fired: surface a watcher event */
                KlEventMask ready = 0;
                if (res & (POLLIN | POLLHUP | POLLERR)) ready |= KL_EVENT_READ;
                if (res & POLLOUT) ready |= KL_EVENT_WRITE;
                memset(&out[count], 0, sizeof(out[count]));
                out[count].kind = KL_COMP_WATCHER;
                out[count].target = w->udata;         /* the tagged KlWatcher */
                out[count].bytes = (size_t)ready;      /* carry the ready mask */
                out[count].ok = 1;
                count++;
            }
            /* No re-arm here: kl_event_dispatch → kl_watcher_rearm → kl_event_mod re-arms
             * the single-shot poll after the callback runs (the one-shot-backend contract). */
            continue;
        }

        KlIouOp *op = data;
        /* An aborted outbound connect: the client dropped this attempt (loser / failure
         * / deadline / cancel) via kl_comp_cancel, which marked it aborted + prep_cancel; its
         * (detached) watcher is already gone. This is the -ECANCELED CQE; free the op WITHOUT
         * dispatching against the freed watcher. (The kernel is done with op->peer now.) */
        if (op->type == IOU_CONNECT && op->aborted) {
            iou_op_unlink(st, op);
            iou_op_free(op);
            continue;
        }
        if (iou_complete(st, op, res, &out[count])) {  /* op finished → emit + free */
            count++;
            iou_op_unlink(st, op);
            iou_op_free(op);
        }
        /* else: partial write re-prepared; op stays in-flight, no event */
    }
    io_uring_cq_advance(&st->ring, seen);
    return count;
}

/* Teardown accept-side force-completion. Does NOT reap; the server drives the
 * drain (kl_comp_run) so every dequeued CQE gets its normal routing (no completion lost). This only
 * GUARANTEES that every posted accept will complete: it UNCONDITIONALLY submits a cancel for each
 * IOU_ACCEPT (not relying on iou_comp_cancel having obtained an SQE: a full SQ could have skipped
 * one; a duplicate cancel is harmless, its CQE is an ignored NULL-data sentinel), ensuring an SQE
 * for each even under SQ pressure (iou_sqe flushes + retries; one more submit+get as a backstop),
 * then flushes. Returns 0, or -1 if a cancel SQE genuinely could not be obtained (the server then
 * skips the reap loop rather than block forever). */
static int iou_shutdown_accepts(struct KlEventCtx *ctx) {
    KlIouState *st = ctx->loop._backend;
    for (KlIouOp *o = st->ops; o; o = o->next) {
        if (o->type != IOU_ACCEPT) continue;
        o->aborted = 1;
        struct io_uring_sqe *sqe = iou_sqe(st);          /* get_sqe, else submit + retry */
        if (!sqe) {
            if (io_uring_submit(&st->ring) < 0) return -1;   /* flush to free SQ slots */
            sqe = io_uring_get_sqe(&st->ring);
            if (!sqe) return -1;                          /* truly saturated: cannot guarantee */
        }
        io_uring_prep_cancel(sqe, o, 0);
        io_uring_sqe_set_data(sqe, NULL);                /* ignored cancel-completion sentinel */
    }
    /* Flush until EVERY queued SQE has actually been submitted; io_uring_submit may submit fewer
     * than are queued, so the guarantee is "sq_ready() reached 0", not "one submit returned >= 0".
     * A submit that makes no progress (<= 0 with entries still pending) means the force cannot be
     * guaranteed → -1, and the caller skips the reap loop rather than block on an un-cancelled op. */
    while (io_uring_sq_ready(&st->ring) > 0)
        if (io_uring_submit(&st->ring) <= 0) return -1;
    return 0;
}

/* ── Completion sub-vtable ─────────────────────────────────────────
 * Group this backend's completion primitives so the dispatch (completion_dispatch.c)
 * reaches them on the compiled-in path (kl_comp_ops_builtin) or through a runtime
 * provider (loop->ops->completion). No behavior change: same funcs, one hop away. */
static const KlCompletionOps iou_completion_ops = {
    iou_comp_drain, iou_comp_prime_accepts, iou_comp_post_recv, iou_comp_post_send,
    iou_comp_post_accept, iou_comp_post_sendfile, iou_comp_cancel,
    iou_comp_post_dgram_recv, iou_comp_post_dgram_send,
    iou_comp_cancel_dgram, iou_comp_retire_dgram, iou_comp_post_connect,
    iou_shutdown_accepts,
};

const KlCompletionOps *kl_comp_ops_builtin(void) { return &iou_completion_ops; }
