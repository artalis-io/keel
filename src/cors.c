#include <keel/cors.h>
#include <string.h>
#include <stdio.h>

/* ── Defaults ───────────────────────────────────────────────────────── */

static const char *kl_cors_default_methods = "GET, POST, OPTIONS";
static const char *kl_cors_default_headers = "Content-Type, Authorization";

/* ── Public API ─────────────────────────────────────────────────────── */

void kl_cors_init(KlCorsConfig *config) {
    memset(config, 0, sizeof(*config));
    config->allowed_methods = kl_cors_default_methods;
    config->allowed_headers = kl_cors_default_headers;
    config->max_age_seconds = 86400;
}

int kl_cors_add_origin(KlCorsConfig *config, const char *origin) {
    if (!origin || origin[0] == '\0') return 0;
    if (config->origin_count >= KL_CORS_MAX_ORIGINS) return 0;

    size_t len = strlen(origin);
    if (len >= KL_CORS_ORIGIN_SIZE) len = KL_CORS_ORIGIN_SIZE - 1;

    memcpy(config->allowed_origins[config->origin_count], origin, len);
    config->allowed_origins[config->origin_count][len] = '\0';
    config->origin_count++;
    return 1;
}

int kl_cors_parse_origins(KlCorsConfig *config, const char *origins) {
    if (!origins) return 0;

    int added = 0;
    const char *p = origins;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        /* Find end of origin (comma or end of string) */
        const char *start = p;
        while (*p && *p != ',') p++;

        /* Trim trailing whitespace */
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

        if (end > start) {
            size_t len = (size_t)(end - start);
            if (len >= KL_CORS_ORIGIN_SIZE) len = KL_CORS_ORIGIN_SIZE - 1;
            char buf[KL_CORS_ORIGIN_SIZE];
            memcpy(buf, start, len);
            buf[len] = '\0';
            if (kl_cors_add_origin(config, buf)) added++;
        }

        if (*p == ',') p++;
    }

    return added;
}

int kl_cors_is_allowed(const KlCorsConfig *config, const char *origin,
                        size_t origin_len) {
    if (!origin || origin_len == 0) return 0;
    if (config->origin_count == 0) return 1;  /* allow all */

    for (int i = 0; i < config->origin_count; i++) {
        size_t olen = strlen(config->allowed_origins[i]);
        if (olen == origin_len &&
            memcmp(config->allowed_origins[i], origin, olen) == 0)
            return 1;
    }
    return 0;
}

/* ── Middleware function ────────────────────────────────────────────── */

// cppcheck-suppress constParameterPointer
int kl_cors_middleware(KlRequest *req, KlResponse *res, void *user_data) {
    const KlCorsConfig *config = user_data;

    /* Extract Origin header */
    size_t origin_len;
    const char *origin = kl_request_header_len(req, "Origin", &origin_len);

    /* No Origin header — not a cross-origin request, continue */
    if (!origin) return 0;

    /* Check if origin is allowed */
    if (!kl_cors_is_allowed(config, origin, origin_len)) return 0;

    /* Determine the Allow-Origin value */
    char origin_buf[KL_CORS_ORIGIN_SIZE];
    const char *allow_origin;
    if (config->origin_count == 0) {
        allow_origin = "*";
    } else {
        /* Echo back the specific origin */
        size_t len = origin_len < KL_CORS_ORIGIN_SIZE - 1
                     ? origin_len : KL_CORS_ORIGIN_SIZE - 1;
        memcpy(origin_buf, origin, len);
        origin_buf[len] = '\0';
        allow_origin = origin_buf;
    }

    /* Add CORS headers */
    kl_response_header(res, "Access-Control-Allow-Origin", allow_origin);

    if (config->allow_credentials && config->origin_count > 0)
        kl_response_header(res, "Access-Control-Allow-Credentials", "true");

    /* Handle OPTIONS preflight */
    int is_options = (req->method_len == 7 &&
                      memcmp(req->method, "OPTIONS", 7) == 0);
    if (is_options) {
        if (config->allowed_methods)
            kl_response_header(res, "Access-Control-Allow-Methods",
                               config->allowed_methods);
        if (config->allowed_headers)
            kl_response_header(res, "Access-Control-Allow-Headers",
                               config->allowed_headers);
        if (config->max_age_seconds > 0) {
            char age[16];
            snprintf(age, sizeof(age), "%d", config->max_age_seconds);
            kl_response_header(res, "Access-Control-Max-Age", age);
        }

        kl_response_status(res, 204);
        kl_response_body(res, "", 0);
        return 1;  /* short-circuit — preflight handled */
    }

    return 0;  /* continue to handler */
}
