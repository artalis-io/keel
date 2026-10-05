/*
 * http_response_internal.h: INTERNAL. Response serialization for the completion driver.
 *
 * kl_http_response_send() serializes a buffered response (status line + headers +
 * Content-Length + Connection + CRLF + body) into an iovec and writes it with the
 * synchronous seam writev. The IOCP completion driver cannot use that synchronous
 * path; it must post the same bytes via overlapped WSASend. This exposes the
 * iovec assembly (the single source of truth for the byte layout) so the driver
 * builds the identical response and posts it itself. See docs/archive/phases/phase8_iocp_design.md.
 */
#ifndef KEEL_SRC_HTTP_RESPONSE_INTERNAL_H
#define KEEL_SRC_HTTP_RESPONSE_INTERNAL_H

#include <keel/http_response.h>
#include <keel/socket.h>   /* KlIoVec */
#include <stddef.h>

/* Assemble the full wire bytes of a buffered response (KL_HTTP_BODY_BUFFER/KL_HTTP_BODY_NONE)
 * into iov[0..return). `cl_buf` is caller-owned scratch (>= 48 bytes) that holds the
 * formatted Content-Length line; the returned iov may point into it, so it must
 * outlive the caller's use of iov. Sets *total_out to the byte total. Returns the
 * iovec count (<= cap), or -1 if the mode is not buffer/none or cap is too small
 * (needs 7). The single source of truth shared with kl_http_response_send(). */
int kl_http_response_build_iovec(KlHttpResponse *res, KlIoVec *iov, int cap,
                            char *cl_buf, size_t cl_buf_cap, size_t *total_out);

/* A streamed plaintext response on a completion loop: hand the bytes to the connection's output
 * queue (completion_http_server.c) instead of sending them on the loop thread. Called by the
 * response's outbound-buffer writer only for a response bound to a completion-driven connection
 * (its socket provider is overlapped). Bytes taken (all of them), 0 to leave them buffered
 * (backpressure), or -1. */
kl_ssize_t kl_http_comp_stream_write(KlHttpResponse *res, const char *data, size_t len);

/* A streamed TLS response: after the engine took a write, move its output onto a completion-driven
 * connection's output queue and start sending it, as conn_write does (a chunk written from a timer
 * while the connection is suspended would otherwise wait in the engine for the resume). 0 when
 * there is nothing to do (readiness, a response that is not a pooled connection's, the driver
 * already moving it), else 0 or -1 from the flush. */
int kl_http_comp_stream_tls_flush(KlHttpResponse *res);

/* Before a streamed TLS write on a completion loop: 1 when the connection's output queue holds as
 * much as a producer may add (the write is then refused as would-block, and the response's drain
 * buffers it), else 0. Always 0 on readiness. */
int kl_http_comp_stream_tls_full(KlHttpResponse *res);

#endif /* KEEL_SRC_HTTP_RESPONSE_INTERNAL_H */
