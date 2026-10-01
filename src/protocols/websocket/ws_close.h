/*
 * ws_close.h: INTERNAL. Validation of a received WebSocket CLOSE payload (RFC 6455 5.5.1, 7.4),
 * shared by the server (http_server_ws.c) and the client (websocket_client.c) so the two cannot
 * drift.
 */
#ifndef KEEL_SRC_WS_CLOSE_H
#define KEEL_SRC_WS_CLOSE_H

#include <keel/websocket.h>   /* KL_WS_PROTOCOL_ERROR */
#include "utf8.h"             /* kl_utf8_validate, KL_UTF8_ACCEPT (src/, on the include path) */
#include <stddef.h>
#include <stdint.h>

/* 0 if the payload is acceptable: empty, or a status code a peer may send followed by a UTF-8
 * reason. Otherwise the status to fail the connection with: 1002 for a 1-byte payload or a code
 * outside 1000-1003, 1007-1014 and 3000-4999 (1004-1006 and 1015 are never sent), 1007 for a
 * reason that is not UTF-8. */
static inline int kl_ws_close_payload_check(const uint8_t *p, size_t len) {
    if (len == 0) return 0;
    if (len == 1) return KL_WS_PROTOCOL_ERROR;
    unsigned code = ((unsigned)p[0] << 8) | p[1];
    int ok = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) ||
             (code >= 3000 && code <= 4999);
    if (!ok) return KL_WS_PROTOCOL_ERROR;
    uint32_t st = KL_UTF8_ACCEPT;
    kl_utf8_validate(&st, p + 2, len - 2);
    return st == KL_UTF8_ACCEPT ? 0 : 1007;   /* 1007 Invalid frame payload data */
}

#endif /* KEEL_SRC_WS_CLOSE_H */
