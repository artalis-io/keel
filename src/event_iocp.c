/*
 * event_iocp.c — Windows IOCP event backend (PAL Phase 8, completion axis).
 *
 * The first backend on a *completion* event model: sockets are associated with a
 * completion port once, and the kernel reports finished overlapped operations
 * (not readiness). This TU is compiled ONLY on Windows under `BACKEND=iocp`
 * (Makefile selection — no #ifdef in shared code), so the completion concept never
 * touches the POSIX/readiness TUs or Keel's public API. See
 * docs/phase8_iocp_design.md.
 *
 * Increment 2 (this file): the KlEventLoop lifecycle over an IOCP port + the
 * completion capability + the overlapped socket provider. The completion tick and
 * connection driver (accept/read/write over WSARecv/WSASend, driven through an
 * internal io_engine seam) land in the next increment; until then the readiness
 * kl_event_wait is a no-op on this loop (a completion loop is driven by that tick,
 * not by kl_event_wait — see the note on kl_event_wait below).
 */
#include <keel/event.h>
#include "event_caps.h"
#include "io_engine.h"       /* kl_io_engine_run_completion (the completion tick) */
#include "socket.h"          /* KlSocketProvider/KlSocketOps + internal KL_SOCK_CAP_OVERLAPPED */

#include "sockcompat.h"      /* winsock2.h (pulls the Win32 base types IOCP needs) */
#include <windows.h>         /* CreateIoCompletionPort / GetQueuedCompletionStatusEx / HANDLE */

/* The loop's IOCP port handle is stashed in loop->_backend (pointer-width); no
 * heap state is needed for the port itself. loop->fd is unused (-1), mirroring the
 * io_uring backend which also has no pollable fd. */

int kl_event_init(KlEventLoop *loop) {
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (port == NULL)
        return -1;
    loop->_backend = (void *)port;
    loop->fd = -1;
    return 0;
}

int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)mask;   /* completion model: no readiness mask — I/O is posted, not armed */
    if (!kl_handle_valid(fd))
        return -1;
    /* Associate the socket with the port; the completion key carries udata (the
     * KlConn*, so a completion maps straight back to its connection). */
    HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd,
                                      (HANDLE)loop->_backend,
                                      (ULONG_PTR)udata, 0);
    return h ? 0 : -1;
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    /* No readiness re-arm under completion: the driver re-posts overlapped ops to
     * express interest. A no-op keeps the shared server transition code (which
     * calls kl_event_mod) working unchanged across both models. */
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    /* IOCP has no de-association API; a socket leaves the port when it is closed.
     * The driver cancels in-flight ops (CancelIoEx) before close in the next
     * increment. No-op here so shared teardown code composes. */
    (void)loop; (void)fd;
    return 0;
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* A completion loop is driven by the completion tick (kl_iocp_run_tick, next
     * increment) via the internal io_engine seam — NOT by this readiness call. It
     * is defined (the KlEventLoop API requires it) but reports no readiness events;
     * blocking for the requested timeout avoids a busy-spin if ever called. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0)
        Sleep((DWORD)timeout_ms);
    return 0;
}

void kl_event_close(KlEventLoop *loop) {
    if (loop->_backend) {
        CloseHandle((HANDLE)loop->_backend);
        loop->_backend = NULL;
    }
    loop->fd = -1;
}

/* PAL Phase 8: a completion loop over native OS handles (SOCKETs associated with
 * the port). Advertising COMPLETION makes the Phase 7 negotiation require an
 * OVERLAPPED provider (kl_caps_compatible). */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped socket provider (Phase 8). Control-plane ops (socket/bind/listen/
 * accept/close/setsockopt/getsockname) fall through to the Winsock kl_sockdef_*
 * defaults (NULL ops → default dispatch); Winsock is started by socket_winsock.o's
 * load constructor, which stays linked under BACKEND=iocp. The data plane is driven
 * by the completion loop's overlapped submit path, so its send/recv ops are left as
 * the (unused-in-completion) synchronous defaults. The OVERLAPPED capability is
 * what the negotiation keys on. */
static const KlSocketOps IOCP_OPS = { .name = "iocp" };

static const KlSocketProvider IOCP_PROVIDER = {
    &IOCP_OPS, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED,
};

const KlSocketProvider *kl_socket_provider_iocp(void) { return &IOCP_PROVIDER; }

/* Completion tick — the server delegates one loop iteration here when its event
 * loop is this completion backend (io_engine seam). The accept/read/write driver
 * (AcceptEx + WSARecv/WSASend feeding the connection protocol core) lands in the
 * next increment; until then this reports "not yet driving" rather than silently
 * spinning a server on an unfinished loop. Not exercised by CI yet — the
 * Windows-IOCP job runs only the backend lifecycle/negotiation suite. */
int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {
    (void)s; (void)timeout_ms;
    return -1;
}
