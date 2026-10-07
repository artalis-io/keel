#include <keel/http1_chunked.h>
#include <stdint.h>
#include <string.h>

void kl_http1_chunked_init(KlHttp1ChunkedDecoder *dec) {
    dec->state = KL_HTTP1_CHUNK_SIZE;
    dec->chunk_remaining = 0;
    dec->total_body = 0;
    dec->size_accum = 0;
    dec->size_digits = 0;
    dec->trailer_cr = 0;
    dec->meta_len = 0;
}

/* A byte that may appear inside a chunk extension or a trailer line: HTAB, visible ASCII, SP and
 * obs-text. Every other control byte (CR and LF included: they only end a line, and only as a pair)
 * is an error. A decoder that skipped to the next CR let a bare LF end the line for a front end and
 * not for us, which frames the body differently on the two sides (request smuggling). */
static int chunk_field_byte(unsigned char ch) {
    return ch == '\t' || (ch >= 0x20 && ch != 0x7F);
}

/*
 * RFC 9112 section 7.1 chunked transfer coding decoder.
 *
 * State machine with fast-path memcpy for large chunks.
 * Security: max 16 hex digits, overflow check before shift,
 * strict CRLF enforcement, extensions/trailers skipped without buffering but validated (no bare
 * LF, no control bytes) and capped (KL_HTTP1_CHUNK_EXT_MAX, KL_HTTP1_CHUNK_TRAILER_MAX).
 *
 * trailer_cr semantics:
 *   In TRAILER/TRAILER_CR states: 1 = at start of line (no content yet)
 *   In DATA_CR state: 1 = already saw CR, expecting LF
 */
int kl_http1_chunked_decode(KlHttp1ChunkedDecoder *dec, const char *data, size_t len,
                      KlHttpBodyReader *reader) {
    size_t i = 0;

    while (i < len) {
        switch (dec->state) {

        case KL_HTTP1_CHUNK_SIZE: {
            unsigned char ch = (unsigned char)data[i];
            int digit;
            if (ch >= '0' && ch <= '9')      digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else if (ch == ';') {
                if (dec->size_digits == 0) {
                    dec->state = KL_HTTP1_CHUNK_ERROR;
                    return -1;
                }
                dec->state = KL_HTTP1_CHUNK_EXT;
                dec->meta_len = 0;
                i++;
                break;
            } else if (ch == '\r') {
                if (dec->size_digits == 0) {
                    dec->state = KL_HTTP1_CHUNK_ERROR;
                    return -1;
                }
                dec->state = KL_HTTP1_CHUNK_SIZE_CR;
                i++;
                break;
            } else {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }

            /* Overflow guard: max 16 hex digits, check before shift */
            if (dec->size_digits >= 16) {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            if (dec->size_accum > SIZE_MAX / 16) {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            dec->size_accum = (dec->size_accum << 4) | (size_t)digit;
            dec->size_digits++;
            i++;
            break;
        }

        case KL_HTTP1_CHUNK_EXT: {
            /* Skip extension bytes until CR (SIZE_CR then demands the LF), rejecting a bare LF or a
             * control byte and an extension list past its cap. */
            unsigned char ch = (unsigned char)data[i];
            if (ch == '\r') {
                dec->state = KL_HTTP1_CHUNK_SIZE_CR;
            } else if (!chunk_field_byte(ch) || ++dec->meta_len > KL_HTTP1_CHUNK_EXT_MAX) {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            i++;
            break;
        }

        case KL_HTTP1_CHUNK_SIZE_CR:
            if (data[i] != '\n') {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            dec->chunk_remaining = dec->size_accum;
            dec->size_accum = 0;
            dec->size_digits = 0;

            if (dec->chunk_remaining == 0) {
                /* Terminal chunk: enter trailer parsing.
                 * trailer_cr=1 means "at start of line". If the next
                 * bytes are \r\n, it's the empty terminating line. */
                dec->state = KL_HTTP1_CHUNK_TRAILER;
                dec->trailer_cr = 1;
                dec->meta_len = 0;
            } else {
                dec->state = KL_HTTP1_CHUNK_DATA;
            }
            i++;
            break;

        case KL_HTTP1_CHUNK_DATA: {
            /* Fast path: forward as many bytes as possible in one call */
            size_t avail = len - i;
            size_t forward = avail < dec->chunk_remaining
                             ? avail : dec->chunk_remaining;

            if (forward > 0 && reader) {
                if (reader->on_data(reader, data + i, forward) < 0) {
                    dec->state = KL_HTTP1_CHUNK_ERROR;
                    return -1;
                }
            }

            dec->total_body += forward;
            dec->chunk_remaining -= forward;
            i += forward;

            if (dec->chunk_remaining == 0) {
                dec->state = KL_HTTP1_CHUNK_DATA_CR;
                dec->trailer_cr = 0;  /* reset: haven't seen CR yet */
            }
            break;
        }

        case KL_HTTP1_CHUNK_DATA_CR:
            /* trailer_cr used here as: 1 = already saw CR, expecting LF */
            if (dec->trailer_cr) {
                /* Previously saw CR across buffer boundary, now expect LF */
                if (data[i] != '\n') {
                    dec->state = KL_HTTP1_CHUNK_ERROR;
                    return -1;
                }
                dec->trailer_cr = 0;
                dec->state = KL_HTTP1_CHUNK_SIZE;
                i++;
            } else if (data[i] == '\r') {
                i++;
                if (i < len) {
                    if (data[i] != '\n') {
                        dec->state = KL_HTTP1_CHUNK_ERROR;
                        return -1;
                    }
                    i++;
                    dec->state = KL_HTTP1_CHUNK_SIZE;
                } else {
                    /* CR at end of buffer, remember for next call */
                    dec->trailer_cr = 1;
                }
            } else {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            break;

        case KL_HTTP1_CHUNK_TRAILER: {
            /* trailer_cr: 1 = at start of line, 0 = mid-line. Every byte of the section counts
             * toward its cap; a bare LF or a control byte is an error, as in an extension. */
            unsigned char ch = (unsigned char)data[i];
            if (++dec->meta_len > KL_HTTP1_CHUNK_TRAILER_MAX) {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            if (ch == '\r') {
                dec->state = KL_HTTP1_CHUNK_TRAILER_CR;
            } else if (!chunk_field_byte(ch)) {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            } else {
                /* Non-CRLF content on this line */
                dec->trailer_cr = 0;
            }
            i++;
            break;
        }

        case KL_HTTP1_CHUNK_TRAILER_CR:
            if (data[i] != '\n' || ++dec->meta_len > KL_HTTP1_CHUNK_TRAILER_MAX) {
                dec->state = KL_HTTP1_CHUNK_ERROR;
                return -1;
            }
            i++;
            if (dec->trailer_cr) {
                /* Empty line (\r\n at start of line): done */
                dec->state = KL_HTTP1_CHUNK_DONE;
                return 1;
            }
            /* End of a non-empty trailer line; next line starts fresh */
            dec->trailer_cr = 1;
            dec->state = KL_HTTP1_CHUNK_TRAILER;
            break;

        case KL_HTTP1_CHUNK_DONE:
            return 1;

        case KL_HTTP1_CHUNK_ERROR:
            return -1;
        }
    }

    if (dec->state == KL_HTTP1_CHUNK_DONE)
        return 1;

    return 0;  /* need more data */
}
