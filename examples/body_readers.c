/*
 * body_readers.c — Buffer and multipart body readers
 *
 * Concepts: kl_body_reader_buffer, kl_body_reader_multipart,
 * KlBufReader, KlMultipartReader, KlMultipartConfig, per-route
 * body reader factories.
 *
 * Build:  make examples
 * Run:    ./examples/body_readers
 * Test:   curl -X POST -d 'hello world' localhost:8080/echo
 *         curl -F "name=Alice" -F "file=@photo.jpg" localhost:8080/upload
 *         open http://localhost:8080
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

#define MAX_BODY_SIZE  (1 << 20)   /* 1 MB */

static KlMultipartConfig mp_config = {
    .max_part_size  = 4 << 20,     /* 4 MB per part */
    .max_total_size = 16 << 20,    /* 16 MB total */
    .max_parts      = 8,
};

/* ── Handlers ───────────────────────────────────────────────────────── */

/* HTML form with text input + file upload */
static void handle_index(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    const char *html =
        "<h2>Body Readers</h2>"
        "<h3>Echo (buffer reader)</h3>"
        "<form method='POST' action='/echo'>"
        "  <textarea name='body' rows='3' cols='40'>hello world</textarea><br>"
        "  <button>Echo</button>"
        "</form>"
        "<h3>Upload (multipart reader)</h3>"
        "<form method='POST' action='/upload' enctype='multipart/form-data'>"
        "  <input name='name' placeholder='Your name'><br>"
        "  <input name='file' type='file'><br>"
        "  <button>Upload</button>"
        "</form>";
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/html");
    kl_response_body_borrow(res, html, strlen(html));
}

/* POST /echo — buffer reader echoes body back */
static void handle_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_response_error(res, 400, "Request body required");
        return;
    }
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/octet-stream");
    kl_response_body_borrow(res, br->data, br->len);
}

/* POST /upload — multipart reader parses form-data */
static void handle_upload(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlMultipartReader *mr = (KlMultipartReader *)req->body_reader;
    if (!mr || mr->num_parts == 0) {
        kl_response_error(res, 400, "No parts received");
        return;
    }

    printf("Received %d part(s):\n", mr->num_parts);
    for (int i = 0; i < mr->num_parts; i++) {
        KlMultipartPart *p = &mr->parts[i];
        printf("  [%d] name=\"%s\"", i, p->name);
        if (p->filename)
            printf(" filename=\"%s\"", p->filename);
        if (p->content_type)
            printf(" type=\"%s\"", p->content_type);
        printf(" size=%zu\n", p->data_len);
    }

    /* Summary response — static buffer (single-threaded, must outlive writev) */
    static char body[1024];
    int off = snprintf(body, sizeof(body),
                       "Received %d part(s)\n", mr->num_parts);
    if (off < 0) off = 0;
    for (int i = 0; i < mr->num_parts && off < (int)sizeof(body) - 128; i++) {
        KlMultipartPart *p = &mr->parts[i];
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "  %s: %zu bytes%s%s%s\n",
                        p->name, p->data_len,
                        p->filename ? " (file: " : "",
                        p->filename ? p->filename : "",
                        p->filename ? ")" : "");
    }

    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    kl_response_body_borrow(res, body, (size_t)off);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    KlServer s;
    KlConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_server_init(&s, &cfg) < 0) return 1;

    kl_server_route(&s, "GET",  "/",       handle_index,  NULL, NULL);
    kl_server_route(&s, "POST", "/echo",   handle_echo,
                    (void *)(size_t)MAX_BODY_SIZE, kl_body_reader_buffer);
    kl_server_route(&s, "POST", "/upload", handle_upload,
                    &mp_config, kl_body_reader_multipart);

    printf("body_readers example listening on :8080\n");
    printf("  curl -X POST -d 'hello world' localhost:8080/echo\n");
    printf("  curl -F 'name=Alice' -F 'file=@photo.jpg' localhost:8080/upload\n");
    printf("  open http://localhost:8080\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
