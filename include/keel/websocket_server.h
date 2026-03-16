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

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlWsServerConn KlWsServerConn;
typedef struct KlConn KlConn;

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

/* ── Per-connection WebSocket state ──────────────────────────────── */

struct KlWsServerConn {
    KlWsServerConfig *config;    /**< Points to route's config (not owned) */
    KlWsFrameParser frame;       /**< Incremental frame parser */
    char *msg_buf;               /**< Reassembly buffer (allocated) */
    size_t msg_len;              /**< Current message length */
    size_t msg_cap;              /**< Message buffer capacity */
    int msg_opcode;              /**< Opcode of first fragment */
    uint32_t utf8_state;         /**< Incremental UTF-8 validator state */
    int close_sent;              /**< Close frame sent flag */
    int close_received;          /**< Close frame received flag */
    uint16_t close_code;         /**< Close status code */
    uint64_t close_deadline_ms;  /**< Close handshake timeout deadline */
    uint64_t next_ping_ms;       /**< 0 = auto-ping disabled */
    KlConn *conn;                /**< Back-pointer for send functions */
    KlAllocator *alloc;          /**< Allocator for message buffer */
    KlDrain drain;               /**< Backpressure write buffer (opt-in) */
    int drain_enabled;           /**< 0 = off (default), 1 = on */
};

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

/* ── Internal (used by connection.c / server.c) ──────────────────── */

int  kl_ws_server_upgrade(KlConn *c, const char *leftover, size_t leftover_len);
int  kl_ws_server_on_readable(KlConn *c);
int  kl_ws_server_on_writable(KlConn *c);
int  kl_ws_server_drain_pending(const KlConn *c);
void kl_ws_server_cleanup(KlConn *c);
void kl_ws_server_drain_close(KlConn *c);
int  kl_ws_server_check_close_timeout(const KlConn *c, uint64_t now);
int  kl_ws_server_auto_ping(KlConn *c, uint64_t now);
int  kl_ws_server_on_readable_data(KlConn *c, uint8_t *data, size_t len);

#endif
