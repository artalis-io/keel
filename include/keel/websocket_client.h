/*
 * websocket_client.h — WebSocket client API
 *
 * Async WebSocket client driven by KlEventCtx watchers.
 * Reuses the shared frame parser from websocket.h.
 * Client frames are masked per RFC 6455 Section 5.3.
 */

#ifndef KEEL_WEBSOCKET_CLIENT_H
#define KEEL_WEBSOCKET_CLIENT_H

#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/tls.h>
#include <keel/websocket.h>

/* ── Defaults ────────────────────────────────────────────────────── */

#define KL_WS_CLIENT_DEFAULT_TIMEOUT_MS   30000
#define KL_WS_CLIENT_RECV_BUF_SIZE        8192
#define KL_WS_CLIENT_DEFAULT_MAX_FRAME    (16 * 1024 * 1024)
#define KL_WS_CLIENT_KEY_B64_SIZE         25   /* 16 bytes -> 24 Base64 chars + NUL */
#define KL_WS_CLIENT_UPGRADE_BUF_INIT     512

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct KlWsClientConn KlWsClientConn;

typedef struct {
    int          timeout_ms;       /**< 0 = KL_WS_CLIENT_DEFAULT_TIMEOUT_MS */
    size_t       max_frame_size;   /**< 0 = KL_WS_CLIENT_DEFAULT_MAX_FRAME */
    KlTlsConfig *tls;             /**< NULL = ws://, non-NULL = wss:// */
    const char  *protocol;        /**< Sec-WebSocket-Protocol, NULL = none */
    int          ping_interval_ms;  /**< 0 = disabled (default) */
} KlWsClientConfig;

typedef struct {
    void (*on_open)(KlWsClientConn *ws, void *user_data);
    void (*on_message)(KlWsClientConn *ws, const char *data, size_t len,
                       int is_binary, void *user_data);
    void (*on_close)(KlWsClientConn *ws, uint16_t code,
                     const char *reason, size_t reason_len, void *user_data);
    void (*on_error)(KlWsClientConn *ws, const char *msg, void *user_data);
} KlWsClientCallbacks;

/* ── Public API ──────────────────────────────────────────────────── */

/**
 * @brief Connect to a WebSocket server.
 *
 * Initiates a non-blocking TCP connect, optional TLS handshake, and
 * WebSocket HTTP upgrade. Callbacks fire on the event loop thread.
 *
 * @param ev     Event context (standalone or &server->ev).
 * @param alloc  Allocator (must outlive the connection).
 * @param cfg    Configuration (NULL for defaults, timeout/TLS/limits).
 * @param url    WebSocket URL (ws:// or wss://).
 * @param cbs    Callbacks (on_open, on_message, on_close, on_error).
 * @param user_data Opaque pointer passed to all callbacks.
 * @return Connection handle, or NULL on immediate failure.
 */
KlWsClientConn *kl_ws_client_connect(KlEventCtx *ev, KlAllocator *alloc,
                                      const KlWsClientConfig *cfg,
                                      const char *url,
                                      const KlWsClientCallbacks *cbs,
                                      void *user_data);

int   kl_ws_client_send_text(KlWsClientConn *ws, const char *data, size_t len);
int   kl_ws_client_send_binary(KlWsClientConn *ws, const char *data, size_t len);
int   kl_ws_client_send_ping(KlWsClientConn *ws, const char *data, size_t len);
void  kl_ws_client_close(KlWsClientConn *ws, uint16_t code,
                          const char *reason, size_t reason_len);
void  kl_ws_client_free(KlWsClientConn *ws);

#endif
