#ifndef KEEL_SERVER_H
#define KEEL_SERVER_H

#include <keel/allocator.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <keel/connection.h>
#include <keel/event.h>

typedef KlParser *(*KlParserFactory)(KlAllocator *alloc);

#define KL_DEFAULT_MAX_CONNS    256
#define KL_DEFAULT_READ_TIMEOUT 30000   /* ms */

typedef struct {
    int port;
    const char *bind_addr;      /* default: "0.0.0.0" */
    int max_connections;        /* default: KL_DEFAULT_MAX_CONNS */
    int read_timeout_ms;        /* default: KL_DEFAULT_READ_TIMEOUT */
    KlAllocator *alloc;         /* default: stdlib */
    KlParserFactory parser;     /* default: kl_parser_llhttp */
} KlConfig;

typedef struct {
    KlConfig config;
    KlAllocator alloc_storage;  /* owned copy if user didn't provide one */
    KlRouter router;
    KlConnPool pool;
    KlEventLoop loop;
    int listen_fd;
    volatile int running;
} KlServer;

int  kl_server_init(KlServer *s, KlConfig *config);
int  kl_server_route(KlServer *s, const char *method, const char *pattern,
                     KlHandler handler, void *user_data,
                     KlBodyReaderFactory body_reader);
int  kl_server_run(KlServer *s);
void kl_server_stop(KlServer *s);
void kl_server_free(KlServer *s);

#endif
