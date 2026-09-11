/*
 * internal_trace.c: the recording ring behind KL_TRACE. Compiled into the library only when
 * -DKEEL_INTERNAL_TRACE is set; otherwise this TU is empty. See internal_trace.h.
 */
#include "internal_trace.h"

#ifdef KEEL_INTERNAL_TRACE

KlTraceRec kl_trace_ring[KEEL_INTERNAL_TRACE_CAP];
unsigned   kl_trace_n;

/* Format nothing until the process is ending: the whole point is to keep the traced path free of
 * I/O. Prints oldest first, and says how many records were dropped when the ring wrapped. */
static void kl_trace_dump(void) {
    unsigned total = kl_trace_n;
    unsigned shown = (total < (unsigned)KEEL_INTERNAL_TRACE_CAP)
                       ? total : (unsigned)KEEL_INTERNAL_TRACE_CAP;
    fprintf(stderr, "[TRACE] %u record(s)", total);
    if (total > shown) fprintf(stderr, ", oldest %u dropped", total - shown);
    fprintf(stderr, "\n");
    for (unsigned i = 0; i < shown; i++) {
        const KlTraceRec *r = &kl_trace_ring[(total < (unsigned)KEEL_INTERNAL_TRACE_CAP)
                                               ? i
                                               : (total + i) % (unsigned)KEEL_INTERNAL_TRACE_CAP];
        fprintf(stderr, "[TRACE] %-8s %-20s %lld %lld %lld %lld %lld %lld\n",
                r->site, r->reason, r->v[0], r->v[1], r->v[2], r->v[3], r->v[4], r->v[5]);
    }
}

void kl_trace_record(const char *site, const char *reason,
                     long long a, long long b, long long c,
                     long long d, long long e, long long f) {
    static int hooked;
    if (!hooked) { hooked = 1; atexit(kl_trace_dump); }
    KlTraceRec *r = &kl_trace_ring[kl_trace_n++ % (unsigned)KEEL_INTERNAL_TRACE_CAP];
    r->site = site; r->reason = reason;
    r->v[0] = a; r->v[1] = b; r->v[2] = c; r->v[3] = d; r->v[4] = e; r->v[5] = f;
}

#else
typedef int kl_trace_translation_unit_not_empty;   /* ISO C forbids an empty TU */
#endif
