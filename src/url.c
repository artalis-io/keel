/*
 * url.c — HTTP/HTTPS URL parser
 *
 * Parses URLs into scheme, host, port, and path components.
 * All output pointers reference the original URL string (zero-copy).
 * Includes CRLF injection guard for hostname and path.
 */

#include <keel/url.h>

#include <stdlib.h>
#include <string.h>

/* ── CRLF injection guard ────────────────────────────────────────── */

static int has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── URL parsing ─────────────────────────────────────────────────── */

int kl_url_parse(const char *url, KlUrl *out)
{
    if (!url || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Scheme */
    if (strncmp(url, "https://", 8) == 0) {
        out->is_https = 1;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->is_https = 0;
        url += 7;
    } else {
        return -1;  /* unsupported scheme */
    }

    /* Host (may include :port) */
    out->host = url;

    /* Handle IPv6 addresses in brackets: [::1]:8080 */
    if (*url == '[') {
        const char *bracket = strchr(url, ']');
        if (!bracket)
            return -1;
        out->host = url + 1;
        out->host_len = (size_t)(bracket - url - 1);
        url = bracket + 1;
    } else {
        /* Find end of host (: or / or end) */
        const char *host_start = url;
        while (*url && *url != ':' && *url != '/')
            url++;
        out->host_len = (size_t)(url - host_start);
    }

    if (out->host_len == 0)
        return -1;

    /* Reject CRLF in hostname (header injection) */
    if (has_crlf(out->host, out->host_len))
        return -1;

    /* Port (optional) */
    if (*url == ':') {
        url++;
        char *end;
        long p = strtol(url, &end, 10);
        if (end == url || p < 1 || p > 65535)
            return -1;
        out->port = (int)p;
        url = end;
    } else {
        out->port = out->is_https ? 443 : 80;
    }

    /* Path (rest of URL, or "/" if empty) — reject CRLF (header injection) */
    if (*url == '/') {
        out->path = url;
        out->path_len = strlen(url);
        if (has_crlf(out->path, out->path_len))
            return -1;
    } else if (*url == '\0') {
        out->path = "/";
        out->path_len = 1;
    } else {
        return -1;  /* unexpected character after port */
    }

    return 0;
}
