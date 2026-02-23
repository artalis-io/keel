#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

/*
 * Demonstrates multipart/form-data file upload.
 *
 *   curl -F "name=Alice" -F "file=@photo.jpg" localhost:8080/upload
 */

#define MAX_PART_SIZE  (4 << 20)   /* 4 MB per part */
#define MAX_TOTAL_SIZE (16 << 20)  /* 16 MB total */
#define MAX_PARTS      8

static KlMultipartConfig mp_config = {
    .max_part_size  = MAX_PART_SIZE,
    .max_total_size = MAX_TOTAL_SIZE,
    .max_parts      = MAX_PARTS,
};

static void handle_upload(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlMultipartReader *mr = (KlMultipartReader *)req->body_reader;
    if (!mr || mr->num_parts == 0) {
        kl_response_error(res, 400, "No parts received");
        return;
    }

    /* Print received parts */
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

    /* Respond with summary */
    char body[1024];
    int off = snprintf(body, sizeof(body),
                       "Received %d part(s)\n", mr->num_parts);
    if (off < 0) off = 0;
    for (int i = 0; i < mr->num_parts && off < (int)sizeof(body) - 128; i++) {
        KlMultipartPart *p = &mr->parts[i];
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "  %s: %zu bytes%s%s\n",
                        p->name, p->data_len,
                        p->filename ? " (file: " : "",
                        p->filename ? p->filename : "");
        if (p->filename)
            off += snprintf(body + off, sizeof(body) - (size_t)off, ")");
    }

    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    kl_response_body(res, body, (size_t)off);
}

static void handle_index(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    const char *html =
        "<form method='POST' action='/upload' enctype='multipart/form-data'>"
        "  <input name='name' placeholder='Your name'><br>"
        "  <input name='file' type='file'><br>"
        "  <button>Upload</button>"
        "</form>";
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/html");
    kl_response_body(res, html, strlen(html));
}

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};
    if (kl_server_init(&s, &cfg) < 0) {
        fprintf(stderr, "kl_server_init failed\n");
        return 1;
    }

    kl_server_route(&s, "GET", "/", handle_index, NULL, NULL);
    kl_server_route(&s, "POST", "/upload", handle_upload,
                    &mp_config, kl_body_reader_multipart);

    printf("multipart example listening on :8080\n");
    printf("  curl -F 'name=Alice' -F 'file=@photo.jpg' localhost:8080/upload\n");
    printf("  open http://localhost:8080\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
