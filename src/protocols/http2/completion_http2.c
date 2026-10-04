/*
 * completion_http2.c - the HTTP/2-over-completion leg of the completion driver: the
 * driver-owned h2 output capture and the h2 connection drive. Reaches back into the server TU for kl_comp_close /
 * kl_comp_tls_flush (completion_internal.h); reuses the h2 session vtable +
 * kl_http2_server_feed verbatim; no IOCP/pollcomp symbol appears here.
 */
#include <keel/http_server.h>
#include <keel/http_connection.h>
#include "http_internal.h"            /* kl_http2_server_feed */
#include "http2_internal.h"
#include "completion_http.h"     /* kl_comp_post_send / post_recv (HTTP wrappers), pulls completion.h */
#include "completion_internal.h" /* kl_comp_close / kl_comp_tls_flush */
#include "http_proto_hooks.h"         /* completion-drive seam registration */

/* Drive an established HTTP/2 connection over the completion loop. Feed received
 * plaintext to the h2 session via kl_http2_server_feed (which parses frames and flushes
 * produced output through conn_write: a synchronous blocking send for plaintext, the
 * memory-BIO ring for TLS), then read more. The h2 session vtable and kl_http2_server_feed
 * are reused verbatim; this only inverts the transport, exactly as the HTTP/1.1 path
 * does. For TLS the received ciphertext was already fed to the engine (kl_comp_drain);
 * read until WANT_READ so coalesced records aren't stranded. */
void kl_comp_http2_drive(struct KlHttpServer *s, KlHttpConn *c) {
    if (c->tls) {
        /* Decrypt + feed every currently-available record (the h2 session writes its
         * output ciphertext into the memory-BIO out ring via conn_write→tls->write),
         * then move that ring onto the connection's output queue (one ordered overlapped send
         * at a time, so frames cannot reorder) and read the next frames once it is out (no new
         * input while output is pending). */
        for (;;) {
            kl_ssize_t p = c->tls->read(c->tls, c->stream.fd, c->stream.read_buf, c->stream.read_cap);
            if (p < 0) { kl_comp_close(s, c); return; }
            if (p == 0) break;                         /* WANT_READ, batch done */
            KlHttpConnState st = kl_http2_server_feed(c, c->stream.read_buf, (size_t)p);
            if (st != KL_HTTP_CONN_HTTP2) {            /* send a GOAWAY the session queued, then close */
                if (kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }
                kl_comp_close_after_output(s, c);
                return;
            }
            /* Read again until WANT_READ: pending() misses whole records held as ciphertext. */
        }
        if (kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }
        kl_comp_recv_after_output(s, c);
        return;
    }
    /* Plaintext: the received frame bytes are already in read_buf (comp_on_read added this recv's
     * bytes). The session writes its frames through conn_write, which on a completion loop puts them
     * on the connection's output queue (one ordered overlapped send at a time, so frames cannot
     * reorder, and frames written between feeds, an upgrade's stream 1 or a drain's GOAWAY, join
     * them in order). Read the next frames once that output is out. */
    KlHttpConnState st = kl_http2_server_feed(c, c->stream.read_buf, c->stream.read_len);
    c->stream.read_len = 0;
    if (st != KL_HTTP_CONN_HTTP2) {                    /* send a GOAWAY the session queued, then close */
        kl_comp_close_after_output(s, c);
        return;
    }
    kl_comp_recv_after_output(s, c);
}

/* Completion-drive seam registration (http_proto_hooks.h): completion_http_server.c reaches
 * HTTP/2-over-completion only through this table. The installer (called by
 * completion_http_server.c) registers it and pulls this object out of the archive. */
static const KlHttp2CompHooks kl_http2_comp_hooks_table = { .drive = kl_comp_http2_drive };

void kl_http2_comp_hooks_install(void) {
    kl_http2_comp_hooks_set(&kl_http2_comp_hooks_table);
}
