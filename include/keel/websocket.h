#ifndef KEEL_WEBSOCKET_H
#define KEEL_WEBSOCKET_H

#include <keel/allocator.h>
#include <stddef.h>
#include <stdint.h>

/* ── Status codes (RFC 6455 Section 7.4.1) ──────────────────────── */

#define KL_WS_NORMAL_CLOSE    1000
#define KL_WS_GOING_AWAY      1001
#define KL_WS_PROTOCOL_ERROR  1002
#define KL_WS_TOO_BIG         1009

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlWsConn KlWsConn;
typedef struct KlConn KlConn;

/* ── Callbacks ───────────────────────────────────────────────────── */

typedef struct {
    void (*on_open)(KlWsConn *ws, void *user_data);
    void (*on_message)(KlWsConn *ws, const char *data, size_t len,
                       int is_binary, void *user_data);
    void (*on_close)(KlWsConn *ws, uint16_t code, const char *reason,
                     size_t reason_len, void *user_data);
    void (*on_ping)(KlWsConn *ws, const char *data, size_t len,
                    void *user_data);  /* NULL = auto-pong */
} KlWsCallbacks;

/* ── Per-route config ────────────────────────────────────────────── */

struct KlWsConfig {
    KlWsCallbacks callbacks;
    void *user_data;
    size_t max_message_size;    /* 0 = 1MB default */
    size_t max_frame_size;      /* 0 = 64KB default */
    int close_timeout_ms;       /* 0 = 5000ms default */
};
typedef struct KlWsConfig KlWsConfig;

/* ── Internal frame parser state (opaque to users) ───────────────── */

typedef enum {
    KL_WS_FRAME_HEADER,
    KL_WS_FRAME_PAYLOAD,
    KL_WS_FRAME_COMPLETE
} KlWsFrameParseState;

typedef struct {
    KlWsFrameParseState state;
    uint8_t header_buf[14];  /* max: 2 + 8 + 4 */
    size_t header_len;
    size_t header_need;      /* total header bytes needed */
    int fin, opcode, masked;
    uint8_t mask_key[4];
    size_t payload_len;
    size_t payload_read;
} KlWsFrameParser;

/* ── Per-connection WebSocket state ──────────────────────────────── */

struct KlWsConn {
    KlWsConfig *config;          /* points to route's config (not owned) */
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
    KlConn *conn;                /* back-pointer for send functions */
    KlAllocator *alloc;
};

/* ── Public API ──────────────────────────────────────────────────── */

/** Initialize a KlWsConfig with safe defaults. */
void kl_ws_config_init(KlWsConfig *config);

/** Send a text frame. Returns 0 on success, -1 on error. */
int kl_ws_send_text(KlWsConn *ws, const char *data, size_t len);

/** Send a binary frame. Returns 0 on success, -1 on error. */
int kl_ws_send_binary(KlWsConn *ws, const char *data, size_t len);

/** Send a ping frame. Returns 0 on success, -1 on error. */
int kl_ws_send_ping(KlWsConn *ws, const char *data, size_t len);

/** Initiate close handshake. Returns 0 on success, -1 on error. */
int kl_ws_close(KlWsConn *ws, uint16_t code, const char *reason,
                size_t reason_len);

/* ── Internal (used by connection.c / server.c) ──────────────────── */

/**
 * Attempt WebSocket upgrade after headers are parsed + route matched.
 * @return New connection state (KL_CONN_WEBSOCKET or KL_CONN_CLOSED).
 */
int kl_ws_upgrade(KlConn *c, const char *leftover, size_t leftover_len);

/**
 * Process readable data on a WebSocket connection.
 * @return New connection state.
 */
int kl_ws_on_readable(KlConn *c);

/**
 * Clean up WebSocket state on connection release.
 * Calls on_close with 1006 if close was not received.
 */
void kl_ws_cleanup(KlConn *c);

/**
 * Send close frame with code 1001 (Going Away) for graceful drain.
 */
void kl_ws_drain_close(KlConn *c);

/**
 * Check close deadline. Returns 1 if deadline expired, 0 otherwise.
 */
int kl_ws_check_close_timeout(const KlConn *c, uint64_t now);

/* ── Frame parser (internal, tested directly) ────────────────────── */

void kl_ws_frame_init(KlWsFrameParser *fp);
int  kl_ws_frame_parse(KlWsFrameParser *fp, const uint8_t *data,
                        size_t len, size_t *consumed);

/**
 * Process raw WebSocket data bytes (after TLS decryption).
 * Used by kl_ws_upgrade() for leftover bytes and by kl_ws_on_readable().
 * @return New connection state.
 */
int kl_ws_on_readable_data(KlConn *c, const uint8_t *data, size_t len);

#endif
