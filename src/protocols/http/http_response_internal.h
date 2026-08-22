/*
 * http_response_internal.h — INTERNAL. Response serialization for the completion driver.
 *
 * kl_http_response_send() serializes a buffered response (status line + headers +
 * Content-Length + Connection + CRLF + body) into an iovec and writes it with the
 * synchronous seam writev. The IOCP completion driver cannot use that synchronous
 * path — it must post the same bytes via overlapped WSASend. This exposes the
 * iovec assembly (the single source of truth for the byte layout) so the driver
 * builds the identical response and posts it itself. See docs/phase8_iocp_design.md.
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

#endif /* KEEL_SRC_HTTP_RESPONSE_INTERNAL_H */
