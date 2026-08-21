/*
 * completion_ws.c — WebSocket-over-completion leg of the split completion driver (B2a).
 * Extracted verbatim from completion_driver.c: the WS connection drive. Reaches back into
 * the server TU for kl_comp_close / kl_comp_tls_flush (completion_internal.h); reuses the
 * transport-agnostic WS frame core (kl_ws_server_*) verbatim — no WebSocket-protocol code
 * and no IOCP/pollcomp symbol appears here.
 */
#include <keel/http_server.h>
#include <keel/http_connection.h>
#include <keel/websocket_server.h> /* kl_ws_server_on_readable_data — WS over completion */
#include "completion.h"          /* kl_comp_post_recv */
#include "completion_internal.h" /* kl_comp_close / kl_comp_tls_flush */
#include "http_proto_hooks.h"         /* completion-drive seam registration */
#include <stdint.h>
#include <sys/types.h>           /* ssize_t (TLS read return) — previously pulled
                                    transitively via http_response.h before off_t neutralization */

/* Drive an established WebSocket connection over the completion loop (8e-1). Feed
 * received bytes to the transport-agnostic WS frame core (kl_ws_server_on_readable_data,
 * the analogue of kl_http2_server_feed); its callbacks emit frames through conn_write — the
 * memory-BIO out ring for TLS — which we flush (plus any drain-buffered output). Reuses
 * the WS core + KlTls vtable verbatim: no WebSocket-protocol code and no IOCP/pollcomp
 * symbol appears here, so the completion axis stays out of the WS layer entirely. */
void kl_comp_ws_drive(struct KlHttpServer *s, KlHttpConn *c) {
    if (c->tls) {
        for (;;) {
            ssize_t p = c->tls->read(c->tls, c->stream.fd, c->stream.read_buf, c->stream.read_cap);
            if (p < 0) { kl_comp_close(s, c); return; }
            if (p == 0) {                              /* WANT_READ — need the network */
                if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
                return;
            }
            KlHttpConnState st = (KlHttpConnState)kl_ws_server_on_readable_data(
                                 c, (uint8_t *)c->stream.read_buf, (size_t)p);
            if (st == KL_HTTP_CONN_WEBSOCKET && kl_ws_server_drain_pending(c))
                st = (KlHttpConnState)kl_ws_server_on_writable(c);   /* flush buffered frames */
            if (kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }   /* ring → socket */
            if (st != KL_HTTP_CONN_WEBSOCKET) { kl_comp_close(s, c); return; }
            if (!c->tls->pending || c->tls->pending(c->tls) == 0) {
                if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
                return;
            }
        }
    }
    /* Plaintext: the received frame bytes are already in read_buf; the callbacks emit
     * through conn_write (a synchronous blocking send on this loop). */
    KlHttpConnState st = (KlHttpConnState)kl_ws_server_on_readable_data(
                         c, (uint8_t *)c->stream.read_buf, c->stream.read_len);
    c->stream.read_len = 0;
    if (st == KL_HTTP_CONN_WEBSOCKET && kl_ws_server_drain_pending(c))
        st = (KlHttpConnState)kl_ws_server_on_writable(c);
    if (st != KL_HTTP_CONN_WEBSOCKET) { kl_comp_close(s, c); return; }
    if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
}

/* Completion-drive seam registration (http_proto_hooks.h): completion_http_server.c reaches
 * WebSocket-over-completion only through this table. The installer (called by
 * completion_http_server.c) registers it and pulls this object out of the archive. */
static const KlWsCompHooks kl_ws_comp_hooks_table = { .drive = kl_comp_ws_drive };

void kl_ws_comp_hooks_install(void) {
    kl_ws_comp_hooks_set(&kl_ws_comp_hooks_table);
}
