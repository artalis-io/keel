/*
 * libFuzzer harness for the WebSocket frame parser.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_websocket fuzz/corpus_websocket/ -max_total_time=60
 */
#include <keel/websocket.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t offset = 0;

    while (offset < size) {
        /* Vary chunk size based on data content for better coverage */
        size_t chunk = 1;
        if (offset < size)
            chunk = (data[offset] % 32) + 1;
        if (chunk > size - offset)
            chunk = size - offset;

        size_t consumed = 0;
        int rc = kl_ws_frame_parse(&fp, data + offset, chunk, &consumed);

        offset += consumed;

        if (rc < 0) {
            /* Protocol error; reset and try from current position */
            kl_ws_frame_init(&fp);
            if (consumed == 0) offset++;  /* skip bad byte */
            continue;
        }

        if (rc == 1) {
            /* Frame complete; reset for next frame */
            kl_ws_frame_init(&fp);
        } else if (consumed == 0) {
            /* No progress, need more data; advance to avoid stall */
            break;
        }
    }

    /* Second pass: feed all at once to exercise single-call path */
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    kl_ws_frame_parse(&fp, data, size, &consumed);

    return 0;
}
