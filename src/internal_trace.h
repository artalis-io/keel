/*
 * internal_trace.h: a fixed-size recording trace for timing-sensitive paths.
 *
 * NOT public API, and deliberately not reachable from a default build. Compile with
 * -DKEEL_INTERNAL_TRACE (for example `make KEEL_EXTRA_CFLAGS=-DKEEL_INTERNAL_TRACE`) to enable it;
 * otherwise KL_TRACE expands to nothing and no record, ring or atexit hook exists.
 *
 * WHY A RING RATHER THAN A PRINT. Diagnosing #281 with an fprintf on the drain path made the failure
 * disappear: 1 run in 10 failed without it and 0 in 10 with it, same binary. Formatting and writing
 * on a hot path changes the race you are trying to observe. Recording a struct and formatting later
 * preserved it, and the trace then showed the cause in one run. Any future work on completion
 * retirement, state transitions, cancellation races, stream backpressure or TLS shutdown has the same
 * hazard, which is why this is a shared facility rather than a one-off.
 *
 * USE. Wrap it per subsystem so the numeric slots get names at the call site:
 *
 *     #define DRAIN_TRACE(c, why) KL_TRACE("drain", why, fd, cl, recvd, budget, left, flags)
 *
 * and document the legend next to that macro. The dump prints site, reason and the six values.
 *
 * SINGLE-THREADED. Keel runs one loop per thread and this takes no lock, so records from several
 * threads can interleave or overwrite. That is acceptable for a debug facility and is the reason it
 * is not public.
 */
#ifndef KEEL_INTERNAL_TRACE_H
#define KEEL_INTERNAL_TRACE_H

#ifdef KEEL_INTERNAL_TRACE

#include <stdio.h>
#include <stdlib.h>

#ifndef KEEL_INTERNAL_TRACE_CAP
#define KEEL_INTERNAL_TRACE_CAP 256
#endif

typedef struct {
    const char *site;
    const char *reason;
    long long   v[6];
} KlTraceRec;

extern KlTraceRec kl_trace_ring[KEEL_INTERNAL_TRACE_CAP];
extern unsigned   kl_trace_n;
void kl_trace_record(const char *site, const char *reason,
                     long long a, long long b, long long c,
                     long long d, long long e, long long f);

#define KL_TRACE(site, reason, a, b, c, d, e, f) \
    kl_trace_record((site), (reason), (long long)(a), (long long)(b), (long long)(c), \
                    (long long)(d), (long long)(e), (long long)(f))

#else   /* the default build: no ring, no hook, no cost */

#define KL_TRACE(site, reason, a, b, c, d, e, f) ((void)0)

#endif  /* KEEL_INTERNAL_TRACE */
#endif  /* KEEL_INTERNAL_TRACE_H */
