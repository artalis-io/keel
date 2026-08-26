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
    if (kl_buf_append(buf, cap, &n, "CONNECT ") != 0 ||
        kl_buf_append(buf, cap, &n, host) != 0 ||
        kl_buf_append_n(buf, cap, &n, ":", 1) != 0 ||
        kl_buf_append_u64(buf, cap, &n, port) != 0 ||
        kl_buf_append(buf, cap, &n, " HTTP/1.1\r\nHost: ") != 0 ||
        kl_buf_append(buf, cap, &n, host) != 0 ||
        kl_buf_append_n(buf, cap, &n, ":", 1) != 0 ||
        kl_buf_append_u64(buf, cap, &n, port) != 0 ||
        kl_buf_append(buf, cap, &n, "\r\n") != 0)
        return -1;
    if (auth) {
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
    /* Headers complete; verify the status line is HTTP/1.x 200. len>=12 guarantees the
     * fixed-offset reads below are in bounds. */
    if (len < 12 ||
        memcmp(buf, "HTTP/1.", 7) != 0 ||
        buf[9] != '2' || buf[10] != '0' || buf[11] != '0')
        return -1;
    return 1;
}
