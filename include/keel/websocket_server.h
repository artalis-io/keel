/**
 * @file websocket_server.h
 * @brief Server-side WebSocket API.
 *
 * Server-side WebSocket types and functions. Shared protocol constants
 * (frame parser, opcodes, status codes) live in websocket.h.
 */

#ifndef KEEL_WEBSOCKET_SERVER_H
#define KEEL_WEBSOCKET_SERVER_H

#include <keel/websocket.h>
#include <keel/drain.h>
#include <keel/http_server.h>   /* KlPeerCred + kl_peer_cred_fd */
#ifdef __cplusplus
extern "C" {
#endif


/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlWsServerConn KlWsServerConn;
typedef struct KlHttpConn KlHttpConn;

/* ── Callbacks ───────────────────────────────────────────────────── */

typedef struct {
    void (*on_open)(KlWsServerConn *ws, void *user_data);       /**< Connection opened */
    void (*on_message)(KlWsServerConn *ws, const char *data, size_t len,
                       int is_binary, void *user_data);          /**< Message received */
    void (*on_close)(KlWsServerConn *ws, uint16_t code, const char *reason,
                     size_t reason_len, void *user_data);        /**< Close frame received */
    void (*on_ping)(KlWsServerConn *ws, const char *data, size_t len,
                    void *user_data);                            /**< Ping callback (NULL = auto-pong) */
} KlWsServerCallbacks;

/* ── Per-route config ────────────────────────────────────────────── */

struct KlWsServerConfig {
    KlWsServerCallbacks callbacks;  /**< Event callbacks */
    void *user_data;                /**< Opaque pointer passed to callbacks */
    size_t max_message_size;        /**< 0 = 1MB default */
    size_t max_frame_size;          /**< 0 = 64KB default */
    int close_timeout_ms;           /**< 0 = 5000ms default */
    int ping_interval_ms;           /**< 0 = disabled (default) */
};
typedef struct KlWsServerConfig KlWsServerConfig;

/* KlWsServerConn is an opaque, connection-owned WebSocket handle: the callbacks and the public API
 * below take a KlWsServerConn *; application code never constructs, sizes, or dereferences one. Its
 * layout and the server-internal driver functions are private, in
 * src/protocols/websocket/websocket_server_internal.h. */

/* ── Public API ──────────────────────────────────────────────────── */

/** @brief Initialize a KlWsServerConfig with safe defaults. */
void kl_ws_server_config_init(KlWsServerConfig *config);

/** @brief Send a text frame. @return 0 on success, -1 on error. */
int kl_ws_server_send_text(KlWsServerConn *ws, const char *data, size_t len);

/** @brief Send a binary frame. @return 0 on success, -1 on error. */
int kl_ws_server_send_binary(KlWsServerConn *ws, const char *data, size_t len);

/** @brief Send a ping frame. @return 0 on success, -1 on error. */
int kl_ws_server_send_ping(KlWsServerConn *ws, const char *data, size_t len);

/** @brief Initiate close handshake. @return 0 on success, -1 on error. */
int kl_ws_server_close(KlWsServerConn *ws, uint16_t code, const char *reason,
                        size_t reason_len);

/** @brief Enable drain-based backpressure for WebSocket writes. */
int kl_ws_server_enable_drain(KlWsServerConn *ws, size_t max_size);

/**
 * @brief Read the peer credentials of a live WebSocket connection.
 *
 * The post-upgrade equivalent of kl_http_request_peer_cred: lets a handler
 * identify the process on the other end of a UNIX-socket WebSocket after the
 * HTTP upgrade, when there is no longer a KlHttpRequest.
 *
 * @param ws   The WebSocket server connection.
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable (not a UNIX socket / unsupported).
 */
int kl_ws_server_peer_cred(const KlWsServerConn *ws, KlPeerCred *out);

/* The server-internal WebSocket driver seam (kl_ws_server_upgrade/on_readable/on_writable/
 * drain_pending/cleanup/drain_close/check_close_timeout/auto_ping/on_readable_data), which takes a
 * KlHttpConn *, is INTERNAL and lives in src/protocols/websocket/websocket_server_internal.h. */

#ifdef __cplusplus
}
#endif

#endif
