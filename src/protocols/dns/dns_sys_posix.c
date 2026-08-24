/*
 * dns_sys_posix.c: POSIX config discovery for the built-in DNS resolver.
 *
 * The POSIX sibling of dns_sys_win.c (implements the dns_sys.h seam): system
 * nameservers + search domains come from /etc/resolv.conf, the hosts file is
 * /etc/hosts. Plain stdio text parsing; no sockets, no platform #ifdef.
 */
#include "dns_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy `a` into out (length-checked; avoids snprintf %s format-truncation). */
static int dns_sys_copy(char *out, size_t cap, const char *a) {
    size_t la = strlen(a);
    if (la + 1 > cap)
        return -1;
    memcpy(out, a, la + 1);
    return 0;
}

int kl_dns_sys_nameservers(KlAllocator *alloc, const char *resolv_conf_path,
                           char out[][KL_DNS_SYS_NS_STRMAX], int max) {
    (void)alloc;   /* POSIX reads a file: no transient OS-query buffer */
    const char *path = resolv_conf_path ? resolv_conf_path : "/etc/resolv.conf";
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nameserver", 10) != 0)
            continue;
        const char *p = line + 10;
        if (*p != ' ' && *p != '\t')
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
               p[i] != '\r' && i + 1 < KL_DNS_SYS_NS_STRMAX) {
            out[n][i] = p[i];
            i++;
        }
        out[n][i] = '\0';
        if (i > 0)
            n++;
    }
    fclose(f);
    return n;
}

void kl_dns_sys_resolv_options(KlAllocator *alloc, const char *resolv_conf_path,
                               char search[][KL_DNS_SYS_NAME_MAX], int max_s,
                               int *nsearch, int *ndots) {
    (void)alloc;
    *nsearch = 0;
    *ndots = 1;
    const char *path = resolv_conf_path ? resolv_conf_path : "/etc/resolv.conf";
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *save = NULL;
        const char *kw = strtok_r(line, " \t\r\n", &save);
        if (!kw)
            continue;
        if (strcmp(kw, "search") == 0 || strcmp(kw, "domain") == 0) {
            *nsearch = 0;                        /* last search/domain line wins */
            const char *tok;
            while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL && *nsearch < max_s) {
                if (dns_sys_copy(search[*nsearch], KL_DNS_SYS_NAME_MAX, tok) == 0)
                    (*nsearch)++;
            }
        } else if (strcmp(kw, "options") == 0) {
            const char *tok;
            while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
                if (strncmp(tok, "ndots:", 6) == 0) {
                    char *end;
                    long v = strtol(tok + 6, &end, 10);
                    if (end != tok + 6 && *end == '\0' && v >= 0 && v <= 15)
                        *ndots = (int)v;
                }
            }
        }
    }
    fclose(f);
}

const char *kl_dns_sys_default_hosts_path(void) {
    return "/etc/hosts";
}
