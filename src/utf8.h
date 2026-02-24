#ifndef KEEL_UTF8_INTERNAL_H
#define KEEL_UTF8_INTERNAL_H

/*
 * DFA-based UTF-8 validator (Bjoern Hoehrmann's approach).
 * Used for WebSocket text frame validation (RFC 6455 Section 8.1).
 * Internal to KEEL, not part of the public API.
 *
 * State 0 = valid/accept, state 12 = reject/error.
 * Call kl_utf8_validate incrementally — pass state across calls.
 */

#include <stddef.h>
#include <stdint.h>

#define KL_UTF8_ACCEPT 0
#define KL_UTF8_REJECT 12

/* Character class lookup table */
static const uint8_t kl_utf8_class[] = {
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
     1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
     7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
     8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,
};

/* State transition table */
static const uint8_t kl_utf8_trans[] = {
     0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
    12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
    12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
    12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
    12,36,12,12,12,12,12,12,12,12,12,12,
};

/*
 * Validate UTF-8 incrementally.
 * @param state  Pointer to state (init to KL_UTF8_ACCEPT, updated in-place).
 * @param data   Input bytes.
 * @param len    Input length.
 * @return Current state: KL_UTF8_ACCEPT if valid so far, KL_UTF8_REJECT on error.
 */
static uint32_t kl_utf8_validate(uint32_t *state, const uint8_t *data,
                                  size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint32_t type = kl_utf8_class[data[i]];
        *state = kl_utf8_trans[*state + type];
        if (*state == KL_UTF8_REJECT) return KL_UTF8_REJECT;
    }
    return *state;
}

#endif
