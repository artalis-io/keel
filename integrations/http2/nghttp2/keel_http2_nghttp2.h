/*
 * keel_http2_nghttp2.h: nghttp2 backend for Keel's HTTP/2 session vtables.
 *
 * Implements the client (KlHttp2ClientSession, include/keel/http2_client.h) and
 * server (KlHttp2ServerSession, include/keel/http2_server.h) session vtables on top
 * of nghttp2. nghttp2 types never appear here or in any Keel core header; the
 * whole nghttp2 dependency is confined to the adapter .c files.
 *
 * Bring-your-own nghttp2 (>= 1.57 for the size_t "*2" API; validated against
 * 1.64). Nothing here is vendored or downloaded.
 *
 * Client usage:
 *   KlHttp2ClientConfig cfg = {
 *       .tls     = &my_tls_cfg,
 *       .session = kl_http2_nghttp2_client_session,   // factory
 *   };
 *   KlHttp2ClientConn *c = kl_http2_client_connect(ev, &alloc, &cfg, url, on_err, ud);
 *
 * Server usage:
 *   KlHttp2ServerConfig h2 = { .factory = kl_http2_nghttp2_server_session };
 *   // wire into the server's HTTP/2 upgrade path.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_HTTP2_NGHTTP2_H
#define KEEL_HTTP2_NGHTTP2_H

#include <keel/allocator.h>
#include <keel/http2_client.h>
#include <keel/http2_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Client-side HTTP/2 session factory (matches KlHttp2ClientSessionFactory).
 *
 * Requests are issued with :scheme "https" (the KEEL h2 client is TLS-oriented;
 * h2c is out of scope for this adapter).
 *
 * @return A ready KlHttp2ClientSession, or NULL on allocation / nghttp2 failure.
 */
KlHttp2ClientSession *kl_http2_nghttp2_client_session(KlAllocator *alloc);

/**
 * @brief Server-side HTTP/2 session factory (matches KlHttp2ServerSessionFactory).
 *
 * @param alloc     Allocator (must outlive the session).
 * @param callbacks KEEL-provided callbacks (on_request/on_data/.../send).
 * @param user_data Opaque pointer passed back to @p callbacks.
 * @return A ready KlHttp2ServerSession, or NULL on allocation / nghttp2 failure.
 */
KlHttp2ServerSession *kl_http2_nghttp2_server_session(KlAllocator *alloc,
                                                KlHttp2ServerCallbacks *callbacks,
                                                void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_HTTP2_NGHTTP2_H */
