/*
 * client_internal.h — shared internals for the split HTTP/1.1 client TUs
 *
 * src/client.c was split (freestanding step B2b) into three translation units
 * so a freestanding *async* client links without dragging in the blocking /
 * hosted sync path (poll()/read()/write()/blocking DNS):
 *
 *   - client_common.c — shared by sync + async: CRLF guard, plain/TLS I/O
 *     abstraction, heap request formatting, header helpers, response
 *     decompression, response free.
 *   - client_sync.c   — the blocking hosted API only (connect_with_timeout,
 *     recv_response_sync, kl_http_client_request[_s], kl_http_client_request_pooled).
 *   - client_async.c  — the event-driven client (Happy Eyeballs, the state
 *     machine, completion connect, kl_http_client_start[_s], kl_http_client_start_pooled).
 *
 * This header is src/-internal (never installed). It carries only the surface
 * that now crosses a TU boundary: the KlHttpClient struct, the streaming
 * decompression wrapper, and declarations for the former-static helpers shared
 * across the split. Keep it minimal.
 */
#ifndef KEEL_HTTP_CLIENT_INTERNAL_H
#define KEEL_HTTP_CLIENT_INTERNAL_H

#include <keel/http_client.h>
#include <keel/http_client_pool.h>
#include <keel/decompress.h>
#include <keel/http1_parser.h>
#include <keel/resolver.h>
#include <keel/connect_op.h>          /* KlConnectOp — outbound-connect state machine (6C) */
#include <keel/connect_op_detail.h>   /* KlConnectOp layout (embedded by value) */
#include <keel/tls.h>
#include <keel/url.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "socket.h"   /* KlSocketProvider / KlSocketHandle / KlSockAddr */

/* ── Proxy constants ─────────────────────────────────────────────── */

#define KL_PROXY_RESPONSE_MAX 4096

/* ── Streaming decompression wrapper (shared: sync + async) ──────────
 * Wraps the user's streaming callbacks so a matching Content-Encoding is
 * transparently inflated before reaching the user. Embedded by value in the
 * sync path and heap-allocated on the async path. */
typedef struct {
    /* User's original callbacks */
    KlHttpClientBodyFn     user_on_body;
    KlHttpClientHeadersFn  user_on_headers;
    void             (*user_on_complete)(void *user_data);
    void              *user_data;

    /* Decompression state */
    KlDecompressStream  ds;
    int                 active;  /* 1 if decompression is active */
    KlDecompressConfig *dcfg;
} DecompStreamWrap;

/* ══════════════════════════════════════════════════════════════════════
 * Async client state machine (client_async.c) — struct exposed here so the
 * async-only helper build_connect_request (which mutates a KlHttpClient) and the
 * async TU share one definition.
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    KL_HTTP_CLIENT_RESOLVING,
    KL_HTTP_CLIENT_CONNECTING,
    KL_HTTP_CLIENT_PROXY_CONNECTING,   /* sending CONNECT request */
    KL_HTTP_CLIENT_PROXY_HANDSHAKE,    /* reading proxy 200 response */
    KL_HTTP_CLIENT_TLS_HANDSHAKE,
    KL_HTTP_CLIENT_SENDING,
    KL_HTTP_CLIENT_SENDING_STREAM,  /* chunked body from body_read callback */
    KL_HTTP_CLIENT_RECEIVING,
    KL_HTTP_CLIENT_DONE
} KlHttpClientState;

/* One in-flight racing connect socket (Happy Eyeballs). */
typedef struct { KlSocketHandle fd; int active; } KlHttpClientConnectAttempt;

struct KlHttpClient {
    KlSocketHandle     fd;
    KlHttpClientState      state;
    KlEventCtx        *ev_ctx;
    KlAllocator       *alloc;

    /* Request (heap-copied, owned) */
    char              *request_buf;
    size_t             request_len;
    size_t             request_sent;

    /* Response */
    KlHttpClientResponse   resp;
    KlHttp1ResponseParser  *parser;
    KlError            error;

    /* TLS (NULL if plain HTTP) */
    KlTls             *tls;
    KlTlsConfig       *tls_cfg;
    char               host_buf[KL_HTTP_CLIENT_HOSTNAME_MAX];

    /* Async DNS resolver (NULL = sync sync name resolution was used) */
    KlResolver        *resolver;
    KlResolveReq      *resolve_req;
    int                owns_resolver;   /* 1 = auto-created, destroy on teardown */
    const char        *resolve_host;    /* host to resolve (borrowed; valid through the request) — 6C */
    int                resolve_port;    /* port for the resolve (6C) */
    int                connect_start_failed; /* 6C: resolver->resolve() could not start — the setup
                                              * returns NULL with no user callback (see cli_co_on_done) */

    /* Happy Eyeballs — racing connect over the resolved address list (RFC 8305), driven by the
     * KlConnectOp state machine (6C). Only active on the async resolver path (conn_racing=1); the
     * UNIX and sync-name-resolution paths stay single-fd. The client owns the idx->fd map
     * (conn_attempts) + the idx->addr map (conn_addrs); KlConnectOp owns the cursor / pending /
     * per-attempt-active / terminal-once / detachment state (its old conn_next/conn_pending/
     * conn_last_err duplicates are gone). The overall request deadline stays CLIENT-owned (it must
     * outlive the connect terminal to bound TLS/send/recv), so it is NOT a KlConnectOp timer. */
    KlConnectOp        connect_op;      /* outbound-connect state machine (async HE path) */
    KlHttpClientConnectAttempt      conn_attempts[KL_RESOLVE_MAX_ADDRS];  /* idx -> racing fd */
    KlResolveResult    conn_addrs;      /* full list, copied from the resolver (idx -> addr) */
    int                conn_racing;     /* 1 = HE attempts in conn_attempts[] */
    int64_t            conn_delay_timer;/* Connection Attempt Delay timer (KlConnectOp-armed) (-1) */
    int64_t            deadline_timer;  /* overall request deadline timer (-1) */
    int                timeout_ms;      /* overall deadline (0 = none) */
    int                connect_delay_ms;/* Connection Attempt Delay */
    KlError            conn_last_err;   /* last connect error, for the all-fail case */

    /* Completion callback */
    KlHttpClientDoneFn     on_done;
    void              *user_data;

    /* Request streaming (chunked body send) */
    KlHttpClientReadFn     body_read;
    void              *stream_user_data;
    char               chunk_buf[KL_HTTP_CLIENT_CHUNK_BUF_SIZE];
    size_t             chunk_len;    /* bytes in chunk_buf to send */
    size_t             chunk_sent;   /* bytes of chunk_buf already sent */
    int                chunk_phase;  /* 0=read, 1=send hdr, 2=send data, 3=send crlf, 4=eof */
    char               chunk_hdr[KL_HTTP_CLIENT_CHUNK_HDR_SIZE];
    size_t             chunk_hdr_len;
    size_t             chunk_hdr_sent;

    /* Pool integration (NULL = legacy close-on-complete) */
    KlHttpClientPool      *pool;
    KlHttpClientPoolConn   pool_conn;
    int                pool_port;
    int                pool_is_tls;

    /* Response decompression */
    KlDecompressConfig *decompress_cfg;
    DecompStreamWrap   *decomp_wrap;     /* heap-allocated for streaming */

    /* Proxy state */
    int             is_proxied;     /* connected via proxy */
    int             is_tunnel;      /* CONNECT tunnel (HTTPS through proxy) */
    char           *connect_buf;    /* CONNECT request buffer (heap) */
    size_t          connect_len;
    size_t          connect_sent;
    char           *proxy_recv;     /* CONNECT response buffer (heap) */
    size_t          proxy_recv_len;
    const char     *proxy_auth;     /* borrowed from config */
    uint16_t        target_port;    /* original target port for CONNECT */
};

/* ══════════════════════════════════════════════════════════════════════
 * Shared helpers (client_common.c) — used by both sync + async TUs.
 * ══════════════════════════════════════════════════════════════════════ */

/* CRLF injection guard: 1 if s[0..len) contains CR or LF. */
int kl_http_client_has_crlf(const char *s, size_t len);

/* Plain-or-TLS I/O abstraction over the socket provider. Returns kl_ssize_t
 * (pointer-width, freestanding) so the async client's I/O locals stay errno-free. */
kl_ssize_t kl_http_client_io_write(const KlSocketProvider *p, KlSocketHandle fd,
                              KlTls *tls, const void *buf, size_t len);
kl_ssize_t kl_http_client_io_read(const KlSocketProvider *p, KlSocketHandle fd,
                             KlTls *tls, void *buf, size_t len);

/* Heap request formatting (buffered + chunked-headers-only forms). */
char *kl_http_client_build_request(KlAllocator *alloc,
                              const char *method, const KlUrl *url,
                              const KlHttpClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              size_t *out_len, int keep_alive,
                              const char *absolute_url);
char *kl_http_client_build_request_headers_only(KlAllocator *alloc,
                                           const char *method, const KlUrl *url,
                                           const KlHttpClientHeader *headers,
                                           int num_headers, size_t *out_len,
                                           int keep_alive,
                                           const char *absolute_url);

/* Response header helpers. */
const char *kl_http_client_find_header_value(const KlHttpClientResponse *resp,
                                        const char *name);
void kl_http_client_remove_header(KlHttpClientResponse *resp, const char *name);

/* 1 if the response carries "Connection: close". */
int kl_http_client_server_wants_close(const KlHttpClientResponse *resp);

/* Post-process a buffered response: inflate the body if Content-Encoding
 * matches the decompressor. Returns 0 on success / no-op, -1 on error. */
int kl_http_client_decompress_response_body(KlHttpClientResponse *resp,
                                       KlDecompressConfig *dcfg);

/* Streaming decompression wrapper callbacks (installed on the response parser
 * so a matching Content-Encoding is inflated transparently). */
int  kl_http_client_decomp_on_body(const char *data, size_t len, void *user_data);
int  kl_http_client_decomp_on_headers(int status, const KlHttpClientHeader *headers,
                                 int num_headers, void *user_data);
void kl_http_client_decomp_on_complete(void *user_data);

#endif /* KEEL_HTTP_CLIENT_INTERNAL_H */
