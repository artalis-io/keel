/* F2 staged C consumer: calls the kl_event_dispatch() inline, whose body references the internal
 * keel__event_dispatch_watcher() link-support symbol, then links against the C-built libkeel.a and
 * runs. A clear tag returns 0 without invoking the helper; the volatile udata keeps the reference from
 * being optimized away. Proves the internal symbol links from a plain C consumer. */
#include <keel/event_ctx.h>
#include <stdio.h>

int main(void) {
    KlEvent ev;
    volatile void *u = 0;    /* volatile: the compiler cannot prove the tag, keeping the keel__ call */
    ev.udata = (void *)u;
    ev.ready = 0;
    int r = kl_event_dispatch((KlEventCtx *)0, &ev);   /* clear tag -> returns 0 */
    printf("c-link dispatch: %d\n", r);
    return r == 0 ? 0 : 1;
}
