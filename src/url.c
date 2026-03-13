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
        url += 7;
    } else if (strncmp(url, "wss://", 6) == 0) {
        out->is_https = 1;
        out->is_ws = 1;
        url += 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        out->is_ws = 1;
        url += 5;
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
    if (strncmp(location, "http://", 7) == 0 ||
        strncmp(location, "https://", 8) == 0) {
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
        const char *colon = strchr(base_url, ':');
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
        const char *auth = strstr(base_url, "://");
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
