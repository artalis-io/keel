/*
 * http_client_proxy.c: shared HTTP CONNECT (proxy tunnel) request/response logic.
 * See http_client_proxy.h. Shared by http_client_sync.c and http_client_async.c so both transports
 * share ONE serialization + status check (no snprintf-vs-append drift). Locale-free and
 * allocation-free; part of the freestanding client archive.
 */

#include "http_client_proxy.h"
#include "kl_cstr.h"     /* kl_buf_append* (bounded, locale-free) + kl_strstr */
#include <string.h>      /* memcmp */

int kl_proxy_build_connect(char *buf, size_t cap, size_t *out_len,
                           const char *host, uint16_t port, const char *auth) {
    size_t n = 0;
    /* An IPv6 literal goes in brackets, or its colons run into the port (RFC 9110 7.2). */
    const char *lb = "", *rb = "";
    for (const char *h = host; *h; h++)
        if (*h == ':') { lb = "["; rb = "]"; break; }
    if (kl_buf_append(buf, cap, &n, "CONNECT ") != 0 ||
        kl_buf_append(buf, cap, &n, lb) != 0 ||
        kl_buf_append(buf, cap, &n, host) != 0 ||
        kl_buf_append(buf, cap, &n, rb) != 0 ||
        kl_buf_append_n(buf, cap, &n, ":", 1) != 0 ||
        kl_buf_append_u64(buf, cap, &n, port) != 0 ||
        kl_buf_append(buf, cap, &n, " HTTP/1.1\r\nHost: ") != 0 ||
        kl_buf_append(buf, cap, &n, lb) != 0 ||
        kl_buf_append(buf, cap, &n, host) != 0 ||
        kl_buf_append(buf, cap, &n, rb) != 0 ||
        kl_buf_append_n(buf, cap, &n, ":", 1) != 0 ||
        kl_buf_append_u64(buf, cap, &n, port) != 0 ||
        kl_buf_append(buf, cap, &n, "\r\n") != 0)
        return -1;
    if (auth) {
        for (const char *a = auth; *a; a++)
            if (*a == '\r' || *a == '\n') return -1;   /* would inject header lines */
        if (kl_buf_append(buf, cap, &n, "Proxy-Authorization: ") != 0 ||
            kl_buf_append(buf, cap, &n, auth) != 0 ||
            kl_buf_append(buf, cap, &n, "\r\n") != 0)
            return -1;
    }
    if (kl_buf_append(buf, cap, &n, "\r\n") != 0)
        return -1;
    *out_len = n;
    return 0;
}

int kl_proxy_connect_status(const char *buf, size_t len) {
    /* End of headers not seen yet -> need more bytes. kl_strstr scans a NUL-terminated
     * region; the callers keep buf NUL-terminated at [len], and no interior NUL appears in
     * a well-formed HTTP header block, so this is safe + len-bounded in practice. */
    if (!kl_strstr(buf, "\r\n\r\n"))
        return 0;
    /* Headers complete; the reply must open with a whole status line (RFC 9112 4):
     * "HTTP/1." DIGIT SP 3DIGIT, then SP (before a reason phrase) or the CR that ends the line.
     * Any 2xx establishes the tunnel (RFC 9110 9.3.6). len>=13 guarantees the fixed-offset reads
     * below are in bounds. */
    if (len < 13 ||
        memcmp(buf, "HTTP/1.", 7) != 0 ||
        buf[7] < '0' || buf[7] > '9' || buf[8] != ' ' ||
        buf[9] != '2' ||
        buf[10] < '0' || buf[10] > '9' || buf[11] < '0' || buf[11] > '9' ||
        (buf[12] != ' ' && buf[12] != '\r'))
        return -1;
    return 1;
}
