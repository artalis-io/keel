/*
 * dns_sys_win.c — Windows config discovery for the built-in DNS resolver.
 *
 * The Winsock sibling of dns_sys_posix.c (implements the dns_sys.h seam).
 * Windows has no resolv.conf/hosts as the source of truth: the system
 * nameservers and DNS suffixes come from the TCP/IP stack via iphlpapi
 * (GetAdaptersAddresses), and the hosts file lives under System32\drivers\etc.
 * Compiled only on Windows (Makefile DNS_SYS_SRC), so it uses Winsock/iphlpapi
 * directly with no internal #ifdef. Links -liphlpapi.
 */
#include "dns_sys.h"

#include <winsock2.h>
#include <ws2tcpip.h>   /* inet_ntop */
#include <iphlpapi.h>   /* GetAdaptersAddresses, IP_ADAPTER_* */
#include <windows.h>
#include <stdint.h>
#include <string.h>

/* Fetch the adapter list into an allocator-backed buffer (caller frees with the
 * returned size). Skips the unicast/anycast/multicast lists — we only need the
 * per-adapter DNS server list + suffix — so the output stays small. Grows on
 * ERROR_BUFFER_OVERFLOW. Returns NULL on OOM / no adapters / error. */
static IP_ADAPTER_ADDRESSES *dns_win_fetch_adapters(KlAllocator *alloc, ULONG *out_size) {
    ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG size = 16384;   /* usually one-shot; grow if the stack reports more */
    for (int tries = 0; tries < 4; tries++) {
        IP_ADAPTER_ADDRESSES *buf = kl_malloc(alloc, size);
        if (!buf)
            return NULL;
        ULONG got = size;
        ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, buf, &got);
        if (rc == ERROR_SUCCESS) { *out_size = size; return buf; }
        kl_free(alloc, buf, size);
        if (rc == ERROR_BUFFER_OVERFLOW && got > size) { size = got; continue; }
        return NULL;
    }
    return NULL;
}

/* Windows returns deprecated fec0::/10 site-local placeholder addresses as DNS
 * servers on adapters with no real IPv6 resolver — not usable; skip them. */
static int dns_win_is_placeholder_v6(const struct sockaddr_in6 *s6) {
    const uint8_t *b = s6->sin6_addr.s6_addr;
    return b[0] == 0xfe && (b[1] & 0xc0) == 0xc0;
}

/* Format a nameserver sockaddr to a numeric string (no port, no scope), or
 * return 0 if it should be skipped. */
static int dns_win_ns_str(const struct sockaddr *sa, char *out, size_t cap) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        return inet_ntop(AF_INET, (void *)&s4->sin_addr, out, cap) != NULL;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        if (dns_win_is_placeholder_v6(s6))
            return 0;
        return inet_ntop(AF_INET6, (void *)&s6->sin6_addr, out, cap) != NULL;
    }
    return 0;
}

int kl_dns_sys_nameservers(KlAllocator *alloc, const char *resolv_conf_path,
                           char out[][KL_DNS_SYS_NS_STRMAX], int max) {
    (void)resolv_conf_path;   /* Windows discovers via iphlpapi, not a file */
    if (!alloc || max <= 0)
        return 0;
    ULONG size = 0;
    IP_ADAPTER_ADDRESSES *list = dns_win_fetch_adapters(alloc, &size);
    if (!list)
        return 0;

    int n = 0;
    for (const IP_ADAPTER_ADDRESSES *aa = list; aa && n < max; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp)
            continue;
        for (const IP_ADAPTER_DNS_SERVER_ADDRESS *d = aa->FirstDnsServerAddress;
             d && n < max; d = d->Next) {
            char s[KL_DNS_SYS_NS_STRMAX];
            if (!d->Address.lpSockaddr ||
                !dns_win_ns_str(d->Address.lpSockaddr, s, sizeof(s)))
                continue;
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (strcmp(out[i], s) == 0) { dup = 1; break; }
            if (dup)
                continue;
            memcpy(out[n], s, strlen(s) + 1);
            n++;
        }
    }
    kl_free(alloc, list, size);
    return n;
}

void kl_dns_sys_resolv_options(KlAllocator *alloc, const char *resolv_conf_path,
                               char search[][KL_DNS_SYS_NAME_MAX], int max_s,
                               int *nsearch, int *ndots) {
    (void)resolv_conf_path;
    *nsearch = 0;
    *ndots = 1;   /* Windows has no ndots knob; the POSIX default is fine */
    if (!alloc || max_s <= 0)
        return;
    ULONG size = 0;
    IP_ADAPTER_ADDRESSES *list = dns_win_fetch_adapters(alloc, &size);
    if (!list)
        return;

    for (const IP_ADAPTER_ADDRESSES *aa = list; aa && *nsearch < max_s; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp || !aa->DnsSuffix || !aa->DnsSuffix[0])
            continue;
        char s[KL_DNS_SYS_NAME_MAX];
        if (WideCharToMultiByte(CP_UTF8, 0, aa->DnsSuffix, -1,
                                s, sizeof(s), NULL, NULL) == 0)
            continue;
        int dup = 0;
        for (int i = 0; i < *nsearch; i++)
            if (strcmp(search[i], s) == 0) { dup = 1; break; }
        if (dup)
            continue;
        memcpy(search[*nsearch], s, strlen(s) + 1);
        (*nsearch)++;
    }
    kl_free(alloc, list, size);
}

const char *kl_dns_sys_default_hosts_path(void) {
    /* %SystemRoot%\System32\drivers\etc\hosts, built once. Single-threaded
     * event-loop model → a function-local static is safe. */
    static char path[MAX_PATH];
    if (path[0] == '\0') {
        UINT n = GetSystemDirectoryA(path, sizeof(path));  /* ...\System32 */
        if (n == 0 || n >= sizeof(path)) {
            memcpy(path, "C:\\Windows\\System32", sizeof("C:\\Windows\\System32"));
            n = (UINT)strlen(path);
        }
        const char *tail = "\\drivers\\etc\\hosts";
        if (n + strlen(tail) + 1 <= sizeof(path)) {
            memcpy(path + n, tail, strlen(tail) + 1);
        } else {
            /* System dir too long to append the tail — use the full default
             * rather than caching a truncated directory-only path. */
            const char *full = "C:\\Windows\\System32\\drivers\\etc\\hosts";
            memcpy(path, full, strlen(full) + 1);
        }
    }
    return path;
}
