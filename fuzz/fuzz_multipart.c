/*
 * libFuzzer harness for the multipart body reader.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_multipart fuzz/corpus_multipart/ -max_total_time=60
 */
#include <keel/allocator.h>
#include <keel/body_reader.h>
#include <keel/body_reader_multipart.h>
#include <keel/request.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlAllocator alloc = kl_allocator_default();

    /* Set up a fake request with multipart content-type */
    KlRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.content_length = size;

    const char *ct = "multipart/form-data; boundary=FUZZBOUNDARY";
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = ct;
    req.headers[0].value_len = strlen(ct);
    req.num_headers = 1;

    KlMultipartConfig cfg = {
        .max_part_size = 64 * 1024,
        .max_total_size = 256 * 1024,
        .max_parts = 16,
    };

    KlBodyReader *reader = kl_body_reader_multipart(&alloc, &req, &cfg);
    if (!reader) return 0;

    /* Feed data in variable-sized chunks */
    size_t offset = 0;
    while (offset < size) {
        /* Vary chunk size based on data content for better coverage */
        size_t chunk = 1;
        if (offset < size)
            chunk = (data[offset] % 64) + 1;
        if (chunk > size - offset)
            chunk = size - offset;

        int rc = reader->on_data(reader, (const char *)data + offset, chunk);
        if (rc < 0) break;
        offset += chunk;
    }

    if (offset >= size)
        reader->on_complete(reader);
    else
        reader->on_error(reader);

    reader->destroy(reader);
    return 0;
}
