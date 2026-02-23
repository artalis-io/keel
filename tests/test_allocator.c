#include "utest.h"
#include <keel/allocator.h>

UTEST(allocator, default_malloc_free) {
    KlAllocator a = kl_allocator_default();
    void *p = kl_malloc(&a, 128);
    ASSERT_TRUE(p != NULL);
    kl_free(&a, p, 128);
}

UTEST(allocator, default_realloc) {
    KlAllocator a = kl_allocator_default();
    void *p = kl_malloc(&a, 64);
    ASSERT_TRUE(p != NULL);
    void *p2 = kl_realloc(&a, p, 64, 256);
    ASSERT_TRUE(p2 != NULL);
    kl_free(&a, p2, 256);
}

/* Tracking allocator for leak detection */
typedef struct {
    int allocs;
    int frees;
    size_t total_allocated;
} TrackCtx;

static void *track_malloc(void *ctx, size_t size) {
    TrackCtx *t = ctx;
    t->allocs++;
    t->total_allocated += size;
    return malloc(size);
}

static void *track_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    TrackCtx *t = ctx;
    t->total_allocated -= old_size;
    t->total_allocated += new_size;
    return realloc(ptr, new_size);
}

static void track_free(void *ctx, void *ptr, size_t size) {
    TrackCtx *t = ctx;
    t->frees++;
    t->total_allocated -= size;
    free(ptr);
}

UTEST(allocator, custom_tracking) {
    TrackCtx track = {0};
    KlAllocator a = {
        .malloc = track_malloc,
        .realloc = track_realloc,
        .free = track_free,
        .ctx = &track,
    };

    void *p1 = kl_malloc(&a, 100);
    void *p2 = kl_malloc(&a, 200);
    ASSERT_EQ(track.allocs, 2);
    ASSERT_EQ(track.total_allocated, (size_t)300);

    kl_free(&a, p1, 100);
    kl_free(&a, p2, 200);
    ASSERT_EQ(track.frees, 2);
    ASSERT_EQ(track.total_allocated, (size_t)0);
}

UTEST(allocator, realloc_tracking) {
    TrackCtx track = {0};
    KlAllocator a = {
        .malloc = track_malloc,
        .realloc = track_realloc,
        .free = track_free,
        .ctx = &track,
    };

    void *p = kl_malloc(&a, 64);
    ASSERT_EQ(track.total_allocated, (size_t)64);

    p = kl_realloc(&a, p, 64, 256);
    ASSERT_EQ(track.total_allocated, (size_t)256);

    kl_free(&a, p, 256);
    ASSERT_EQ(track.total_allocated, (size_t)0);
}

UTEST_MAIN();
