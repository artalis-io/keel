#ifndef KEEL_CORS_H
#define KEEL_CORS_H

#include <keel/request.h>
#include <keel/http_response.h>
#include <stddef.h>

/** @file cors.h
 *  @brief Built-in CORS middleware.
 *
 *  Usage:
 *  @code
 *    KlCorsConfig cors;
 *    kl_cors_init(&cors);
 *    kl_cors_add_origin(&cors, "https://app.example.com");
 *    kl_server_use(&s, "*", pattern, kl_cors_middleware, &cors);
 *  @endcode
 */

/** @brief Maximum number of allowed origins. */
#define KL_CORS_MAX_ORIGINS 16
/** @brief Maximum origin string length. */
#define KL_CORS_ORIGIN_SIZE 256

typedef struct {
    char allowed_origins[KL_CORS_MAX_ORIGINS][KL_CORS_ORIGIN_SIZE];
    int origin_count;             /**< 0 = allow all (*) */
    const char *allowed_methods;  /**< default: "GET, POST, OPTIONS" */
    const char *allowed_headers;  /**< default: "Content-Type, Authorization" */
    int allow_credentials;        /**< 1 = include credentials header */
    int max_age_seconds;          /**< preflight cache time (default: 86400) */
} KlCorsConfig;

/**
 * @brief Initialize CORS config with defaults.
 *        Allows all origins (*), standard methods/headers, 24h preflight cache.
 */
void kl_cors_init(KlCorsConfig *config);

/**
 * @brief Add an allowed origin.
 *        If no origins are added, all origins are allowed (*).
 *        Once at least one origin is added, only those origins are allowed.
 * @return 1 on success, 0 if full or invalid.
 */
int  kl_cors_add_origin(KlCorsConfig *config, const char *origin);

/**
 * @brief Parse comma-separated origins and add them.
 *        Useful for parsing from environment variables.
 * @return Number of origins added.
 */
int  kl_cors_parse_origins(KlCorsConfig *config, const char *origins);

/**
 * @brief Check if an origin is allowed by the config.
 * @return 1 if allowed, 0 if not.
 */
int  kl_cors_is_allowed(const KlCorsConfig *config, const char *origin,
                         size_t origin_len);

/**
 * @brief CORS middleware function.
 *        Pass a KlCorsConfig* as user_data when registering.
 *
 *        For OPTIONS requests: sends 204 with preflight headers, short-circuits.
 *        For other requests: adds Access-Control-Allow-Origin and continues.
 *
 * @return 0 to continue, 1 to short-circuit (OPTIONS preflight).
 */
int  kl_cors_middleware(KlRequest *req, KlHttpResponse *res, void *user_data);

#endif
