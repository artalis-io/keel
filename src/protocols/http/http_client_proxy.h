/*
 * http_client_proxy.h: transport-independent HTTP CONNECT (proxy tunnel) helpers.
 *
 * The sync (http_client_sync.c) and async (http_client_async.c) clients both establish a proxy
 * tunnel with an HTTP CONNECT handshake. They previously each open-coded the request
 * serialization (one via snprintf, one via kl_buf_append_*) and the response validation,
 * risking behavioral drift around authority formatting, auth handling and the 200 check.
 * These two pure-byte functions are the single shared definition; the two clients differ
 * ONLY in how they move bytes (blocking send/recv vs the async state machine).
 *
 * Both are locale-free and allocation-free, so http_client_proxy.c joins the freestanding
 * client archive alongside http_client_async.c.
 */
#ifndef KEEL_HTTP_CLIENT_PROXY_H
#define KEEL_HTTP_CLIENT_PROXY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Serialize a proxy CONNECT request into @buf:
 *   "CONNECT <host>:<port> HTTP/1.1\r\nHost: <host>:<port>\r\n"
 *   [ "Proxy-Authorization: <auth>\r\n" ]  "\r\n"
 * Returns 0 on success and sets *out_len to the byte count; -1 if it does not fit @cap.
 * Locale-free (bounded append helpers): the ONE serialization shared by sync + async.
 */
int kl_proxy_build_connect(char *buf, size_t cap, size_t *out_len,
                           const char *host, uint16_t port, const char *auth);

/*
 * Classify an accumulated proxy CONNECT response (the caller owns byte movement + the
 * overall size cap; @buf need not be NUL-terminated; @len is authoritative):
 *   1  = tunnel established  (end-of-headers seen AND status is HTTP/1.x 200)
 *   0  = need more bytes     (end-of-headers "\r\n\r\n" not seen yet)
 *  -1  = protocol error      (headers complete but status is not 2xx-200, or malformed)
 */
int kl_proxy_connect_status(const char *buf, size_t len);

#endif /* KEEL_HTTP_CLIENT_PROXY_H */
