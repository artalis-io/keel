/*
 * custom_allocator.c — Tracking allocator that counts allocations
 *
 * Concepts: KlAllocator vtable (malloc, realloc, free with sizes),
 * tracking allocator, passing custom allocator to APIs,
 * verifying zero leaks on shutdown.
 *
 * Build:  make examples
 * Run:    ./examples/custom_allocator
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Tracking allocator ───────────────────────────────────────────── */

typedef struct {
    int alloc_count;
    int free_count;
    size_t total_bytes;
    size_t current_bytes;
} TrackingCtx;

static void *tracking_malloc(void *ctx, size_t size) {
    TrackingCtx *t = ctx;
    void *ptr = malloc(size);
    if (ptr) {
        t->alloc_count++;
        t->total_bytes += size;
        t->current_bytes += size;
    }
    return ptr;
}

static void *tracking_realloc(void *ctx, void *ptr, size_t old_size,
                               size_t new_size) {
    TrackingCtx *t = ctx;
    void *new_ptr = realloc(ptr, new_size);
    if (new_ptr) {
        t->current_bytes -= old_size;
        t->current_bytes += new_size;
        if (!ptr) t->alloc_count++;
        t->total_bytes += new_size;
    }
    return new_ptr;
}

static void tracking_free(void *ctx, void *ptr, size_t size) {
    TrackingCtx *t = ctx;
    if (ptr) {
        t->free_count++;
        t->current_bytes -= size;
    }
    free(ptr);
}

/* ── Demo ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("custom_allocator example\n\n");

    TrackingCtx tracking = {0};
    KlAllocator alloc = {
        .malloc  = tracking_malloc,
        .realloc = tracking_realloc,
        .free    = tracking_free,
        .ctx     = &tracking,
    };

    /* Use the allocator with URL parsing */
    printf("--- URL parsing (zero-alloc) ---\n");
    KlUrl url;
    int rc = kl_url_parse("https://example.com:443/api/v1?key=val", &url);
    printf("  parse result: %d\n", rc);
    printf("  host: %.*s, port: %d, path: %.*s\n",
           (int)url.host_len, url.host, url.port,
           (int)url.path_len, url.path);
    printf("  allocs after URL parse: %d (should be 0)\n",
           tracking.alloc_count);

    /* Use the allocator with kl_malloc/kl_free */
    printf("\n--- Manual allocation ---\n");
    void *buf = kl_malloc(&alloc, 256);
    printf("  allocated 256 bytes (count=%d, current=%zu)\n",
           tracking.alloc_count, tracking.current_bytes);

    buf = kl_realloc(&alloc, buf, 256, 512);
    printf("  reallocated to 512 bytes (current=%zu)\n",
           tracking.current_bytes);

    kl_free(&alloc, buf, 512);
    printf("  freed 512 bytes (current=%zu)\n", tracking.current_bytes);

    /* Summary */
    printf("\n--- Summary ---\n");
    printf("  total allocations: %d\n", tracking.alloc_count);
    printf("  total frees:       %d\n", tracking.free_count);
    printf("  total bytes:       %zu\n", tracking.total_bytes);
    printf("  leaked bytes:      %zu\n", tracking.current_bytes);
    printf("  leak-free:         %s\n",
           tracking.current_bytes == 0 ? "YES" : "NO");

    return tracking.current_bytes != 0;
}
