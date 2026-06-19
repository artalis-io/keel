/* test_server_freeze.c — covers kl_server_freeze's new (v2.4.0)
 * KlServer.config mirror seal.  Before v2.4.0 the freeze just sealed
 * router tables; now it also deep-copies the live KlConfig into an
 * mprotect-RO arena so heap-write primitives can't pivot parser/
 * log_fn/access_log function pointers or relax max_body_size/
 * max_header_size DoS caps mid-request. */

#include "utest.h"
#include <keel/keel.h>
#include <keel/parser.h>

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void dummy_log(int level, const char *fmt, va_list ap,
                       void *user_data) {
    (void)level; (void)fmt; (void)ap; (void)user_data;
}

static KlParser *dummy_parser(KlAllocator *alloc) {
    return kl_parser_llhttp(alloc);
}

/* Post-freeze the live config pointer changes from &s->config to a
 * sealed arena-resident copy.  The accessor MUST surface that
 * change. */
UTEST(server_freeze, accessor_swaps_to_sealed_copy) {
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;
    cfg.bind_addr = "127.0.0.1";
    cfg.max_body_size = 4096;
    cfg.max_header_size = 1024;
    cfg.log_fn = dummy_log;
    cfg.parser = dummy_parser;

    ASSERT_EQ(kl_server_init(&s, &cfg), 0);

    /* Pre-freeze: accessor reads the mutable original. */
    ASSERT_TRUE(kl_server_config(&s) == &s.config);
    ASSERT_EQ(kl_server_is_frozen(&s), 0);

    ASSERT_EQ(kl_server_freeze(&s), 0);

    /* Post-freeze: accessor reads the sealed snapshot. */
    ASSERT_TRUE(kl_server_config(&s) != &s.config);
    ASSERT_TRUE(s.config_frozen != NULL);
    ASSERT_EQ(kl_server_is_frozen(&s), 1);

    /* Values survived the deep copy. */
    const KlConfig *cf = kl_server_config(&s);
    ASSERT_EQ(cf->max_connections, 2);
    ASSERT_EQ((int)cf->max_body_size, 4096);
    ASSERT_EQ((int)cf->max_header_size, 1024);
    ASSERT_TRUE(cf->log_fn == dummy_log);
    ASSERT_TRUE(cf->parser == dummy_parser);
    ASSERT_TRUE(cf->bind_addr != NULL);
    ASSERT_EQ(strcmp(cf->bind_addr, "127.0.0.1"), 0);
    /* The bind_addr string itself was duplicated into the arena, NOT
     * the original literal pointer kept around. */
    ASSERT_TRUE(cf->bind_addr != cfg.bind_addr);

    kl_server_free(&s);
}

/* Calling freeze twice is a no-op the second time (idempotent
 * defensively).  Both calls return 0. */
UTEST(server_freeze, double_freeze_idempotent) {
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;

    ASSERT_EQ(kl_server_init(&s, &cfg), 0);
    ASSERT_EQ(kl_server_freeze(&s), 0);
    const KlConfig *first = kl_server_config(&s);
    ASSERT_EQ(kl_server_freeze(&s), 0);
    /* Same snapshot — we did NOT allocate a second arena. */
    ASSERT_TRUE(kl_server_config(&s) == first);
    kl_server_free(&s);
}

/* The big one.  An attacker with a heap-write primitive that lands
 * inside KlConfig (the most security-relevant target — parser,
 * log_fn, access_log, max_body_size, max_header_size all live there)
 * MUST get SIGSEGV instead of a silent control-flow hijack.
 *
 * fork+death-test pattern — child resets SIGSEGV/SIGBUS to default so
 * sanitizer runtimes don't swallow the fault. */
UTEST(server_freeze, frozen_config_write_faults) {
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;
    cfg.log_fn = dummy_log;
    cfg.parser = dummy_parser;

    ASSERT_EQ(kl_server_init(&s, &cfg), 0);
    ASSERT_EQ(kl_server_freeze(&s), 0);

    /* The sealed snapshot is what kl_server_config returns; that's
     * what hot paths read; that's the byte range we want sealed. */
    const KlConfig *snap = kl_server_config(&s);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        /* Pivot the parser factory to a bogus address — the kind of
         * single-byte / single-word write a heap UAF could land. */
        ((volatile KlConfig *)snap)->parser =
            (KlParserFactory)(uintptr_t)0xdeadbeef;
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    kl_server_free(&s);
}

/* Same as above but targeting the DoS cap (max_body_size).  An
 * attacker hoping to bypass body-size limits by overwriting the
 * cap to SIZE_MAX must hit the same RO page. */
UTEST(server_freeze, frozen_max_body_size_write_faults) {
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;
    cfg.max_body_size = 1024;

    ASSERT_EQ(kl_server_init(&s, &cfg), 0);
    ASSERT_EQ(kl_server_freeze(&s), 0);

    const KlConfig *snap = kl_server_config(&s);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        ((volatile KlConfig *)snap)->max_body_size = (size_t)-1;
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    kl_server_free(&s);
}

/* The duplicated bind_addr string ALSO lives in the sealed arena —
 * a write into it must fault.  This protects against the
 * "swap listen address to an attacker-controlled host" gadget if
 * kl_server_run is ever called twice for the same KlServer. */
UTEST(server_freeze, frozen_bind_addr_write_faults) {
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;
    cfg.bind_addr = "127.0.0.1";

    ASSERT_EQ(kl_server_init(&s, &cfg), 0);
    ASSERT_EQ(kl_server_freeze(&s), 0);

    const KlConfig *snap = kl_server_config(&s);
    /* Sanity — accessor really did re-point at an arena copy. */
    ASSERT_TRUE(snap->bind_addr != NULL);
    ASSERT_TRUE(snap->bind_addr != cfg.bind_addr);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        /* Cast away const + volatile — we are deliberately committing
         * the UB the seal is designed to catch. */
        ((volatile char *)snap->bind_addr)[0] = 'X';
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    kl_server_free(&s);
}

UTEST_MAIN();
