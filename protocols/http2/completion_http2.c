/*
 * completion_http2.c — HTTP/2-over-completion leg of the split completion driver (B2a).
 * Extracted verbatim from completion_driver.c: the driver-owned h2 output capture and
 * the h2 connection drive. Reaches back into the server TU for kl_comp_close /
 * kl_comp_tls_drain_output (completion_internal.h); reuses the h2 session vtable +
 * kl_http2_server_feed verbatim — no IOCP/pollcomp symbol appears here.
 */
#include <keel/http_server.h>
#include <keel/http_connection.h>
#include "http_internal.h"            /* kl_http2_server_feed / kl_http2_server_set_writer */
#include "http2_internal.h"
#include "completion_http.h"     /* kl_comp_post_send / post_recv (HTTP wrappers) — pulls completion.h */
#include "completion_internal.h" /* kl_comp_close / kl_comp_tls_drain_output */
#include "http_proto_hooks.h"         /* completion-drive seam registration */
#include <string.h>
#include <stdint.h>              /* SIZE_MAX (h2 output capture growth guard) */

/* Driver-owned h2 output capture (8d-4): the completion driver installs this writer via
 * the h2 output seam (kl_http2_server_set_writer) around a feed, so the session's produced
 * frames land in one buffer the driver posts as a single overlapped send. The buffer +
 * grow logic live here, not h2.c — h2.c only exposes the generic writer seam. */
typedef struct { KlAllocator *alloc; char *buf; size_t len, cap; int err; } CompH2Cap;
static ssize_t comp_h2_capture_write(void *ctx, const void *data, size_t len) {
    CompH2Cap *cp = ctx;
    if (cp->err) return -1;
    if (len > SIZE_MAX - cp->len) { cp->err = 1; return -1; }
    if (cp->len + len > cp->cap) {
        size_t ncap = cp->cap ? cp->cap : 4096;
        while (ncap < cp->len + len) {
            if (ncap > SIZE_MAX / 2) { cp->err = 1; return -1; }
            ncap *= 2;
        }
        char *nb = kl_realloc(cp->alloc, cp->buf, cp->cap, ncap);
        if (!nb) { cp->err = 1; return -1; }
        cp->buf = nb;
        cp->cap = ncap;
    }
    memcpy(cp->buf + cp->len, data, len);
    cp->len += len;
    return (ssize_t)len;
}

/* Drive an established HTTP/2 connection over the completion loop (8d-1). Feed received
 * plaintext to the h2 session via kl_http2_server_feed (which parses frames and flushes
 * produced output through conn_write — a synchronous blocking send for plaintext, the
 * memory-BIO ring for TLS), then read more. The h2 session vtable and kl_http2_server_feed
 * are reused verbatim — this only inverts the transport, exactly as the HTTP/1.1 path
 * does. For TLS the received ciphertext was already fed to the engine (kl_comp_drain);
 * loop on pending() so coalesced records aren't stranded. */
void kl_comp_http2_drive(struct KlHttpServer *s, KlHttpConn *c) {
    if (c->tls) {
        /* Decrypt + feed every currently-available record (the h2 session writes its
         * output ciphertext into the memory-BIO out ring via conn_write→tls->write),
         * then drain that ring and post it as ONE ordered overlapped send (8d-3) —
         * deferring the next recv to comp_on_write, so at most one h2 send is in flight
         * and frames cannot reorder. */
        for (;;) {
            ssize_t p = c->tls->read(c->tls, c->stream.fd, c->stream.read_buf, c->stream.read_cap);
            if (p < 0) { kl_comp_close(s, c); return; }
            if (p == 0) break;                         /* WANT_READ — batch done */
            KlHttpConnState st = kl_http2_server_feed(c, c->stream.read_buf, (size_t)p);
            if (st != KL_HTTP_CONN_HTTP2) { kl_comp_close(s, c); return; }
            if (!c->tls->pending || c->tls->pending(c->tls) == 0) break;
        }
        unsigned char *cipher = NULL;
        size_t clen = 0, ccap = 0;
        if (kl_comp_tls_drain_output(c, &cipher, &clen, &ccap) < 0) { kl_comp_close(s, c); return; }
        if (clen > 0) {
            KlIoVec iov = { cipher, clen };
            int rc = kl_comp_post_send(c, &iov, 1, clen);
            kl_free(c->stream.alloc, cipher, ccap);
            if (rc < 0) kl_comp_close(s, c);
        } else {
            kl_free(c->stream.alloc, cipher, ccap);
            if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
        }
        return;
    }
    /* Plaintext: the received frame bytes are already in read_buf (comp_on_read added
     * this recv's bytes). Capture the session's output (8d-3) so all frames it produces
     * this feed go out as ONE ordered overlapped send; defer the next recv until that
     * send completes (comp_on_write) so at most one h2 send is ever in flight — frames
     * must not reorder. */
    CompH2Cap cap = { c->stream.alloc, NULL, 0, 0, 0 };
    kl_http2_server_set_writer(c, comp_h2_capture_write, &cap);
    KlHttpConnState st = kl_http2_server_feed(c, c->stream.read_buf, c->stream.read_len);
    kl_http2_server_set_writer(c, NULL, NULL);       /* restore the default socket writer */
    c->stream.read_len = 0;
    if (st != KL_HTTP_CONN_HTTP2 || cap.err) {
        kl_free(c->stream.alloc, cap.buf, cap.cap);
        kl_comp_close(s, c);
        return;
    }
    if (cap.len > 0) {
        KlIoVec iov = { cap.buf, cap.len };
        int rc = kl_comp_post_send(c, &iov, 1, cap.len);
        kl_free(c->stream.alloc, cap.buf, cap.cap);
        if (rc < 0) kl_comp_close(s, c);
    } else {
        kl_free(c->stream.alloc, cap.buf, cap.cap);
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);   /* no output — read more */
    }
}

/* Completion-drive seam registration (http_proto_hooks.h): completion_http_server.c reaches
 * HTTP/2-over-completion only through this table. The installer (called by
 * completion_http_server.c) registers it and pulls this object out of the archive. */
static const KlHttp2CompHooks kl_http2_comp_hooks_table = { .drive = kl_comp_http2_drive };

void kl_http2_comp_hooks_install(void) {
    kl_http2_comp_hooks_set(&kl_http2_comp_hooks_table);
}
