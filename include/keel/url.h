#ifndef KEEL_URL_H
#define KEEL_URL_H

#include <stddef.h>

/**
 * @brief Parsed URL components (points into original URL string).
 *
 * All string fields are non-owning pointers into the original URL string.
 * The URL string must remain valid for the lifetime of this struct.
 */
typedef struct {
    int         is_https;   /**< 1 for https://, 0 for http:// */
    const char *host;       /**< Hostname (without brackets for IPv6) */
    size_t      host_len;
    int         port;       /**< Port number (default: 80 for http, 443 for https) */
    const char *path;       /**< Path including leading '/' and query string */
    size_t      path_len;
} KlUrl;

/**
 * @brief Parse an HTTP/HTTPS URL into components.
 *
 * Supports http:// and https:// schemes, IPv6 addresses in brackets
 * ([::1]:port), explicit ports, and path+query. Rejects CRLF
 * injection in hostname and path.
 *
 * @param url  URL string to parse.
 * @param out  Parsed components (pointers into url).
 * @return 0 on success, -1 on error.
 */
int kl_url_parse(const char *url, KlUrl *out);

#endif
