/*
 * websocket_server.h — Server-side WebSocket API
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
    void (*on_open)(KlWsServerConn *ws, void *user_data);
    void (*on_message)(KlWsServerConn *ws, const char *data, size_t len,
                       int is_binary, void *user_data);
    void (*on_close)(KlWsServerConn *ws, uint16_t code, const char *reason,
                     size_t reason_len, void *user_data);
    void (*on_ping)(KlWsServerConn *ws, const char *data, size_t len,
                    void *user_data);  /* NULL = auto-pong */
} KlWsServerCallbacks;

/* ── Per-route config ────────────────────────────────────────────── */

struct KlWsServerConfig {
    KlWsServerCallbacks callbacks;
    void *user_data;
    size_t max_message_size;    /* 0 = 1MB default */
    size_t max_frame_size;      /* 0 = 64KB default */
    int close_timeout_ms;       /* 0 = 5000ms default */
    int ping_interval_ms;       /* 0 = disabled (default) */
};
typedef struct KlWsServerConfig KlWsServerConfig;

/* ── Per-connection WebSocket state ──────────────────────────────── */

struct KlWsServerConn {
    KlWsServerConfig *config;    /* points to route's config (not owned) */
    KlWsFrameParser frame;       /* incremental frame parser */
    char *msg_buf;               /* reassembly buffer (allocated) */
    size_t msg_len;
    size_t msg_cap;
    int msg_opcode;              /* opcode of first fragment */
    uint32_t utf8_state;         /* incremental UTF-8 validator state */
    int close_sent;
    int close_received;
    uint16_t close_code;
    uint64_t close_deadline_ms;
    uint64_t next_ping_ms;       /* 0 = auto-ping disabled */
    KlConn *conn;                /* back-pointer for send functions */
    KlAllocator *alloc;
    KlDrain drain;               /* backpressure write buffer (opt-in) */
    int drain_enabled;           /* 0 = off (default), 1 = on */
};

/* ── Public API ──────────────────────────────────────────────────── */

/** Initialize a KlWsServerConfig with safe defaults. */
void kl_ws_server_config_init(KlWsServerConfig *config);

/** Send a text frame. Returns 0 on success, -1 on error. */
int kl_ws_server_send_text(KlWsServerConn *ws, const char *data, size_t len);

/** Send a binary frame. Returns 0 on success, -1 on error. */
int kl_ws_server_send_binary(KlWsServerConn *ws, const char *data, size_t len);

/** Send a ping frame. Returns 0 on success, -1 on error. */
int kl_ws_server_send_ping(KlWsServerConn *ws, const char *data, size_t len);

/** Initiate close handshake. Returns 0 on success, -1 on error. */
int kl_ws_server_close(KlWsServerConn *ws, uint16_t code, const char *reason,
                        size_t reason_len);

/** Enable drain-based backpressure for WebSocket writes. */
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
