/*
 * conn_internal.h — INTERNAL. The model-blind connection protocol core (PAL Phase 8).
 *
 * The readiness read path (kl_conn_on_readable) interleaves TRANSPORT (recv into
 * read_buf, buffer growth, TLS-record drain) with the PROTOCOL CORE (parse the
 * received bytes → route → run the handler → build the response). The completion
 * driver (event_iocp.c) delivers bytes a different way (a finished WSARecv fills
 * read_buf) but needs the SAME protocol core afterwards.
 *
 * These are thin, non-static handles onto the existing static helpers in
 * connection.c — exposed so the completion driver can reuse the core WITHOUT the
 * readiness transport wrapper, and WITHOUT the completion concept leaking back into
 * connection.c (which stays model-blind: it never learns whether a readiness recv
 * or a completed WSARecv produced the bytes). kl_conn_on_readable is unchanged, so
 * the readiness path stays byte-identical. See docs/phase8_iocp_design.md §4.
 */
#ifndef KEEL_SRC_CONN_INTERNAL_H
#define KEEL_SRC_CONN_INTERNAL_H

#include <keel/connection.h>
#include <keel/router.h>

/* Headers fully parsed in read_buf: null-terminate, run pre-body path, set up the
 * body reader, and dispatch. `leftover`/`leftover_len` is any body-bytes tail that
 * arrived in the same buffer. Returns the next connection state. */
KlConnState kl_conn_dispatch_request(KlConn *c, KlRouter *router,
                                     const char *leftover, size_t leftover_len);

/* Body fully consumed: run post-body middleware and the handler. Returns the next
 * connection state (typically KL_CONN_SENDING). */
KlConnState kl_conn_run_post_body(KlConn *c, KlRouter *router);

/* Feed `nread` freshly-received request-body bytes (already in read_buf[0..nread])
 * to the chunked decoder / body reader. Returns the next state (KL_CONN_READING_BODY
 * = need more). The model-blind body core, shared by the readiness read path and the
 * completion driver (a completed WSARecv). */
KlConnState kl_conn_ingest_body(KlConn *c, size_t nread);

/* Response fully sent: log access, then keep-alive reset (→ KL_CONN_READING) or
 * close (→ KL_CONN_CLOSED). */
KlConnState kl_conn_send_complete(KlConn *c);

#endif /* KEEL_SRC_CONN_INTERNAL_H */
