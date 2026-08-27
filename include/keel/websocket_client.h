/**
 * @file websocket_client.h
 * @brief WebSocket client API.
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
#ifdef __cplusplus
extern "C" {
#endif


/* ── Defaults ────────────────────────────────────────────────────── */

/** @brief Default connect/handshake timeout in milliseconds. */
#define KL_WS_CLIENT_DEFAULT_TIMEOUT_MS   30000
/** @brief Receive buffer size in bytes. */
#define KL_WS_CLIENT_RECV_BUF_SIZE        8192
/** @brief Default maximum frame size (16 MB). */
#define KL_WS_CLIENT_DEFAULT_MAX_FRAME    (16 * 1024 * 1024)
/** @brief Base64-encoded key size (16 bytes -> 24 Base64 chars + NUL). */
#define KL_WS_CLIENT_KEY_B64_SIZE         25
/** @brief Initial upgrade response buffer size. */
#define KL_WS_CLIENT_UPGRADE_BUF_INIT     512

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct KlWsClientConn KlWsClientConn;

/*
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default. New members are appended
 * after ping_interval_ms.
 */
typedef struct {
    int          timeout_ms;       /**< 0 = KL_WS_CLIENT_DEFAULT_TIMEOUT_MS */
    size_t       max_frame_size;   /**< 0 = KL_WS_CLIENT_DEFAULT_MAX_FRAME */
    KlTlsConfig *tls;             /**< NULL = ws://, non-NULL = wss:// */
    const char  *protocol;        /**< Sec-WebSocket-Protocol, NULL = none */
    int          ping_interval_ms;  /**< 0 = disabled (default) */
} KlWsClientConfig;

typedef struct {
    void (*on_open)(KlWsClientConn *ws, void *user_data);             /**< Connection opened */
    void (*on_message)(KlWsClientConn *ws, const char *data, size_t len,
                       int is_binary, void *user_data);                /**< Message received */
    void (*on_close)(KlWsClientConn *ws, uint16_t code,
                     const char *reason, size_t reason_len, void *user_data); /**< Close frame received */
    void (*on_error)(KlWsClientConn *ws, const char *msg, void *user_data);   /**< Error occurred */
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

/** @brief Send a text frame. @return 0 on success, -1 on error. */
int   kl_ws_client_send_text(KlWsClientConn *ws, const char *data, size_t len);

/** @brief Send a binary frame. @return 0 on success, -1 on error. */
int   kl_ws_client_send_binary(KlWsClientConn *ws, const char *data, size_t len);

/** @brief Send a ping frame. @return 0 on success, -1 on error. */
int   kl_ws_client_send_ping(KlWsClientConn *ws, const char *data, size_t len);

/** @brief Initiate close handshake. */
void  kl_ws_client_close(KlWsClientConn *ws, uint16_t code,
                          const char *reason, size_t reason_len);

/** @brief Free all WebSocket client resources. */
void  kl_ws_client_free(KlWsClientConn *ws);

#ifdef __cplusplus
}
#endif

#endif
