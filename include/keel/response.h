#ifndef KEEL_RESPONSE_H
#define KEEL_RESPONSE_H

#include <keel/allocator.h>
#include <stddef.h>
#include <sys/types.h>

/* Pluggable write callback — same signature as sh_json's ShJsonWriteFn */
typedef int (*KlWriteFn)(void *ctx, const char *data, size_t len);

typedef enum {
    KL_BODY_NONE,
    KL_BODY_BUFFER,
    KL_BODY_FILE,
    KL_BODY_STREAM
} KlBodyMode;

typedef struct {
    KlAllocator *alloc;
    int conn_fd;

    /* Header buffer (allocated, grows via allocator) */
    char *hdr_buf;
    size_t hdr_len;
    size_t hdr_cap;

    int status;
    int headers_sent;
    KlBodyMode body_mode;

    /* KL_BODY_BUFFER */
    const char *body;
    size_t body_len;

    /* KL_BODY_FILE */
    int file_fd;
    off_t file_size;
    off_t file_offset;

    /* KL_BODY_STREAM */
    int stream_error;
} KlResponse;

int  kl_response_init(KlResponse *res, KlAllocator *alloc);
void kl_response_reset(KlResponse *res);  /* fast reinit for keep-alive */
void kl_response_status(KlResponse *res, int code);
/* name and value must be null-terminated C strings */
void kl_response_header(KlResponse *res, const char *name, const char *value);
void kl_response_body(KlResponse *res, const char *data, size_t len);
void kl_response_file(KlResponse *res, int fd, off_t size);
void kl_response_free(KlResponse *res);

/* Convenience */
void kl_response_json(KlResponse *res, int code, const char *json, size_t len);
void kl_response_error(KlResponse *res, int code, const char *message);

/* Streaming API */
int kl_response_begin_stream(KlResponse *res, int status,
                             KlWriteFn *out_write, void **out_ctx);
int kl_response_end_stream(KlResponse *res);

/* Internal — flush headers + body to fd. Returns bytes remaining or -1 on error. */
int kl_response_send(KlResponse *res);

#endif
