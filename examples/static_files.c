/*
 * static_files.c — Static file server with sendfile
 *
 * Concepts: kl_http_response_file (sendfile), MIME type detection, path
 * traversal guard, wildcard route.
 *
 * Build:  make examples
 * Run:    mkdir -p public && echo "<h1>Hi</h1>" > public/index.html
 *         ./examples/static_files
 * Test:   curl localhost:8080/index.html
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *docroot = "./public";

static const char *mime_type(const char *path, size_t len) {
    if (len >= 5 && memcmp(path + len - 5, ".html", 5) == 0) return "text/html";
    if (len >= 4 && memcmp(path + len - 4, ".css", 4) == 0)  return "text/css";
    if (len >= 3 && memcmp(path + len - 3, ".js", 3) == 0)   return "application/javascript";
    if (len >= 4 && memcmp(path + len - 4, ".png", 4) == 0)  return "image/png";
    if (len >= 4 && memcmp(path + len - 4, ".jpg", 4) == 0)  return "image/jpeg";
    if (len >= 5 && memcmp(path + len - 5, ".json", 5) == 0) return "application/json";
    return "application/octet-stream";
}

static void handle_static(KlRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;

    /* Reject path traversal — portable check (req->path is not null-terminated) */
    int has_dotdot = 0;
    for (size_t i = 0; i + 1 < req->path_len; i++) {
        if (req->path[i] == '.' && req->path[i + 1] == '.') { has_dotdot = 1; break; }
    }
    if (has_dotdot) {
        kl_http_response_error(res, 403, "Forbidden");
        return;
    }

    /* Build file path */
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%.*s",
             docroot, (int)req->path_len, req->path);

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        kl_http_response_error(res, 404, "Not Found");
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        kl_http_response_error(res, 404, "Not Found");
        return;
    }

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type",
                       mime_type(req->path, req->path_len));
    kl_http_response_file(res, fd, st.st_size);
}

int main(void) {
    KlServer s;
    KlConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_server_init(&s, &cfg) < 0) return 1;
    kl_server_route(&s, "GET", "/*", handle_static, NULL, NULL);

    printf("static_files example serving %s on :8080\n", docroot);
    printf("  curl localhost:8080/index.html\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
