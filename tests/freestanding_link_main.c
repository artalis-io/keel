/*
 * freestanding_link_main.c — B2 CRT-less PE/COFF link proof.
 *
 * This is the entry TU for `make freestanding-link`: it LINKS
 * libkeel_freestanding_selfcontained.a (mem-family + strlen in-archive) into PE/COFF
 * image with NO hosted CRT (-nostdlib, lld PE), proving the freestanding archive
 * links standalone. It is the last literal of the acceptance milestone from
 * docs/phase10_uefi_feasibility_design.md:
 *
 *     "links as PE/COFF without a hosted CRT, documented undefined-symbol
 *      whitelist, passes host sanitizer tests, performs an HTTP/1.1 GET over a
 *      mock completion provider."
 *
 * It is a LINK target, not a RUN target — it need not execute; it must resolve
 * with an empty undefined set under -nostdlib. So every symbol the archive
 * leaves undefined (the documented whitelist) is DEFINED here as a minimal,
 * fail-closed stub:
 *
 *   - PLATFORM SEAMS      : kl_monotonic_ms / kl_plat_random / kl_resolve_sync
 *   - FALLBACK SOCKET OPS : kl_sockdef_*            (never called; provider wins)
 *   - EVENT/COMPLETION    : kl_event_*_builtin / kl_comp_ops_builtin
 *   - VENDORED RESIDUAL   : abort / fprintf / stderr (llhttp api.c debug/switch)
 *   - PE COMPILER-RUNTIME : __chkstk (Windows-ABI stack probe, x86_64 + aarch64)
 *
 * A REAL UEFI build supplies these from EDK2 (BaseMemoryLib, a real clock,
 * EFI_TCP4/EFI token provider, CpuDeadLoop for abort, the PE runtime's __chkstk).
 * The point of this TU is only that the ARCHIVE'S link closure is exactly the
 * documented seam — nothing hosted leaks in.
 *
 * FREESTANDING: no libc headers. It pulls Keel's public/internal headers under
 * the same -DKEEL_FREESTANDING + freestanding shim (-isystem tests/freestanding/
 * shim) as the archive objects, so <string.h>/<stddef.h> resolve to the
 * freestanding surface, not a hosted libc.
 */

#include <keel/client.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include "../src/socket.h"        /* kl_sockdef_* + KlSocketProvider */
#include "../src/resolve_sync.h"  /* kl_resolve_sync */
#include "../src/event_builtin.h" /* kl_event_*_builtin */
#include "../src/completion.h"    /* KlCompletionOps + kl_comp_ops_builtin */
#ifdef KEEL_FS_LINK_DGRAM
#include <keel/datagram.h>          /* 6.4a-1 composition probe: pull the datagram archive layer */
#endif
#ifdef KEEL_FS_LINK_DNS
#include <keel/datagram.h>
#include <keel/dns_resolver.h>     /* 6.4a-2 composition probe: pull the DNS layer (which rides the datagram layer) */
#endif

#include <stddef.h>
#include <stdint.h>

/* ── EFI-style entry: pull the client objects, then return. ─────────────────
 * References a handful of public client APIs so the linker pulls client_async.c
 * / client_common.c (and their transitive archive objects) into the image. The
 * body never runs; it only needs to REFERENCE these symbols so they are not
 * dead-stripped before the link closure is checked. */
int efi_main(void *image_handle, void *system_table);

int efi_main(void *image_handle, void *system_table) {
    (void)image_handle; (void)system_table;

    /* Take the address of the client entry points so the archive objects that
     * define them (and everything they reference) participate in the link. */
    void *refs[] = {
        (void *)&kl_client_start,
        (void *)&kl_client_start_s,
        (void *)&kl_client_response,
        (void *)&kl_client_error,
        (void *)&kl_client_last_error,
        (void *)&kl_client_cancel,
        (void *)&kl_client_free,
        (void *)&kl_event_ctx_init,
    };
    /* Defeat dead-strip: derive the return from the refs without calling them. */
    uintptr_t acc = 0;
    for (unsigned i = 0; i < sizeof(refs) / sizeof(refs[0]); i++)
        acc |= (uintptr_t)refs[i];
#ifdef KEEL_FS_LINK_DGRAM
    /* Composition probe (6.4a-1 review): also reference the KlUdp entry points so the datagram archive
     * layer participates in the link — proving the client + datagram freestanding archives compose with
     * NO duplicate or unresolved symbol across their overlapping base objects (allocator / event_ctx /
     * sockaddr / completion_*). */
    void *dgram_refs[] = {
        (void *)&kl_datagram_socket_init, (void *)&kl_datagram_recv_start, (void *)&kl_datagram_send,
    };
    for (unsigned i = 0; i < sizeof(dgram_refs) / sizeof(dgram_refs[0]); i++)
        acc |= (uintptr_t)dgram_refs[i];
#endif
#ifdef KEEL_FS_LINK_DNS
    /* Composition probe (6.4a-2): reference the built-in resolver entry point so dns_resolver.o
     * (and, transitively, the datagram layer it rides) participates in the link — proving the
     * client + DNS freestanding archives compose with NO duplicate or unresolved symbol across
     * their overlapping base objects (allocator / event_ctx / sockaddr / completion_* / kl_cstr). */
    void *dns_refs[] = {
        (void *)&kl_dns_resolver_create, (void *)&kl_datagram_socket_init, (void *)&kl_datagram_send,
    };
    for (unsigned i = 0; i < sizeof(dns_refs) / sizeof(dns_refs[0]); i++)
        acc |= (uintptr_t)dns_refs[i];
#endif
    return acc ? 0 : 1;
}

/* ══════════════════════════════════════════════════════════════════════
 * PLATFORM SEAMS (fail-closed link stubs)
 * ══════════════════════════════════════════════════════════════════════ */
uint64_t kl_monotonic_ms(void) { return 0; }
void kl_plat_random(void *buf, size_t len) { (void)buf; (void)len; }
int kl_resolve_sync(const char *host, uint16_t port, int socktype,
                    KlSockAddr *out, int max, int *n) {
    (void)host; (void)port; (void)socktype; (void)out; (void)max; (void)n;
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════
 * FALLBACK SOCKET DEFAULTS (referenced by src/socket.h inline dispatchers)
 * ══════════════════════════════════════════════════════════════════════ */
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
#if defined(KEEL_FS_LINK_DGRAM) || defined(KEEL_FS_LINK_DNS)
/* Datagram-path socket seams referenced by udp.c (a client-only link never binds / reads the local
 * addr / needs the datagram data-plane, so these live under the composition-probe guard). The DNS
 * layer rides KlUdp, so its probe needs them too. */
void kl_sockdef_set_cloexec(KlSocketHandle f) { (void)f; }
int  kl_sockdef_bind(KlSocketHandle f, const KlSockAddr *a) { (void)f;(void)a; return -1; }
int  kl_sockdef_get_local_addr(KlSocketHandle f, KlSockAddr *a) { (void)f;(void)a; return -1; }
const struct KlDatagramOps *kl_sockdef_dgram(void) { return NULL; }
#endif

/* ══════════════════════════════════════════════════════════════════════
 * EVENT / COMPLETION BUILTIN HOOKS (fail-closed)
 * ══════════════════════════════════════════════════════════════════════ */
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

/* ══════════════════════════════════════════════════════════════════════
 * VENDORED-llhttp RESIDUAL + PE COMPILER-RUNTIME (fail-closed link stubs)
 * ══════════════════════════════════════════════════════════════════════
 * abort / fprintf / stderr come from vendor/llhttp/api.c (unreachable switch
 * defaults + the never-called pretty-printer). A real UEFI build maps abort ->
 * CpuDeadLoop and drops stdio; here they are inert stubs so the -nostdlib link
 * resolves. We keep them minimal (no varargs machinery needed — never called). */
typedef struct _KL_FS_FILE KL_FS_FILE;
KL_FS_FILE *stderr;                      /* llhttp references &stderr */
void abort(void) { for (;;) { } }
int fprintf(KL_FS_FILE *stream, const char *fmt, ...) { (void)stream; (void)fmt; return 0; }

/* __chkstk — the Windows-ABI stack-probe helper the PE target emits for
 * functions with large stack frames (both x86_64 and aarch64). Part of the PE
 * compiler runtime, supplied by the CRT/EDK2 on a real target; a no-op stub is
 * safe here because the image is not executed. Documented as the sole
 * compiler-runtime residual in the freestanding symbol gate. */
void __chkstk(void) { }
