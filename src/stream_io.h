#ifndef KEEL_SRC_STREAM_IO_H
#define KEEL_SRC_STREAM_IO_H

/*
 * stream_io.h — raw (non-TLS) I/O over the embedded KlStream. Substrate.
 *
 * Thin inlines that read the socket provider off a KlStream and perform raw
 * recv/send/peek + io-status classification via the socket seam. Generic
 * transport primitives — no protocol knowledge. The TLS-aware connection I/O
 * (conn_read/conn_write on a KlHttpConn) is an HTTP adapter ABOVE these and
 * lives in protocols/http/http_internal.h.
 */

#include <keel/stream.h>
#include <keel/stream_detail.h>   /* KlStream layout: fd, ctx */
#include <keel/event_ctx.h>       /* KlEventCtx (hot path reads ctx->sockets) */
#include "socket.h"               /* kl_sock_recv/send/recv_peek/io_status */

/* The socket provider for a stream (ctx->sockets; NULL = POSIX fast path). */
static inline const KlSocketProvider *kl_stream_provider(const KlStream *s) {
    return s->ctx ? s->ctx->sockets : NULL;
}
static inline ssize_t kl_stream_recv(const KlStream *s, void *buf, size_t len) {
    return kl_sock_recv(kl_stream_provider(s), s->fd, buf, len);
}
static inline ssize_t kl_stream_send(const KlStream *s, const void *buf, size_t len) {
    return kl_sock_send(kl_stream_provider(s), s->fd, buf, len);
}
static inline ssize_t kl_stream_recv_peek(const KlStream *s, void *buf, size_t len) {
    return kl_sock_recv_peek(kl_stream_provider(s), s->fd, buf, len);
}
/* Classify the last raw stream op's status (would-block / interrupted / etc.). */
static inline KlIoStatus kl_stream_io_status(const KlStream *s) {
    return kl_sock_io_status(kl_stream_provider(s));
}

#endif /* KEEL_SRC_STREAM_IO_H */
