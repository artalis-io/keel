/**
 * @file http_redirect.h
 * @brief HTTP redirect following (sync + async).
 *
 * Orthogonal redirect module that wraps the existing client APIs
 * with automatic 3xx redirect following. Does not modify http_client.h
 * or client.c. Supports both unpooled and pooled variants.
 */

#ifndef KEEL_HTTP_REDIRECT_H
#define KEEL_HTTP_REDIRECT_H

#include <keel/allocator.h>
#include <keel/http_client.h>
#include <keel/http_client_pool.h>
#include <keel/error.h>
#include <keel/event_ctx.h>
#include <keel/url.h>
#ifdef __cplusplus
extern "C" {
#endif


/* ── Constants ────────────────────────────────────────────────────── */

/** @brief Default maximum redirects. */
#define KL_HTTP_REDIRECT_DEFAULT_MAX 10

/* ── Config ───────────────────────────────────────────────────────── */

/*
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default. New members are appended
 * after max_redirects.
 */
typedef struct {
    /**
     * Maximum number of redirect HOPS to follow.  `max_redirects = N`
     * means up to N redirect responses are chased; the total request
     * count is therefore N + 1 (the initial request plus N follow-ups).
     * Matches curl / fetch / most HTTP clients.  0 = use default 10.
     */
    int max_redirects;
} KlHttpRedirectConfig;

/* ── Sync API ─────────────────────────────────────────────────────── */

/**
 * @brief Synchronous HTTP request with automatic redirect following.
 *
 * Follows 301/302/303/307/308 redirects up to max_redirects hops.
 * Method transformation per RFC 7231/7538: 301/302/303 change
 * POST/PUT/PATCH to GET (dropping body); 307/308 preserve method.
 *
 * @return 0 on success, -1 on error. Sets resp->error on failure.
 */
int kl_http_redirect_request(KlAllocator *alloc, const KlHttpClientConfig *cfg,
                        const KlHttpRedirectConfig *redir,
                        const char *method, const char *url,
                        const KlHttpClientHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        KlHttpClientResponse *resp);

/**
 * @brief Synchronous pooled HTTP request with automatic redirect following.
 */
int kl_http_redirect_request_pooled(KlHttpClientPool *pool,
                               KlAllocator *alloc, const KlHttpClientConfig *cfg,
                               const KlHttpRedirectConfig *redir,
                               const char *method, const char *url,
                               const KlHttpClientHeader *headers, int num_headers,
                               const char *body, size_t body_len,
                               KlHttpClientResponse *resp);

/* ── Async API ────────────────────────────────────────────────────── */

typedef struct KlHttpRedirectClient KlHttpRedirectClient;

/**
 * @brief Callback invoked when an async redirect-following request completes.
 */
typedef void (*KlHttpRedirectDoneFn)(KlHttpRedirectClient *rc, void *user_data);

/**
 * @brief Start an asynchronous HTTP request with automatic redirect following.
 * @return Client handle, or NULL on immediate failure.
 */
KlHttpRedirectClient *kl_http_redirect_start(KlEventCtx *ev_ctx, KlAllocator *alloc,
                                    const KlHttpClientConfig *cfg,
                                    const KlHttpRedirectConfig *redir,
                                    const char *method, const char *url,
                                    const KlHttpClientHeader *headers, int num_headers,
                                    const char *body, size_t body_len,
                                    KlHttpRedirectDoneFn on_done, void *user_data);

/**
 * @brief Start an asynchronous pooled HTTP request with redirect following.
 * @return Client handle, or NULL on immediate failure.
 */
KlHttpRedirectClient *kl_http_redirect_start_pooled(KlHttpClientPool *pool,
                                           KlEventCtx *ev_ctx, KlAllocator *alloc,
                                           const KlHttpClientConfig *cfg,
                                           const KlHttpRedirectConfig *redir,
                                           const char *method, const char *url,
                                           const KlHttpClientHeader *headers, int num_headers,
                                           const char *body, size_t body_len,
                                           KlHttpRedirectDoneFn on_done, void *user_data);

/**
 * @brief Get the final response from a completed redirect client.
 *
 * Valid until @ref kl_http_redirect_free.  Returns NULL when the request
 * terminated in an error state (whether the failure occurred on the
 * initial request, a redirect-following step, or because the
 * @ref kl_http_redirect_start call itself never produced an inner client).
 * Callers should pair this with @ref kl_http_redirect_last_error to
 * distinguish "no response yet" from "no response ever."
 */
const KlHttpClientResponse *kl_http_redirect_response(const KlHttpRedirectClient *rc);

/**
 * @brief Check if the redirect request completed with an error.
 * @return 0 on success, -1 on error.
 */
int kl_http_redirect_error(const KlHttpRedirectClient *rc);

/**
 * @brief Get the specific error code from a completed redirect request.
 *
 * Defined post-completion (after the @ref KlHttpRedirectDoneFn fires).
 * Together with @ref kl_http_redirect_response NULL, a non-zero error here
 * is the only indicator a caller has that no response was ever
 * received; there is no separate "inner client construction failed"
 * vs. "all redirect hops failed" distinction in the public API.
 */
KlError kl_http_redirect_last_error(const KlHttpRedirectClient *rc);

/**
 * @brief Cancel an in-flight redirect request.
 */
void kl_http_redirect_cancel(KlHttpRedirectClient *rc);

/**
 * @brief Free all redirect client resources.
 */
void kl_http_redirect_free(KlHttpRedirectClient *rc);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_HTTP_REDIRECT_H */
