#include "utest.h"
#include <keel/keel.h>
#include <string.h>

/*
 * Buffer-reader tests only.
 *
 * Multipart streaming tests live in tests/test_multipart_stream.c.
 */

UTEST(buf, create_destroy) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);
    br->destroy(br);
}

UTEST(buf, single_chunk) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    ASSERT_EQ(br->on_data(br, "hello", 5), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)5);
    ASSERT_TRUE(memcmp(b->data, "hello", 5) == 0);

    br->destroy(br);
}

UTEST(buf, multi_chunk) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);

    ASSERT_EQ(br->on_data(br, "abc", 3), 0);
    ASSERT_EQ(br->on_data(br, "def", 3), 0);
    ASSERT_EQ(br->on_data(br, "ghi", 3), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)9);
    ASSERT_TRUE(memcmp(b->data, "abcdefghi", 9) == 0);

    br->destroy(br);
}

UTEST(buf, growth) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);

    /* Feed > 1024 bytes (initial cap) to trigger realloc */
    char chunk[256];
    memset(chunk, 'A', sizeof(chunk));
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(br->on_data(br, chunk, sizeof(chunk)), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)(256 * 8));
    ASSERT_TRUE(b->cap >= b->len);

    br->destroy(br);
}

UTEST(buf, max_size_reject) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, (void *)(size_t)100);

    char data[60];
    memset(data, 'X', sizeof(data));
    ASSERT_EQ(br->on_data(br, data, 60), 0);
    /* This should exceed the 100-byte limit */
    ASSERT_EQ(br->on_data(br, data, 60), -1);

    br->destroy(br);
}

UTEST(buf, exact_max_size) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, (void *)(size_t)100);

    char data[100];
    memset(data, 'Y', sizeof(data));
    ASSERT_EQ(br->on_data(br, data, 100), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)100);

    br->destroy(br);
}

UTEST(buf, empty_body) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);

    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)0);

    br->destroy(br);
}

UTEST_MAIN();
