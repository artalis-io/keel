/*
 * s4_link_stubs.c — server-only link residuals for the S-4 EFI_TCP4 HTTP server.
 *
 * u1_link_stubs.c covers the CLIENT residuals (the connect/recv/send seam fallbacks,
 * the event/completion builtins, and the vendored-llhttp abort/fprintf/__chkstk). The
 * SERVER seam additionally references:
 *
 *   - the bind / listen / reuseaddr + writev / sendfile / cork / tcp_nodelay
 *     DEFAULT-provider fallbacks. The EFI_TCP4 provider always supplies these ops, so
 *     the kl_sockdef_* else-branch of the seam inline is never taken — but the symbol
 *     must still resolve under -nostdlib (the compiler emits the reference). Fail-closed.
 *   - the file-response platform hooks (response.c's kl_http_response_send/reset/free). The
 *     EFI server serves only in-memory (borrowed) bodies — no kl_http_response_file — so
 *     these never run; fail-closed.
 *   - _fltused: the PE/COFF floating-point marker the backend emits for connection.c's
 *     access-log double. The CRT/EDK2 supplies it on a real target; here a plain int.
 *
 * A later phase (S-6 file responses / S-7 teardown) can promote any of these to real
 * EFI implementations; for the plaintext GET / -> 200 boot they are inert.
 */

#include "../../src/socket.h"     /* KlSocketHandle / KlSockAddr / KlIoVec + kl_sockdef_* */
#include "../../src/platform.h"   /* kl_plat_file_pread / kl_plat_file_close */

/* ── server-side socket seam fallbacks — never reached (EFI provider supplies them) ── */
int kl_sockdef_set_reuseaddr(KlSocketHandle f, int on) { (void)f; (void)on; return -1; }
int kl_sockdef_set_tcp_nodelay(KlSocketHandle f, int on) { (void)f; (void)on; return -1; }
int kl_sockdef_set_cork(KlSocketHandle f, int on) { (void)f; (void)on; return -1; }
int kl_sockdef_bind(KlSocketHandle f, const KlSockAddr *a) { (void)f; (void)a; return -1; }
int kl_sockdef_listen(KlSocketHandle f, int backlog) { (void)f; (void)backlog; return -1; }
/* kl_sockdef_accept moved to u1_link_stubs.c: event_efi.c (the shared completion provider)
 * references kl_sock_accept in el_drain, so the fail-closed default is a CLIENT residual too —
 * not a server-only one — else U-3/U-7 (which link only u1_link_stubs.c) fail to link. */
ssize_t kl_sockdef_writev(KlSocketHandle f, const KlIoVec *iov, int iovcnt) {
    (void)f; (void)iov; (void)iovcnt; return -1;
}
ssize_t kl_sockdef_sendfile(KlSocketHandle out_fd, int in_fd, uint64_t *offset, size_t count) {
    (void)out_fd; (void)in_fd; (void)offset; (void)count; return -1;
}

/* ── file-response platform hooks — the EFI server serves in-memory bodies only ── */
int  kl_plat_file_pread(int fd, void *buf, size_t count, long long offset) {
    (void)fd; (void)buf; (void)count; (void)offset; return -1;
}
void kl_plat_file_close(int fd) { (void)fd; }

/* ── stop-wakeup self-pipe hook (S-7) — never reached on a freestanding EFI server ──
 * kl_server_free's self-pipe teardown references kl_plat_wakeup_close, but a freestanding
 * server has no self-pipe (stop_wake_rd stays KL_INVALID_SOCKET — the wakeup init is
 * #ifndef KEEL_FREESTANDING), so the block is skipped and this stub is never called. It
 * only has to resolve the link. */
void kl_plat_wakeup_close(KlPlatWakeup *w) { (void)w; }

/* ── PE/COFF floating-point marker (connection.c access-log double) ── */
int _fltused = 0;
