/*
 * h2_server.h — Server-side HTTP/2 API
 *
 * Server-side HTTP/2 types and functions. Shared protocol constants
 * (max streams, window size) live in h2.h.
 */

#ifndef KEEL_H2_SERVER_H
#define KEEL_H2_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <keel/h2.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <keel/router.h>

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlH2ServerSession KlH2ServerSession;
typedef struct KlH2ServerCallbacks KlH2ServerCallbacks;
typedef struct KlH2ServerStream KlH2ServerStream;
typedef struct KlH2ServerConn KlH2ServerConn;
typedef struct KlConn KlConn;

/* ── KlH2ServerCallbacks — KEEL provides these to the session ────── */

struct KlH2ServerCallbacks {
    int (*on_request)(void *ud, uint32_t stream_id,
                      const char *method, size_t method_len,
                      const char *path, size_t path_len,
                      const char *authority, size_t authority_len,
                      const char **hdr_names, const char **hdr_values,
                      const size_t *hdr_name_lens, const size_t *hdr_value_lens,
                      int num_headers);
    int (*on_data)(void *ud, uint32_t stream_id, const char *data, size_t len);
    int (*on_stream_end)(void *ud, uint32_t stream_id);
    void (*on_stream_reset)(void *ud, uint32_t stream_id, uint32_t error_code);
    ssize_t (*send)(void *ud, const void *data, size_t len);
};

/* ── KlH2ServerSession — user-provided vtable ───────────────────── */

struct KlH2ServerSession {
    ssize_t (*recv)(KlH2ServerSession *self, const void *data, size_t len);
    int (*submit_response)(KlH2ServerSession *self, uint32_t stream_id,
                           int status, const char **hdr_names,
                           const char **hdr_values, int num_headers,
                           const void *body, size_t body_len);
    int (*want_write)(KlH2ServerSession *self);
    int (*flush)(KlH2ServerSession *self);
    int (*shutdown)(KlH2ServerSession *self);
    void (*destroy)(KlH2ServerSession *self);
};

/* ── Factory ─────────────────────────────────────────────────────── */

typedef KlH2ServerSession *(*KlH2ServerSessionFactory)(KlAllocator *alloc,
                                                        KlH2ServerCallbacks *callbacks,
                                                        void *user_data);

/* ── Config ──────────────────────────────────────────────────────── */

typedef struct KlH2ServerConfig {
    KlH2ServerSessionFactory factory;
    int max_concurrent_streams;  /**< 0 = KL_H2_DEFAULT_MAX_STREAMS */
    int initial_window_size;     /**< 0 = KL_H2_DEFAULT_WINDOW_SIZE */
} KlH2ServerConfig;

/* ── Per-stream state ────────────────────────────────────────────── */

struct KlH2ServerStream {
    uint32_t stream_id;
    KlRequest req;
    KlResponse res;
    KlBodyReader *body_reader;
    KlRoute *route;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int route_result;
    int headers_done;
    int body_done;
    int response_submitted;
    char *hdr_storage;
    size_t hdr_storage_len;
};

/* ── Per-connection HTTP/2 state ─────────────────────────────────── */

struct KlH2ServerConn {
    KlH2ServerSession *session;
    KlH2ServerCallbacks callbacks;
    KlConn *conn;
    KlRouter *router;
    KlAllocator *alloc;
    KlH2ServerStream *streams;
    int num_streams;
    int max_streams;
    int goaway_sent;
};

/* ── Internal functions (used by connection.c, server.c) ─────────── */

int  kl_h2_server_upgrade(KlConn *c, KlRouter *router, KlH2ServerConfig *cfg,
                           const char *leftover, size_t leftover_len);
int  kl_h2_server_upgrade_from_h1(KlConn *c, KlRouter *router,
                                   KlH2ServerConfig *cfg,
                                   const char *leftover, size_t leftover_len);
int  kl_h2_server_on_readable(KlConn *c);
int  kl_h2_server_on_writable(KlConn *c);
void kl_h2_server_drain_shutdown(KlConn *c);
void kl_h2_server_cleanup(KlConn *c);

#endif
