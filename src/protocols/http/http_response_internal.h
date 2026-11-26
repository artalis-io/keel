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

/* The flush behind kl_http_response_send(), without its output-ownership check: for the server's own
 * output drivers only. A completion loop's driver flushes a streamed response through it; the public
 * function refuses an application's direct send while that driver owns the response's transmission
 * (kl_http_comp_owns_output). Same return contract as kl_http_response_send(). */
int kl_http_response_send_driven(KlHttpResponse *res);

/* 1 when the completion machinery owns the transmission of this response: it is the one embedded in
 * a pooled server connection (stream_inflight) and that connection is driven by a completion loop
 * (comp_driven). A response has exactly one output driver, so the application must not then
 * initiate direct transmission. Always 0 on readiness, for any other response, and in a build
 * without the completion axis. */
int kl_http_comp_owns_output(const KlHttpResponse *res);

/* 1 while the response has a stream that takes body bytes: begun (its head written), not ended, and
 * not reset or released since. Every stream write, legacy or bounded, compressed or not, is refused
 * when it is 0, so nothing follows the terminating chunk. */
int kl_http_response_stream_open(const KlHttpResponse *res);

/* 1 while kl_http_response_begin_stream() would accept the response: no head sent, no stream begun
 * or ended. Wrappers that add headers first (SSE, compression) check it before touching the
 * response, so a refused begin changes nothing. */
int kl_http_response_stream_can_begin(const KlHttpResponse *res);

/* The built-in chunked stream writer and its scatter-gather form. The latter emits one HTTP chunk
 * whose payload is the concatenation of iov[0..iovcnt). Kept internal so protocol helpers can batch
 * logical output without changing KlHttpResponseWriteFn or the public response ABI. */
#define KL_HTTP_STREAM_IOV_MAX 32
int kl_http_response_stream_write(void *ctx, const char *data, size_t len);
int kl_http_response_stream_writev(KlHttpResponse *res, const KlIoVec *iov, int iovcnt);

/* 1 when a streamed plaintext response's writes belong on an output queue rather than a send on the
 * loop thread: the response is bound to a completion loop (completion_http_server.c). Decided by how
 * the loop drives its connections, never by the socket provider's capability bits: a provider placed
 * on the context without negotiation does not change the drive model. Always 0 on readiness and in
 * a build without the completion axis. */
int kl_http_comp_stream_driven(const KlHttpResponse *res);

/* A streamed plaintext response on a completion loop: hand the bytes to the connection's output
 * queue (completion_http_server.c) instead of sending them on the loop thread. Called by the
 * response's outbound-buffer writer only when kl_http_comp_stream_driven says so. Bytes taken (all
 * of them), 0 to leave them buffered (backpressure), or -1 (also for a response that is not a
 * pooled completion-driven connection's own). */
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

/* The bytes of a streamed response a completion-driven connection's output queue holds and has not
 * seen completed (queued, posted, ciphertext with TLS). 0 on readiness and for a response that is
 * not a pooled completion-driven connection's own. Counted toward the response's stream capacity. */
size_t kl_http_comp_stream_queued(const KlHttpResponse *res);

/* Would kl_http_response_stream_try_writev() take a chunk of `len` payload bytes now? The same
 * checks in the same order, returning the status that write would, with nothing written: ACCEPTED
 * means it fits (nothing taken, the edge left as it was), WOULD_BLOCK arms the writable edge as a
 * refused write does, ERROR is sticky as there. A write of at most `len` bytes made before anything
 * else changes the stream is then ACCEPTED (the framed size grows with the length). For writers
 * that must know a chunk fits before producing it (a compressor's flush). */
KlHttpStreamStatus kl_http_response_stream_admit(KlHttpResponse *res, size_t len);

/* A progress point of a suspended streamed response (its output moved): fire the writable edge if
 * it is armed and the pending bytes are below the capacity. The caller re-validates the connection
 * afterwards: the callback may write, end the stream, complete or cancel the op. */
void kl_http_response_stream_progress(KlHttpResponse *res);

/* Where a stream's body goes when it is not HTTP/1.1 chunked framing on the response's own write
 * target: an HTTP/2 stream's response, whose body the HTTP/2 session frames as DATA
 * (http2_server.c installs one before the handler runs). The response layer keeps its rules (one
 * begin, nothing after the end, all-or-nothing admission against the stream capacity, the writable
 * edge) and hands the sink unframed body bytes. No op calls back into the response or the producer:
 * output moves, the edge fires and the stream closes later, from the loop. */
typedef struct KlHttpStreamSink {
    /* The head (status + the response's headers) is committed. 0, or -1 (the begin is refused). */
    int    (*begin)(void *ctx);
    /* Take `len` body bytes, the concatenation of iov[0..iovcnt). Admission was checked by the
     * caller for a bounded write; the legacy writer passes bounded 0 and the sink then refuses only
     * past its hard cap (the capacity). 0, or -1 (the stream fails). */
    int    (*write)(void *ctx, const KlIoVec *iov, int iovcnt, size_t len, int bounded);
    /* The body is complete. 0, or -1. */
    int    (*end)(void *ctx);
    /* Bytes accepted and not yet taken by the transport. */
    size_t (*pending)(const void *ctx);
} KlHttpStreamSink;

/* The close notification: fire it once and clear it (the HTTP/2 stream is being released). */
void kl_http_response_stream_closed(KlHttpResponse *res, int completed);

#endif /* KEEL_SRC_HTTP_RESPONSE_INTERNAL_H */
