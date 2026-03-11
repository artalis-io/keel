/*
 * h2_client.c — HTTP/2 client demo with mock session
 *
 * Demonstrates the KlH2ClientSession vtable interface using a stub
 * session. In a real deployment, replace StubH2ClientSession with
 * an nghttp2-based wrapper.
 *
 * Build:  make examples
 * Run:    ./examples/h2_client
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Stub H2 Client Session ──────────────────────────────────────── */

typedef struct {
    KlH2ClientSession base;
    KlAllocator *alloc;
    int32_t next_stream_id;
} StubH2ClientSession;

static int stub_recv(KlH2ClientSession *self, const char *data, size_t len) {
    (void)self; (void)data; (void)len;
    /* A real implementation would parse HTTP/2 frames here */
    return 0;
}

static int32_t stub_submit_request(KlH2ClientSession *self,
                                    const char *method, const char *path,
                                    const char *authority,
                                    const KlH2ClientHeader *hdrs, int n,
                                    const char *body, size_t body_len) {
    StubH2ClientSession *s = (StubH2ClientSession *)self;
    (void)hdrs; (void)n; (void)body; (void)body_len;
    printf("  Submit: %s %s (authority: %s)\n", method, path, authority);

    int32_t stream_id = s->next_stream_id;
    s->next_stream_id += 2;  /* HTTP/2 client streams are odd */

    /* In a real implementation, this would send HEADERS + DATA frames
       via the on_send callback. For demo, we simulate a response. */
    if (self->keel_cbs.on_response) {
        self->keel_cbs.on_response(self, stream_id, 200, NULL, 0);
    }
    if (self->keel_cbs.on_data) {
        const char *resp_body = "{\"msg\":\"mock h2 response\"}";
        self->keel_cbs.on_data(self, stream_id, resp_body, strlen(resp_body));
    }
    if (self->keel_cbs.on_stream_close) {
        self->keel_cbs.on_stream_close(self, stream_id, 0);
    }

    return stream_id;
}

static int stub_flush(KlH2ClientSession *self) {
    (void)self;
    return 0;
}

static void stub_destroy(KlH2ClientSession *self) {
    StubH2ClientSession *s = (StubH2ClientSession *)self;
    kl_free(s->alloc, s, sizeof(*s));
}

static KlH2ClientSession *stub_client_factory(KlAllocator *alloc) {
    StubH2ClientSession *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->next_stream_id = 1;
    s->base.recv = stub_recv;
    s->base.submit_request = stub_submit_request;
    s->base.flush = stub_flush;
    s->base.destroy = stub_destroy;
    return &s->base;
}

/* ── Callbacks ───────────────────────────────────────────────────── */

static void on_response(KlH2ClientConn *c, int32_t stream_id,
                        const KlH2ClientResponse *resp, void *ud) {
    (void)c; (void)ud;
    printf("Response on stream %d: status=%d body=%.*s\n",
           stream_id, resp->status,
           (int)resp->body_len, resp->body ? resp->body : "");
}

static void on_error(KlH2ClientConn *c, const char *msg, void *ud) {
    (void)c; (void)ud;
    fprintf(stderr, "Error: %s\n", msg);
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "event_ctx_init failed\n");
        return 1;
    }

    KlH2ClientConfig cfg = {
        .session = stub_client_factory,
    };

    printf("HTTP/2 client demo (mock session)\n");
    printf("Connecting to http://localhost:8080 ...\n");

    KlH2ClientConn *c = kl_h2_client_connect(&ev, &alloc, &cfg,
                                               "http://localhost:8080",
                                               on_error, NULL);
    if (!c) {
        fprintf(stderr, "connect failed\n");
        kl_event_ctx_free(&ev);
        return 1;
    }

    /* Submit two multiplexed requests */
    printf("Submitting requests:\n");
    kl_h2_client_request(c, "GET", "/api/hello", NULL, 0, NULL, 0,
                          on_response, NULL);
    kl_h2_client_request(c, "GET", "/api/world", NULL, 0, NULL, 0,
                          on_response, NULL);

    kl_h2_client_free(c);
    kl_event_ctx_free(&ev);
    printf("Done.\n");
    return 0;
}
