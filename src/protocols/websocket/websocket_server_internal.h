/*
 * websocket_server_internal.h: INTERNAL. The concrete KlWsServerConn layout + the server-internal
 * WebSocket driver seam.
 *
 * KlWsServerConn is opaque on the public surface (forward-declared in <keel/websocket_server.h>; the
 * callbacks and public API take a KlWsServerConn *). Its layout and the driver functions that take a
 * KlHttpConn * (called by the HTTP connection driver on upgrade and by the completion WS adapter) live
 * here. Include this ONLY from the WebSocket implementation TUs (src/protocols/websocket), the HTTP
 * connection driver that dispatches the upgrade (src/protocols/http/http_connection.c), and
 * explicitly-justified white-box tests. Substrate and integrations must NOT include it.
 */
#ifndef KEEL_SRC_WEBSOCKET_SERVER_INTERNAL_H
#define KEEL_SRC_WEBSOCKET_SERVER_INTERNAL_H

#include <keel/websocket_server.h>   /* KlWsServerConn forward decl, KlWsServerConfig, KlHttpConn forward */
#include <keel/websocket.h>          /* KlWsFrameParser */
#include <keel/drain.h>              /* KlDrain */
#include <keel/allocator.h>          /* KlAllocator */
#include <stddef.h>
#include <stdint.h>

struct KlWsServerConn {
    KlWsServerConfig *config;    /* Points to route's config (not owned) */
    KlWsFrameParser frame;       /* Incremental frame parser */
    char *msg_buf;               /* Reassembly buffer (allocated) */
    size_t msg_len;              /* Current message length */
    size_t msg_cap;              /* Message buffer capacity */
    int msg_opcode;              /* Opcode of first fragment */
    uint32_t utf8_state;         /* Incremental UTF-8 validator state */
    int close_sent;              /* Close frame sent flag */
    int close_received;          /* Close frame received flag */
    uint16_t close_code;         /* Close status code */
    uint64_t close_deadline_ms;  /* Close handshake timeout deadline */
    uint64_t next_ping_ms;       /* 0 = auto-ping disabled */
    KlHttpConn *conn;            /* Back-pointer for send functions */
    KlAllocator *alloc;          /* Allocator for message buffer */
    KlDrain drain;               /* Backpressure write buffer (opt-in) */
    int drain_enabled;           /* 0 = off (default), 1 = on */
};

/* Server-internal WebSocket driver seam (used by http_connection.c / http_server.c / completion_ws.c). */
int  kl_ws_server_upgrade(KlHttpConn *c, const char *leftover, size_t leftover_len);
int  kl_ws_server_on_readable(KlHttpConn *c);
int  kl_ws_server_on_writable(KlHttpConn *c);
int  kl_ws_server_drain_pending(const KlHttpConn *c);
void kl_ws_server_cleanup(KlHttpConn *c);
void kl_ws_server_drain_close(KlHttpConn *c);
int  kl_ws_server_check_close_timeout(const KlHttpConn *c, uint64_t now);
int  kl_ws_server_auto_ping(KlHttpConn *c, uint64_t now);
int  kl_ws_server_on_readable_data(KlHttpConn *c, uint8_t *data, size_t len);

#endif /* KEEL_SRC_WEBSOCKET_SERVER_INTERNAL_H */
