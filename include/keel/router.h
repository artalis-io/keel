#ifndef KEEL_ROUTER_H
#define KEEL_ROUTER_H

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <stddef.h>

typedef void (*KlHandler)(KlRequest *req, KlResponse *res, void *user_data);

typedef struct {
    const char *name;   size_t name_len;
    const char *value;  size_t value_len;
} KlParam;

#define KL_MAX_PARAMS 16

typedef struct {
    const char *method;
    const char *pattern;
    KlHandler handler;
    void *user_data;
    KlBodyReaderFactory body_reader;   /* NULL = discard body */
} KlRoute;

typedef struct {
    KlRoute *routes;
    int count;
    int capacity;
    KlAllocator *alloc;
} KlRouter;

int  kl_router_init(KlRouter *r, KlAllocator *alloc);
int  kl_router_add(KlRouter *r, const char *method, const char *pattern,
                   KlHandler handler, void *user_data,
                   KlBodyReaderFactory body_reader);
int  kl_router_match(KlRouter *r, const char *method, size_t method_len,
                     const char *path, size_t path_len,
                     KlRoute **matched, KlParam *params, int *num_params);
void kl_router_free(KlRouter *r);

#endif
