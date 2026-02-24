#ifndef KEEL_BODY_READER_MULTIPART_H
#define KEEL_BODY_READER_MULTIPART_H

#include <keel/body_reader.h>
#include <keel/request.h>
#include <stddef.h>

#define KL_MP_MAX_BOUNDARY 70   /* RFC 2046 */

typedef struct {
    const char *name;           /* null-terminated, allocated */
    const char *filename;       /* null-terminated or NULL */
    const char *content_type;   /* null-terminated or NULL */
    char *data;
    size_t data_len;
    size_t data_cap;            /* allocation capacity */
    size_t name_len;            /* strlen(name), stored to avoid recalc on free */
    size_t filename_len;        /* strlen(filename) or 0 */
    size_t content_type_len;    /* strlen(content_type) or 0 */
} KlMultipartPart;

typedef struct {
    size_t max_part_size;       /* 0 = unlimited */
    size_t max_total_size;      /* 0 = unlimited */
    int max_parts;              /* 0 = unlimited */
} KlMultipartConfig;

typedef enum {
    KL_MP_PREAMBLE,
    KL_MP_AFTER_BOUNDARY,  /* boundary found, waiting for \r\n or -- */
    KL_MP_HEADERS,
    KL_MP_BODY,
    KL_MP_DONE,
    KL_MP_ERROR
} KlMultipartState;

typedef struct {
    KlBodyReader base;
    KlAllocator *alloc;

    /* "\r\n--" + boundary for body scanning */
    char delimiter[KL_MP_MAX_BOUNDARY + 6];
    size_t delimiter_len;

    /* Parts (growable array) */
    KlMultipartPart *parts;
    int num_parts;
    int parts_cap;

    /* Limits */
    KlMultipartConfig config;
    size_t total_received;

    /* State machine */
    KlMultipartState state;

    /* Overlap buffer: last (delimiter_len - 1) bytes from previous on_data,
     * to detect boundaries spanning chunks */
    char overlap[KL_MP_MAX_BOUNDARY + 6];
    size_t overlap_len;

    /* Part header accumulator */
    char hdr_buf[2048];
    size_t hdr_len;
} KlMultipartReader;

/**
 * @brief Factory: create a multipart/form-data body reader.
 *
 * Extracts the boundary from Content-Type. Returns NULL (triggering 415)
 * if the content type is not multipart/form-data or has no boundary.
 *
 * @param alloc     Allocator for parts and buffers.
 * @param req       Parsed request (Content-Type must be set).
 * @param user_data KlMultipartConfig pointer, or NULL for defaults.
 * @return Multipart body reader, or NULL on rejection.
 */
KlBodyReader *kl_body_reader_multipart(KlAllocator *alloc, const KlRequest *req,
                                        void *user_data);

#endif
