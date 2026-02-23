#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

void handle_get_users(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    const char *json = "[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]";
    kl_response_json(res, 200, json, strlen(json));
}

void handle_get_user(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    /* In a real app, you'd look up :id from route params */
    const char *json = "{\"id\":1,\"name\":\"Alice\"}";
    kl_response_json(res, 200, json, strlen(json));
}

void handle_create_user(KlRequest *req, KlResponse *res, void *ctx) {
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

void handle_not_found(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_error(res, 404, "Not Found");
}

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};

    if (kl_server_init(&s, &cfg) < 0) return 1;
    kl_server_route(&s, "GET",  "/api/users",     handle_get_users, NULL, NULL);
    kl_server_route(&s, "GET",  "/api/users/:id", handle_get_user, NULL, NULL);
    kl_server_route(&s, "POST", "/api/users",     handle_create_user, NULL,
                    kl_body_reader_buffer);

    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
