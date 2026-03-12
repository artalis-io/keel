/*
 * rest_api_server.c — REST API with route parameters and body reading
 *
 * Concepts: Route params (:id), query strings, POST body, KlBufReader.
 *
 * Build:  make examples
 * Run:    ./examples/rest_api_server
 * Test:   curl localhost:8080/api/users
 *         curl localhost:8080/api/users/42
 *         curl -X POST -d '{"name":"Eve"}' localhost:8080/api/users
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

static void handle_get_users(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    const char *json = "[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]";
    kl_response_json(res, 200, json, strlen(json));
}

static void handle_get_user(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    size_t id_len;
    const char *id = kl_request_param(req, "id", &id_len);
    if (!id) {
        kl_response_error(res, 400, "Missing id");
        return;
    }
    static char json[256]; /* static: single-threaded event loop, outlives writev */
    int n = snprintf(json, sizeof(json),
                     "{\"id\":%.*s,\"name\":\"Alice\"}", (int)id_len, id);
    if (n < 0) n = 0;
    kl_response_json(res, 200, json, (size_t)n);
}

static void handle_create_user(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_response_error(res, 400, "Request body required");
        return;
    }
    /* Echo back the body as "created" */
    kl_response_status(res, 201);
    kl_response_header(res, "Content-Type", "application/json");
    kl_response_body(res, br->data, br->len);
}

int main(void) {
    KlServer s;
    KlConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };

    if (kl_server_init(&s, &cfg) < 0) return 1;
    kl_server_route(&s, "GET",  "/api/users",     handle_get_users, NULL, NULL);
    kl_server_route(&s, "GET",  "/api/users/:id", handle_get_user, NULL, NULL);
    kl_server_route(&s, "POST", "/api/users",     handle_create_user, NULL,
                    kl_body_reader_buffer);

    printf("rest_api example listening on :8080\n");
    printf("  curl localhost:8080/api/users\n");
    printf("  curl localhost:8080/api/users/42\n");
    printf("  curl -X POST -d '{\"name\":\"Eve\"}' localhost:8080/api/users\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
