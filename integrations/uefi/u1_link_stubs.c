/*
 * u1_link_stubs.c — the NON-U-1 link seams the self-contained freestanding
 * archive leaves undefined once the client objects are pulled in.
 *
 * U-1's scope is the ALLOCATOR + the two PLATFORM hooks (kl_monotonic_ms /
 * kl_plat_random) — those are supplied by allocator_uefi.c / platform_uefi.c and
 * are DELIBERATELY NOT stubbed here. Everything else the archive references but
 * U-1 does not yet implement — the socket provider (U-2), the event/completion
 * provider (U-3), sync DNS (U-5), and the vendored-llhttp / PE-runtime residual —
 * is defined here as a minimal FAIL-CLOSED stub so the U-1 self-test LINKS under
 * -nostdlib. None of these run in the U-1 test (it never issues a request); they
 * exist only to close the link. This mirrors tests/freestanding_link_main.c's
 * stub section, minus the platform hooks (which U-1 now really provides).
 */

#include <keel/event_ctx.h>
#include <keel/sockaddr.h>

#include "../../src/socket.h"        /* kl_sockdef_* + KlSocketProvider */
#include "../../src/resolve_sync.h"  /* kl_resolve_sync */
#include "../../src/event_builtin.h" /* kl_event_*_builtin */
#include "../../src/completion.h"    /* KlCompletionOps + kl_comp_ops_builtin */

#include <stddef.h>
#include <stdint.h>

/* ── sync DNS (U-5) — fail-closed ─────────────────────────────────────────── */
int kl_resolve_sync(const char *host, uint16_t port, int socktype,
                    KlSockAddr *out, int max, int *n) {
    (void)host; (void)port; (void)socktype; (void)out; (void)max; (void)n;
    return -1;
}

/* ── socket provider fallbacks (U-2) — never reached; fail-closed ─────────── */
KlSocketHandle kl_sockdef_socket(int d, int t, int p) { (void)d;(void)t;(void)p; return KL_INVALID_SOCKET; }
int  kl_sockdef_connect(KlSocketHandle f, const KlSockAddr *a) { (void)f;(void)a; return -1; }
int  kl_sockdef_close(KlSocketHandle f) { (void)f; return -1; }
int  kl_sockdef_set_nonblocking(KlSocketHandle f) { (void)f; return -1; }
void kl_sockdef_set_nosigpipe(KlSocketHandle f) { (void)f; }
int  kl_sockdef_get_so_error(KlSocketHandle f, int *e) { (void)f; if (e) *e = 0; return -1; }
KlIoStatus kl_sockdef_io_status(void) { return KL_IO_FATAL; }
ssize_t kl_sockdef_send(KlSocketHandle f, const void *b, size_t n) { (void)f;(void)b;(void)n; return -1; }
ssize_t kl_sockdef_recv(KlSocketHandle f, void *b, size_t n) { (void)f;(void)b;(void)n; return -1; }
ssize_t kl_sockdef_recv_peek(KlSocketHandle f, void *b, size_t n) { (void)f;(void)b;(void)n; return -1; }

/* ── event / completion builtins (U-3) — never reached; fail-closed ───────── */
int  kl_event_init_builtin(KlEventLoop *loop) { (void)loop; return -1; }
int  kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;(void)fd;(void)mask;(void)udata; return -1;
}
int  kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;(void)fd;(void)mask;(void)udata; return -1;
}
int  kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) { (void)loop;(void)fd; return -1; }
int  kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    (void)loop;(void)out;(void)max;(void)timeout_ms; return -1;
}
void kl_event_close_builtin(KlEventLoop *loop) { (void)loop; }
unsigned kl_event_caps_builtin(const KlEventLoop *loop) { (void)loop; return 0; }
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop; return NULL;
}
const KlCompletionOps *kl_comp_ops_builtin(void) { return NULL; }

/* ── vendored-llhttp residual + PE compiler-runtime ──────────────────────────
 * abort / fprintf / stderr come from vendor/llhttp/api.c (unreachable switch
 * defaults + the never-called pretty-printer). Inert stubs so -nostdlib
 * resolves; a real UEFI build maps abort -> CpuDeadLoop and drops stdio. */
typedef struct _KL_FS_FILE KL_FS_FILE;
KL_FS_FILE *stderr;
void abort(void) { for (;;) { } }
int fprintf(KL_FS_FILE *stream, const char *fmt, ...) { (void)stream; (void)fmt; return 0; }

/* __chkstk — Windows-ABI stack-probe helper the PE/COFF target emits at the
 * prologue of any function with a stack frame larger than one page (4 KiB).
 * Contract (MS x64): on entry %rax = the frame size in bytes; __chkstk walks the
 * stack DOWNWARD one page at a time, TOUCHING each page so a guard-paged stack
 * faults in / is validated, then returns with %rax unchanged (the caller does
 * `sub %rax, %rsp`). It must preserve every register except %r10/%r11 (and the
 * flags), and must not disturb the return address.
 *
 * A no-op is WRONG on UEFI: OVMF runs on a bounded, guard-paged stack, so a
 * >4 KiB frame that is never probed writes past the touched region and takes a
 * #PF the moment it stores to a high local (observed: the llhttp-generated
 * state machine pulled in transitively has a ~4 KiB frame). This is a real,
 * spec-correct probe (touch one byte per page). Written in asm so the prologue
 * ABI (rax in/out, only r10/r11 clobbered) is exact. */
__attribute__((naked)) void __chkstk(void);
__attribute__((naked)) void __chkstk(void) {
    __asm__ volatile(
        "push %%rcx\n\t"                 /* save the two temporaries we use */
        "push %%rax\n\t"
        "cmp  $0x1000, %%rax\n\t"        /* frame <= one page? nothing to probe */
        "lea  0x18(%%rsp), %%rcx\n\t"    /* rcx = caller %rsp (past our 2 pushes + retaddr) */
        "jb   2f\n\t"
        "1:\n\t"
        "sub  $0x1000, %%rcx\n\t"        /* step down one page */
        "test %%rcx, (%%rcx)\n\t"        /* touch it (fault-in / validate) */
        "sub  $0x1000, %%rax\n\t"
        "cmp  $0x1000, %%rax\n\t"
        "ja   1b\n\t"
        "2:\n\t"
        "sub  %%rax, %%rcx\n\t"          /* touch the final partial page */
        "test %%rcx, (%%rcx)\n\t"
        "pop  %%rax\n\t"                 /* restore rax (frame size, for caller) */
        "pop  %%rcx\n\t"
        "ret\n\t"
        ::: "memory");
}
