/*
 * url.c — HTTP/HTTPS URL parser
 *
 * Parses URLs into scheme, host, port, and path components.
 * All output pointers reference the original URL string (zero-copy).
 * Includes CRLF injection guard for hostname and path.
 */

#include <keel/url.h>

#include <string.h>

#include "kl_cstr.h"   /* locale-free port parse + bounded find (no strtol) */

/* ── CRLF injection guard ────────────────────────────────────────── */

static int has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── Percent-decoding (for http+unix:// socket paths) ─────────────── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Percent-decode src[0..len) into dst (NUL-terminated, capacity dstsz).
 * Rejects invalid %XX escapes, an embedded NUL, and overflow.
 * Returns the decoded length on success, -1 on error.
 */
static int pct_decode(const char *src, size_t len, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= len)
                return -1;  /* truncated escape */
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;  /* invalid hex */
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0')
            return -1;  /* embedded NUL not allowed in a socket path */
        if (di + 1 >= dstsz)
            return -1;  /* no room for byte + NUL */
        dst[di++] = c;
    }
    dst[di] = '\0';
    return (int)di;
}

/* ── URL parsing ─────────────────────────────────────────────────── */

int kl_url_parse(const char *url, KlUrl *out)
{
    if (!url || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Scheme (check the longer "+unix" variants before the plain ones) */
    if (kl_str_startswith(url, "https+unix://")) {
        out->is_https = 1;
        out->is_unix = 1;
        url += 13;
    } else if (kl_str_startswith(url, "http+unix://")) {
        out->is_unix = 1;
        url += 12;
    } else if (kl_str_startswith(url, "https://")) {
        out->is_https = 1;
        url += 8;
    } else if (kl_str_startswith(url, "http://")) {
        url += 7;
    } else if (kl_str_startswith(url, "wss+unix://")) {
        out->is_https = 1;
        out->is_ws = 1;
        out->is_unix = 1;
        url += 11;
    } else if (kl_str_startswith(url, "ws+unix://")) {
        out->is_ws = 1;
        out->is_unix = 1;
        url += 10;
    } else if (kl_str_startswith(url, "wss://")) {
        out->is_https = 1;
        out->is_ws = 1;
        url += 6;
    } else if (kl_str_startswith(url, "ws://")) {
        out->is_ws = 1;
        url += 5;
    } else {
        return -1;  /* unsupported scheme */
    }

    /* UNIX socket: authority is a percent-encoded socket path up to the
     * first literal '/'.  There is no host/port; callers use "localhost"
     * for the Host header. */
    if (out->is_unix) {
        const char *auth = url;
        while (*url && *url != '/')
            url++;
        size_t enc_len = (size_t)(url - auth);
        if (enc_len == 0)
            return -1;  /* empty socket path */
        int dec = pct_decode(auth, enc_len, out->unix_path,
                             sizeof(out->unix_path));
        if (dec < 0)
            return -1;
        if (has_crlf(out->unix_path, (size_t)dec))
            return -1;

        out->host = NULL;
        out->host_len = 0;
        out->port = 0;

        if (*url == '/') {
            out->path = url;
            out->path_len = strlen(url);
            if (has_crlf(out->path, out->path_len))
                return -1;
        } else {
            out->path = "/";
            out->path_len = 1;
        }
        return 0;
    }

    /* Host (may include :port) */
    out->host = url;

    /* Handle IPv6 addresses in brackets: [::1]:8080 */
    if (*url == '[') {
        const char *bracket = kl_strchr(url, ']');
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

    /* Port (optional) — bounded decimal, locale-free (no strtol) */
    if (*url == ':') {
        url++;
        const char *pstart = url;
        while (*url >= '0' && *url <= '9')
            url++;
        uint16_t p;
        /* kl_parse_u16_decimal rejects empty span, non-digits, and > 65535;
         * an explicit port of 0 is also invalid (matches the old p < 1). */
        if (kl_parse_u16_decimal(pstart, (size_t)(url - pstart), &p) != 0 ||
            p == 0)
            return -1;
        out->port = (int)p;
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

/* ── URL resolution ─────────────────────────────────────────────── */

/**
 * Strip fragment (#...) from a string, returning the length without it.
 */
static size_t strip_fragment(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '#')
            return i;
    }
    return len;
}

int kl_url_resolve(const char *base_url, const char *location,
                   char *out, size_t out_size)
{
    if (!base_url || !location || !out || out_size == 0)
        return -1;

    size_t loc_len = strlen(location);
    if (loc_len == 0)
        return -1;

    /* 1. Absolute URL — starts with http:// or https:// */
    if (kl_str_startswith(location, "http://") ||
        kl_str_startswith(location, "https://")) {
        size_t n = strip_fragment(location, loc_len);
        if (n >= out_size)
            return -1;
        memcpy(out, location, n);
        out[n] = '\0';
        return 0;
    }

    /* 2. Protocol-relative — starts with // */
    if (location[0] == '/' && location[1] == '/') {
        /* Extract scheme from base_url */
        const char *colon = kl_strchr(base_url, ':');
        if (!colon)
            return -1;
        size_t scheme_len = (size_t)(colon - base_url);

        size_t loc_stripped = strip_fragment(location, loc_len);
        if (scheme_len + 1 + loc_stripped >= out_size)
            return -1;
        memcpy(out, base_url, scheme_len + 1); /* "http:" or "https:" */
        memcpy(out + scheme_len + 1, location, loc_stripped);
        out[scheme_len + 1 + loc_stripped] = '\0';
        return 0;
    }

    /* 3. Absolute path — starts with / */
    if (location[0] == '/') {
        /* Extract scheme://host[:port] from base_url */
        const char *auth = kl_strstr(base_url, "://");
        if (!auth)
            return -1;
        auth += 3; /* past "://" */

        /* Find end of authority (host[:port]) — first / or end */
        const char *auth_end = auth;
        while (*auth_end && *auth_end != '/')
            auth_end++;
        size_t origin_len = (size_t)(auth_end - base_url);

        size_t loc_stripped = strip_fragment(location, loc_len);
        if (origin_len + loc_stripped >= out_size)
            return -1;
        memcpy(out, base_url, origin_len);
        memcpy(out + origin_len, location, loc_stripped);
        out[origin_len + loc_stripped] = '\0';
        return 0;
    }

    /* Bare relative refs not supported */
    return -1;
}
