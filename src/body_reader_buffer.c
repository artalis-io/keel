#include <keel/body_reader.h>
#include <string.h>
#include <stdint.h>

#define KL_BUF_READER_INIT_CAP 1024

static int buf_on_data(KlBodyReader *self, const char *data, size_t len) {
    KlBufReader *br = (KlBufReader *)self;

    if (br->max_size > 0 && len > br->max_size - br->len)
        return -1;  /* exceeded limit */

    if (len > br->cap - br->len) {
        size_t need = br->len + len;  /* safe: checked above or cap >= len */
        size_t new_cap;
        if (br->cap > SIZE_MAX / 2)
            new_cap = need;
        else
            new_cap = br->cap * 2;
        if (new_cap < need)
            new_cap = need;
        if (br->max_size > 0 && new_cap > br->max_size)
            new_cap = br->max_size;

        char *new_data = kl_realloc(br->alloc, br->data, br->cap, new_cap);
        if (!new_data) return -1;
        br->data = new_data;
        br->cap = new_cap;
    }

    memcpy(br->data + br->len, data, len);
    br->len += len;
    return 0;
}

static void buf_on_complete(KlBodyReader *self) {
    (void)self;
}

static void buf_on_error(KlBodyReader *self) {
    (void)self;
}

static void buf_destroy(KlBodyReader *self) {
    KlBufReader *br = (KlBufReader *)self;
    if (br->data)
        kl_free(br->alloc, br->data, br->cap);
    kl_free(br->alloc, br, sizeof(KlBufReader));
}

/* user_data is void* (not const) to match the KlBodyReaderFactory typedef — it
 * carries max_size by value, not a pointer to read. */
/* cppcheck-suppress constParameterPointer */
KlBodyReader *kl_body_reader_buffer(KlAllocator *alloc, const KlRequest *req, void *user_data) {
    (void)req;

    KlBufReader *br = kl_malloc(alloc, sizeof(KlBufReader));
    if (!br) return NULL;

    size_t max_size = (size_t)user_data;
    size_t init_cap = KL_BUF_READER_INIT_CAP;
    if (max_size > 0 && init_cap > max_size)
        init_cap = max_size;

    br->base.on_data = buf_on_data;
    br->base.on_complete = buf_on_complete;
    br->base.on_error = buf_on_error;
    br->base.destroy = buf_destroy;
    br->alloc = alloc;
    br->data = kl_malloc(alloc, init_cap);
    if (!br->data) {
        kl_free(alloc, br, sizeof(KlBufReader));
        return NULL;
    }
    br->len = 0;
    br->cap = init_cap;
    br->max_size = max_size;

    return &br->base;
}
