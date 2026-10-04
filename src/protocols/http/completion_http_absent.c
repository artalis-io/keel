/*
 * completion_http_absent.c: the KEEL_NO_COMPLETION build's HTTP completion stubs.
 *
 * The HTTP leg of the split completion_absent stub: it aborts() every HTTP-typed completion symbol
 * that shared HTTP TUs reference but that is never reached on a readiness loop (the completion
 * branches are gated behind KL_EVENT_CAP_COMPLETION). It mirrors the hosted split (where
 * completion_http_server.c defines these over the neutral kl_comp_*_raw seam) so that under
 * KEEL_NO_COMPLETION the HTTP-typed surface stays in src/protocols/http/ and the substrate stub
 * (src/completion_absent.c) names NO HTTP type. Selected by the Makefile alongside completion_absent.c
 * (COMPLETION_CORE, KEEL_NO_COMPLETION). See docs/archive/freezes/protocols_restructure_freeze.md §4.8.
 *
 * Reaching any of these means the completion gate was bypassed (a build/logic error) so they
 * abort() (fail-loud), exactly like the neutral stubs.
 */
#include "completion_http.h"
#include "http_internal.h"            /* kl_comp_queue_write */
#include "http_response_internal.h"   /* kl_http_comp_stream_write */
#include <stdlib.h>   /* abort */

/* ── Tier 2: HTTP wrappers (normally in completion_http_server.c) ─────────────── */

int kl_comp_post_recv(KlHttpConn *c) { (void)c; abort(); }

int kl_comp_post_send(KlHttpConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    (void)c; (void)iov; (void)iovcnt; (void)total; abort();
}

int kl_comp_prime_accepts(struct KlHttpServer *s) { (void)s; abort(); }

int kl_comp_post_accept(struct KlHttpServer *s) { (void)s; abort(); }

int kl_comp_shutdown_accepts(struct KlHttpServer *s) { (void)s; abort(); }

int kl_comp_post_sendfile(KlHttpConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    (void)c; (void)head_iov; (void)head_n; (void)head_total; (void)file_fd; (void)count;
    abort();
}

/* ── Tier 3: HTTP-only completion orchestration (normally in completion_http_server.c) ─────────────── */

int kl_http_comp_run(struct KlHttpServer *server, int timeout_ms) {
    (void)server; (void)timeout_ms; abort();
}

int kl_http_comp_quiesce_accepts(struct KlHttpServer *server) { (void)server; abort(); }

void kl_http_comp_resume(struct KlHttpServer *server, KlHttpConn *conn) {
    (void)server; (void)conn; abort();
}

void kl_http_comp_post_read(KlHttpConn *conn) { (void)conn; abort(); }

void kl_http_comp_tls_finish(struct KlHttpServer *server, KlHttpConn *conn, int draining) {
    (void)server; (void)conn; (void)draining; abort();
}

/* The output queue: reached only through a completion-driven connection (comp_driven, set at a
 * completion accept) or an overlapped socket provider, neither of which a readiness loop has. */
kl_ssize_t kl_comp_queue_write(KlHttpConn *c, const void *buf, size_t len) {
    (void)c; (void)buf; (void)len; abort();
}

kl_ssize_t kl_http_comp_stream_write(KlHttpResponse *res, const char *data, size_t len) {
    (void)res; (void)data; (void)len; abort();
}
