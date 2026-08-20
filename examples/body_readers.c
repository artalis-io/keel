/*
 * body_readers.c — Buffer and multipart body readers
 *
 * Concepts: kl_body_reader_buffer, kl_body_reader_multipart,
 * KlBufReader, kl_multipart_next streaming iterator,
 * KlMultipartConfig, per-route body reader factories.
 *
 * Build:  make examples
 * Run:    ./examples/body_readers
 * Test:   curl -X POST -d 'hello world' localhost:8080/echo
 *         curl -F "name=Alice" -F "file=@photo.jpg" localhost:8080/upload
 *         open http://localhost:8080
 */

#include <keel/keel.h>
#include <keel/body_reader_multipart.h>
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
static void handle_index(KlRequest *req, KlHttpResponse *res, void *ctx) {
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
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/html");
    kl_http_response_body_borrow(res, html, strlen(html));
}

/* POST /echo — buffer reader echoes body back */
static void handle_echo(KlRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_http_response_error(res, 400, "Request body required");
        return;
    }
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    kl_http_response_body_borrow(res, br->data, br->len);
}

/* POST /upload — multipart streaming iterator parses form-data.
 *
 * Drives kl_multipart_next() to walk the events. The full body has
 * been received by the time the handler runs, so NEED_DATA never
 * fires here (it would in a handler that yields mid-stream). */
static void handle_upload(KlRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlBodyReader *br = req->body_reader;
    if (!br) {
        kl_http_response_error(res, 400, "No reader");
        return;
    }

    /* Static so the slice handed to kl_http_response_body_borrow outlives the
     * handler return. Capped at 8 parts for the demo. */
    static char  body[1024];
    static char  names[8][128];
    static char  fnames[8][128];
    static size_t sizes[8];
    static int   has_filename[8];
    int parts = 0;

    KlMultipartPartMeta meta;
    const char *d = NULL;
    size_t      dn = 0;
    for (;;) {
        KlMultipartEvent e = kl_multipart_next(br, &meta, &d, &dn);
        if (e == KL_MP_EVT_PART_BEGIN) {
            if (parts < 8) {
                size_t n = meta.name_len < sizeof(names[0]) - 1
                               ? meta.name_len : sizeof(names[0]) - 1;
                memcpy(names[parts], meta.name, n);
                names[parts][n] = '\0';
                has_filename[parts] = meta.filename != NULL;
                if (has_filename[parts]) {
                    size_t fn = meta.filename_len < sizeof(fnames[0]) - 1
                                    ? meta.filename_len : sizeof(fnames[0]) - 1;
                    memcpy(fnames[parts], meta.filename, fn);
                    fnames[parts][fn] = '\0';
                }
                sizes[parts] = 0;
            }
            parts++;
            continue;
        }
        if (e == KL_MP_EVT_PART_DATA) {
            if (parts > 0 && parts <= 8) sizes[parts - 1] += dn;
            continue;
        }
        if (e == KL_MP_EVT_PART_END) continue;
        if (e == KL_MP_EVT_DONE)     break;
        kl_http_response_error(res, 400, "Parse error");
        return;
    }
    if (parts == 0) {
        kl_http_response_error(res, 400, "No parts received");
        return;
    }

    printf("Received %d part(s):\n", parts);
    for (int i = 0; i < parts && i < 8; i++) {
        printf("  [%d] name=\"%s\"", i, names[i]);
        if (has_filename[i]) printf(" filename=\"%s\"", fnames[i]);
        printf(" size=%zu\n", sizes[i]);
    }

    int off = snprintf(body, sizeof(body),
                       "Received %d part(s)\n", parts);
    if (off < 0) off = 0;
    for (int i = 0; i < parts && i < 8 && off < (int)sizeof(body) - 128; i++) {
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "  %s: %zu bytes%s%s%s\n",
                        names[i], sizes[i],
                        has_filename[i] ? " (file: " : "",
                        has_filename[i] ? fnames[i] : "",
                        has_filename[i] ? ")" : "");
    }

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/plain");
    kl_http_response_body_borrow(res, body, (size_t)off);
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
