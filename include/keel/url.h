#ifndef KEEL_URL_H
#define KEEL_URL_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Parsed URL components (points into original URL string).
 *
 * All string fields are non-owning pointers into the original URL string.
 * The URL string must remain valid for the lifetime of this struct.
 */
/** @brief Max AF_UNIX socket path (fits the largest sun_path across platforms). */
#define KL_URL_UNIX_PATH_MAX 108

typedef struct {
    int         is_https;   /**< 1 for https:// or wss://, 0 for http:// or ws:// */
    int         is_ws;      /**< 1 for ws:// or wss://, 0 for http:// or https:// */
    int         is_unix;    /**< 1 for http+unix:// or https+unix:// */
    const char *host;       /**< Hostname (without brackets for IPv6); NULL for unix */
    size_t      host_len;
    int         port;       /**< Port number (default: 80 for http/ws, 443 for https/wss; 0 for unix) */
    const char *path;       /**< Path including leading '/' and query string */
    size_t      path_len;
    char        unix_path[KL_URL_UNIX_PATH_MAX]; /**< Decoded AF_UNIX socket path (when is_unix) */
} KlUrl;

/**
 * @brief Parse an HTTP/HTTPS URL into components.
 *
 * Supports http://, https://, ws://, and wss:// schemes, IPv6
 * addresses in brackets ([::1]:port), explicit ports, and
 * path+query. Rejects CRLF injection in hostname and path.
 *
 * Also supports http+unix:// and https+unix://, where the authority is a
 * percent-encoded AF_UNIX socket path (e.g. http+unix://%2Frun%2Fapp.sock/v1).
 * The decoded path is written to out->unix_path and out->is_unix is set;
 * out->host is NULL (callers use "localhost" for the Host header).
 *
 * @param url  URL string to parse.
 * @param out  Parsed components (pointers into url).
 * @return 0 on success, -1 on error.
 */
int kl_url_parse(const char *url, KlUrl *out);

/**
 * @brief Maximum URL length for resolved URLs.
 */
#define KL_URL_MAX 2048

/**
 * @brief Resolve a Location header value against a base URL.
 *
 * Handles absolute URLs (http:// or https://), protocol-relative (//),
 * and absolute-path (/...) references. Strips fragment (#) if present.
 * Bare relative refs (../foo) are not supported and return -1.
 *
 * @param base_url   The URL of the current request.
 * @param location   The Location header value.
 * @param out        Output buffer for the resolved URL.
 * @param out_size   Size of the output buffer.
 * @return 0 on success, -1 on error.
 */
int kl_url_resolve(const char *base_url, const char *location,
                   char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
