/**
 * @file redirect.h
 * @brief HTTP redirect following (sync + async).
 *
 * Orthogonal redirect module that wraps the existing client APIs
 * with automatic 3xx redirect following. Does not modify client.h
 * or client.c. Supports both unpooled and pooled variants.
 */

#ifndef KEEL_REDIRECT_H
#define KEEL_REDIRECT_H

#include <keel/allocator.h>
#include <keel/client.h>
#include <keel/client_pool.h>
#include <keel/error.h>
#include <keel/event_ctx.h>
#include <keel/url.h>

/* ── Constants ────────────────────────────────────────────────────── */

/** @brief Default maximum redirects. */
#define KL_REDIRECT_DEFAULT_MAX 10

/* ── Config ───────────────────────────────────────────────────────── */

typedef struct {
    int max_redirects;   /**< Max redirects to follow (0 = default 10) */
} KlRedirectConfig;

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
int kl_redirect_request(KlAllocator *alloc, const KlClientConfig *cfg,
                        const KlRedirectConfig *redir,
                        const char *method, const char *url,
                        const KlClientHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        KlClientResponse *resp);

/**
 * @brief Synchronous pooled HTTP request with automatic redirect following.
 */
int kl_redirect_request_pooled(KlClientPool *pool,
                               KlAllocator *alloc, const KlClientConfig *cfg,
                               const KlRedirectConfig *redir,
                               const char *method, const char *url,
                               const KlClientHeader *headers, int num_headers,
                               const char *body, size_t body_len,
                               KlClientResponse *resp);

/* ── Async API ────────────────────────────────────────────────────── */

typedef struct KlRedirectClient KlRedirectClient;

/**
 * @brief Callback invoked when an async redirect-following request completes.
 */
typedef void (*KlRedirectDoneFn)(KlRedirectClient *rc, void *user_data);

/**
 * @brief Start an asynchronous HTTP request with automatic redirect following.
 * @return Client handle, or NULL on immediate failure.
 */
KlRedirectClient *kl_redirect_start(KlEventCtx *ev_ctx, KlAllocator *alloc,
                                    const KlClientConfig *cfg,
                                    const KlRedirectConfig *redir,
                                    const char *method, const char *url,
                                    const KlClientHeader *headers, int num_headers,
                                    const char *body, size_t body_len,
                                    KlRedirectDoneFn on_done, void *user_data);

/**
 * @brief Start an asynchronous pooled HTTP request with redirect following.
 * @return Client handle, or NULL on immediate failure.
 */
KlRedirectClient *kl_redirect_start_pooled(KlClientPool *pool,
                                           KlEventCtx *ev_ctx, KlAllocator *alloc,
                                           const KlClientConfig *cfg,
                                           const KlRedirectConfig *redir,
                                           const char *method, const char *url,
                                           const KlClientHeader *headers, int num_headers,
                                           const char *body, size_t body_len,
                                           KlRedirectDoneFn on_done, void *user_data);

/**
 * @brief Get the final response from a completed redirect client.
 * @return Response pointer (valid until kl_redirect_free), or NULL if error.
 */
const KlClientResponse *kl_redirect_response(const KlRedirectClient *rc);

/**
 * @brief Check if the redirect request completed with an error.
 * @return 0 on success, -1 on error.
 */
int kl_redirect_error(const KlRedirectClient *rc);

/**
 * @brief Get the specific error code from a completed redirect request.
 */
KlError kl_redirect_last_error(const KlRedirectClient *rc);

/**
 * @brief Cancel an in-flight redirect request.
 */
void kl_redirect_cancel(KlRedirectClient *rc);

/**
 * @brief Free all redirect client resources.
 */
void kl_redirect_free(KlRedirectClient *rc);

#endif /* KEEL_REDIRECT_H */
