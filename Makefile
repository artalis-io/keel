CC      = cc
AR      = ar
UNAME_S := $(shell uname -s)
LDFLAGS =

# Windows detection: explicit `make OS=windows` (e.g. MinGW cross-compile from
# Linux: make CC=x86_64-w64-mingw32-gcc OS=windows) or a native MSYS2/MinGW shell
# where uname reports MINGW*/MSYS*. Selects the WSAPoll event backend + Winsock.
ifeq ($(OS),windows)
  WINDOWS := 1
endif
ifneq ($(findstring MINGW,$(UNAME_S)),)
  WINDOWS := 1
endif
ifneq ($(findstring MSYS,$(UNAME_S)),)
  WINDOWS := 1
endif

# Detect Cosmopolitan toolchain (cosmocc, x86_64-unknown-cosmo-cc, etc.)
ifneq ($(findstring cosmo,$(CC)),)
  COSMO := 1
endif
# Fat cosmo compiler creates dual-arch objects (.aarch64/ counterparts)
ifeq ($(CC),cosmocc)
  COSMO_FAT := 1
endif
ifdef COSMO
  # Cosmopolitan: force poll backend, omit -D_DEFAULT_SOURCE and -fstack-protector-strong
  # Note: plain ar is used instead of cosmoar (cosmoar fails with recursive .aarch64/ lookups)
  # APE binaries have their own non-relocatable layout — no PIE / RELRO / FORTIFY here.
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -Iinclude -Ivendor/llhttp
  VENDOR_CFLAGS = -std=c11 -O2 -Iinclude -Ivendor/llhttp
  EVENT_SRC = src/event_poll.c
  FILE_IO_SRC = src/file_io.c
else ifdef WINDOWS
  # Windows (MinGW-w64): WSAPoll event backend + Winsock socket provider, its own
  # TUs (event_wsapoll.c / socket_winsock.c / platform_win.c — no #ifdef in the
  # POSIX TUs). PE has no ELF -z RELRO / _FORTIFY_SOURCE=3; keep CFLAGS simple.
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -fstack-protector-strong -Iinclude -Ivendor/llhttp
  VENDOR_CFLAGS = -std=c11 -O2 -Iinclude -Ivendor/llhttp
  # Event backend: WSAPoll (readiness, default) or IOCP (completion, BACKEND=iocp).
  # The IOCP TU is the only place the completion model lives — no #ifdef leaks into
  # the shared/POSIX TUs. socket_winsock.c stays linked either way (kl_sockdef_*
  # defaults + the Winsock load constructor the IOCP provider reuses).
  ifeq ($(BACKEND),iocp)
    EVENT_SRC = src/event_iocp.c
    # Completion axis: event_iocp.c implements the completion backend (completion.h
    # primitives + kl_comp_ops_builtin). The generic driver + dispatch (COMPLETION_CORE)
    # are always linked; this flag suppresses the readiness kl_comp_ops_builtin stub.
    COMPLETION_BACKEND = 1
  else
    EVENT_SRC = src/event_wsapoll.c
  endif
  SOCKET_SRC = src/socket_winsock.c
  PLATFORM_SRC = src/platform_win.c
  PLATFORM_WAKEUP_SRC = src/platform_wakeup_win.c
  SERVER_PLAT_SRC = src/server_plat_win.c
  DGRAM_SRC = src/socket_dgram_win.c   # Winsock datagram ops (KlSocketProvider.dgram)
  UDP_CMSG_SRC = src/udp_cmsg_win.c    # shared WSARecvMsg fetch + pktinfo parse (IOCP + dgram)
  DNS_SYS_SRC = src/dns_sys_win.c
  FILE_IO_SRC = src/file_io.c
  TEST_COMPAT_SRC = tests/net_compat_win.c
  LDFLAGS += -lws2_32 -lmswsock -lbcrypt -liphlpapi
  EXE = .exe
else
  # Build hardening (parity with Hull's W^X posture in docs/security.md):
  #   -fPIE / -pie           — ASLR for executables linking libkeel.a
  #   -fstack-protector-strong — stack canaries on functions with buffers
  #   -D_FORTIFY_SOURCE=3    — runtime checks on str/mem calls (glibc 2.34+ / gcc 12+ / clang 9+).
  #                            Older toolchains emit a noisy warning and behave as =2; leave loud.
  # Some toolchain spec files (Alpine/musl, hardened Debian, etc.) pre-set
  # _FORTIFY_SOURCE at the command line. Undefine first so our value wins
  # without provoking a "macro redefined" warning that -Werror would
  # promote to a hard error.
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 \
            -fstack-protector-strong -fPIE \
            -Iinclude -Ivendor/llhttp
  VENDOR_CFLAGS = -std=c11 -O2 -fPIE -Iinclude -Ivendor/llhttp
  # Use -Wl,-pie so clang routes the flag to the linker without flagging
  # it as "unused during compilation" — Keel's one-shot compile+link
  # rules (tests/, examples/) combined with -Werror would otherwise
  # promote that warning to a hard error.
  LDFLAGS += -Wl,-pie

  # Platform event loop backend
  ifeq ($(UNAME_S),Linux)
    ifeq ($(BACKEND),iouring)
      # io_uring is completion-native (PAL 8f): BACKEND=iouring builds the completion backend
      # (event_iouring.c driving completion.h via SQE/CQE + completion_driver.c). The old
      # readiness-adapted io_uring TU + file_io_iouring.c were retired (8f-5d) — benchmarks put
      # the readiness POLL_ADD adapter ~2–2.3× slower than completion on both x86 and ARM
      # (docs/phase8f5_iouring_default_migration_design.md). File responses ride zero-copy
      # splice (8f-2), so the io_uring file backend is gone; FILE_IO_SRC is the POSIX file_io.c.
      EVENT_SRC = src/event_iouring.c
      FILE_IO_SRC = src/file_io.c
      COMPLETION_BACKEND = 1
      LDFLAGS += -luring
    else ifeq ($(BACKEND),poll)
      EVENT_SRC = src/event_poll.c
      FILE_IO_SRC = src/file_io.c
    else ifeq ($(BACKEND),pollcomp)
      # Portable completion backend over poll() (PAL 8d-0.5): event_pollcomp.c implements
      # the completion backend on POSIX, so the completion model is runtime-testable off
      # Windows. The generic driver + dispatch (COMPLETION_CORE) are always linked; this
      # flag suppresses the readiness kl_comp_ops_builtin stub.
      # event_pollcomp.c is now a PURE runtime provider (no kl_event_*_builtin /
      # kl_comp_ops_builtin — so it can be injected into a default libkeel, RC-2); the
      # compiled-in-default glue (kl_event_*_builtin + kl_comp_ops_builtin, forwarding to
      # the provider ops) lives in event_pollcomp_builtin.c, linked ONLY for this BACKEND.
      EVENT_SRC = src/event_pollcomp.c src/event_pollcomp_builtin.c
      FILE_IO_SRC = src/file_io.c
      COMPLETION_BACKEND = 1
    else
      EVENT_SRC = src/event_epoll.c
      FILE_IO_SRC = src/file_io.c
    endif
    CFLAGS += -D_DEFAULT_SOURCE
    VENDOR_CFLAGS += -D_DEFAULT_SOURCE
    FUZZ_PLATFORM_CFLAGS += -D_DEFAULT_SOURCE
    # Linux linker hardening — RELRO + BIND_NOW + non-executable stack.
    # ld64 (macOS) rejects -z flags, so gate to Linux.
    LDFLAGS += -lpthread -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
  else
    ifeq ($(BACKEND),poll)
      EVENT_SRC = src/event_poll.c
    else ifeq ($(BACKEND),pollcomp)
      # Portable completion backend over poll() (PAL 8d-0.5) — see the Linux branch.
      # event_pollcomp.c = pure runtime provider; event_pollcomp_builtin.c = the compiled-in
      # glue (kl_event_*_builtin + kl_comp_ops_builtin). See the Linux pollcomp block (RC-2).
      EVENT_SRC = src/event_pollcomp.c src/event_pollcomp_builtin.c
      COMPLETION_BACKEND = 1
    else
      EVENT_SRC = src/event_kqueue.c
    endif
    FILE_IO_SRC = src/file_io.c
  endif
endif

# Header dependency tracking
CFLAGS += -MMD -MP
VENDOR_CFLAGS += -MMD -MP

# Core library — parser-agnostic
# Socket provider + platform services: POSIX defaults; the Windows branch sets
# the socket_winsock.c / platform_win.c siblings.
SOCKET_SRC ?= src/socket_posix.c
PLATFORM_SRC ?= src/platform_posix.c
PLATFORM_WAKEUP_SRC ?= src/platform_wakeup_posix.c
SERVER_PLAT_SRC ?= src/server_plat_posix.c
# The POSIX datagram data-plane (KlDatagramOps) for the POSIX socket provider, and
# the shared cmsg parsers the POSIX completion backends (io_uring/pollcomp) reuse.
DGRAM_SRC ?= src/socket_dgram_posix.c
UDP_CMSG_SRC ?= src/udp_cmsg.c
# Test-harness network helpers: per-platform sibling TU (mirrors SOCKET_SRC), so
# tests/net_compat.h stays logic-#ifdef-free. Linked into each test binary.
TEST_COMPAT_SRC ?= tests/net_compat_posix.c
# DNS config discovery (nameservers/hosts/search): POSIX resolv.conf/hosts; the
# Windows branch swaps the iphlpapi sibling. dns_resolver.c itself is #ifdef-free
# and runs over the udp + socket.h seams.
DNS_SYS_SRC ?= src/dns_sys_posix.c
# Completion axis core (RC-1). The generic driver (completion_driver.c: kl_comp_run /
# kl_io_engine_*) + the runtime-dispatch surface (completion_dispatch.c: the kl_comp_*
# primitives, routed to the compiled-in backend or a runtime provider) are linked on
# EVERY build. On a readiness build they are never called (gated by KL_EVENT_CAP_
# COMPLETION); on a completion build the dispatch reaches the backend's kl_comp_ops_builtin.
# KEEL_NO_COMPLETION swaps in aborting stubs (completion_absent.c) — the axis is compiled out.
ifdef KEEL_NO_COMPLETION
  COMPLETION_CORE = src/completion_absent.c
else
  COMPLETION_CORE = src/completion_driver.c src/completion_dispatch.c
  # A readiness EVENT_SRC (epoll/kqueue/poll/wsapoll) has no completion backend, so it
  # needs the kl_comp_ops_builtin→NULL stub the dispatch falls back to (never dereferenced).
  # A completion backend (COMPLETION_BACKEND=1) provides its own kl_comp_ops_builtin.
  ifndef COMPLETION_BACKEND
    COMPLETION_CORE += src/completion_readiness_stub.c
  endif
endif
CORE_SRC = src/allocator.c src/allocator_default_stdlib.c src/error.c src/version.c src/sockaddr.c $(SOCKET_SRC) $(PLATFORM_SRC) $(PLATFORM_WAKEUP_SRC) src/response.c src/router.c \
           src/connection.c src/server.c $(SERVER_PLAT_SRC) src/async.c src/timer.c \
           src/body_reader_buffer.c \
           src/body_reader_multipart.c src/chunked.c src/cors.c \
           src/websocket.c src/websocket_client.c \
           src/h2.c src/h2_client.c src/thread_pool.c src/url.c \
           src/client.c src/client_pool.c src/redirect.c src/sse.c \
           src/resolver_cache.c src/proxy_protocol.c src/udp.c $(DGRAM_SRC) $(UDP_CMSG_SRC) src/udp_server.c \
           src/dns_resolver.c $(DNS_SYS_SRC) src/resolve_sync.c \
           src/compress.c src/decompress.c src/drain.c \
           $(COMPLETION_CORE) $(FILE_IO_SRC) src/event_dispatch.c $(EVENT_SRC)

# The built-in DNS resolver now builds on every platform: dns_resolver.c is
# #ifdef-free (over the udp + socket.h seams) and DNS_SYS_SRC swaps the config-
# discovery TU (dns_sys_posix.c / dns_sys_win.c iphlpapi), so no filtering is
# needed here.

# Default parser backend (llhttp)
LLHTTP_SRC = parsers/parser_llhttp.c parsers/response_parser_llhttp.c \
             vendor/llhttp/llhttp.c vendor/llhttp/api.c vendor/llhttp/http.c

# Optional mbedTLS backend (bring-your-own — mbedTLS is not vendored). The adapter
# now lives in integrations/mbedtls/ (out of the dependency-light core); it still
# builds on POSIX and Windows (its BIO I/O goes through the socket seam). Point
# MBEDTLS_DIR at a source tree (include/ + library/) OR a system prefix (include/ +
# lib/, e.g. `MBEDTLS_DIR=$(brew --prefix mbedtls)`); if unset, the compiler's
# default search paths are used (e.g. MSYS2 /mingw64).
#   make KEEL_TLS=mbedtls [MBEDTLS_DIR=/path]
# `make integration-mbedtls` builds the adapter standalone (integrations/mbedtls/).
ifdef KEEL_TLS
ifeq ($(KEEL_TLS),mbedtls)
  # -Isrc: the adapter includes the internal socket seam ("socket.h").
  # -Iintegrations/mbedtls: its public header <keel_tls_mbedtls.h>.
  CFLAGS  += -Isrc -Iintegrations/mbedtls
  ifdef MBEDTLS_DIR
    CFLAGS  += -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library
    LDFLAGS += -L$(MBEDTLS_DIR)/library -L$(MBEDTLS_DIR)/lib
  endif
  ifdef MBEDTLS_CONFIG_FILE
    CFLAGS += -I$(MBEDTLS_DIR) -DMBEDTLS_CONFIG_FILE='"$(MBEDTLS_CONFIG_FILE)"'
  endif
  LDFLAGS += -lmbedtls -lmbedx509 -lmbedcrypto   # after -lkeel in the link line
  TLS_MBEDTLS_SRC = integrations/mbedtls/tls_mbedtls.c
  TLS_MBEDTLS_OBJ = integrations/mbedtls/tls_mbedtls.o
endif
endif
TLS_MBEDTLS_OBJ ?=

# Optional miniz compression backend: make KEEL_COMPRESS=miniz MINIZ_DIR=/path/to/miniz
ifdef KEEL_COMPRESS
ifeq ($(KEEL_COMPRESS),miniz)
  MINIZ_DIR ?= ../miniz
  MINIZ_CFLAGS = -I$(MINIZ_DIR) -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO
  CFLAGS += $(MINIZ_CFLAGS)
  COMPRESS_MINIZ_SRC = src/compress_miniz.c src/decompress_miniz.c
  COMPRESS_MINIZ_OBJ = src/compress_miniz.o src/decompress_miniz.o
endif
endif
COMPRESS_MINIZ_OBJ ?=

CORE_OBJ = $(CORE_SRC:.c=.o)
LLHTTP_OBJ = $(LLHTTP_SRC:.c=.o)
LIB = libkeel.a

all: $(LIB)

# Include generated dependency files (after default target)
-include $(CORE_OBJ:.o=.d) $(LLHTTP_OBJ:.o=.d)

$(LIB): $(CORE_OBJ) $(LLHTTP_OBJ) $(TLS_MBEDTLS_OBJ) $(COMPRESS_MINIZ_OBJ)
ifdef COSMO_FAT
	@# Fat cosmocc: use single-arch cosmo ar (not cosmoar which fails with .aarch64/ recursion,
	@# and not macOS ar which creates BSD archives that GNU ld.bfd can't resolve symbols from)
	x86_64-unknown-cosmo-ar rcs $@ $^
	@mkdir -p .aarch64
	@aarch64-unknown-cosmo-ar rcs .aarch64/$@ $(foreach o,$^,$(dir $(o)).aarch64/$(notdir $(o)))
else ifdef COSMO
	@# Single-arch cosmo: use the AR passed by the caller (e.g. x86_64-unknown-cosmo-ar)
	$(AR) rcs $@ $^
else
	$(AR) rcs $@ $^
endif

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The lwIP-raw completion backend (integrations/lwip/event_lwip_raw.o +
# lwip_raw_glue.o) is a RUNTIME PROVIDER built next to a STOCK libkeel, NOT compiled into
# the core lib (BACKEND=lwipraw was retired in RC-3). Its object build rules live in
# integrations/lwip/Makefile (loopback-raw), which supplies the BYO lwIP include dirs.

# Vendor code — relaxed warnings
vendor/llhttp/llhttp.o: vendor/llhttp/llhttp.c
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<
vendor/llhttp/api.o: vendor/llhttp/api.c
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<
vendor/llhttp/http.o: vendor/llhttp/http.c
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<

# Examples
EXAMPLES = examples/hello_server examples/rest_api_server examples/middleware \
           examples/static_files examples/streaming examples/body_readers \
           examples/websocket_server examples/websocket_client \
           examples/async examples/thread_pool \
           examples/h2_server examples/h2_client \
           examples/client examples/async_client examples/async_thread_pool \
           examples/custom_allocator examples/custom_socket_provider \
           examples/connection_pool examples/url_parser \
           examples/sse examples/streaming_client examples/timer \
           examples/redirect_client examples/proxy_client \
           examples/unix_socket_server

examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

# TLS example — only built when KEEL_TLS=mbedtls
ifeq ($(KEEL_TLS),mbedtls)
EXAMPLES += examples/tls_server examples/tls_client
endif

# Compression example — only built when KEEL_COMPRESS=miniz
ifeq ($(KEEL_COMPRESS),miniz)
EXAMPLES += examples/compress_server examples/decompress_client
endif

examples: $(EXAMPLES)

# Tests — relax pedantic warnings triggered by utest.h vendor macros
# test_iocp_engine.c requires the IOCP backend (asserts COMPLETION caps +
# kl_socket_provider_iocp) — exclude by default, add back only for that backend.
TEST_SRC = $(filter-out tests/test_iocp_engine.c, $(wildcard tests/test_*.c))
ifeq ($(BACKEND),iocp)
  TEST_SRC += tests/test_iocp_engine.c
endif
TEST_BIN = $(TEST_SRC:.c=)

# Per-platform test-network helpers (net_compat_posix.c / net_compat_win.c),
# linked into every test binary. Built by the generic %.o rule.
TEST_COMPAT_OBJ = $(TEST_COMPAT_SRC:.c=.o)
# Keep the compat .o from being auto-deleted as a pattern-rule intermediate
# (it's a prerequisite of the tests/% pattern rule, so Make would otherwise
# rebuild it on every invocation).
.SECONDARY: $(TEST_COMPAT_OBJ)

tests/%: tests/%.c $(LIB) $(TEST_COMPAT_OBJ)
	$(CC) $(CFLAGS) -Wno-pedantic -Wno-sign-compare -Wno-unused-result -Ivendor -o $@ $< $(TEST_COMPAT_OBJ) -L. -lkeel $(LDFLAGS)

test: $(TEST_BIN)
	@failed=0; \
	for t in $(TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME TESTS FAILED"; exit 1; fi

# Windows unit-test subset (see docs/phase6_winsock_design.md Part C). 47 of the
# 55 suites run on the Windows runner. Tier 1: platform-neutral logic. Tier 2:
# socket/thread runtime (WSAPoll/Winsock/winpthreads). Tier 3: suites whose POSIX
# network idioms (<sys/socket.h> etc., socketpair/pipe/close/read/write/fcntl/poll)
# are routed through tests/net_compat.h — including the mock-TLS suites (tls,
# tls_integration, peer_cert), which exercise the TLS server/client integration
# against an in-test mock KlTls and need no mbedTLS.
#
# 8 not listed. 6 are genuinely POSIX/Linux-only: udp_batching (recvmmsg),
# udp_offload (UDP GSO), udp_tos (Windows restricts IP_TOS/DSCP setsockopt),
# unix_socket (SO_PEERCRED), file_io (POSIX file-path
# assumptions). 2 build clean but have runtime failures needing Windows-native
# iteration, deferred for now: dns_resolver (mock-UDP-nameserver + hosts/resolv.conf
# harness) and proxy (CONNECT tunnel timing) — both still covered on Windows by
# smoke-dns and the POSIX suites. (The real mbedTLS backend is validated separately
# by `make KEEL_TLS=mbedtls smoke-tls`; mbedTLS is BYO and stays out of CI.)
WIN_TEST_SUITES = allocator body_reader chunked cors decompress drain \
                  multipart_stream overflow parser response_parser router url \
                  client client_stream connection h2_client redirect \
                  server_stats thread_pool timer websocket_client \
                  error proxy_protocol resolver_cache request timeout \
                  integration server_integration peer_addr client_happy_eyeballs \
                  async client_pool cross_module event_ctx event_caps \
                  h2 response socket_provider websocket compress event sse \
                  udp udp_server udp_multicast \
                  tls tls_integration peer_cert
WIN_TEST_BIN = $(addprefix tests/test_,$(addsuffix $(EXE),$(WIN_TEST_SUITES)))

# On Windows the test binaries need the `.exe` suffix and the win_prelude.h
# force-include (utest.h QueryPerformanceCounter clash). POSIX builds fall through
# to the extension-less `tests/%` rule above, so `test-win` also runs natively as
# a subset sanity check.
ifeq ($(WINDOWS),1)
tests/test_%$(EXE): tests/test_%.c $(LIB) $(TEST_COMPAT_OBJ)
	$(CC) $(CFLAGS) -include tests/win_prelude.h -Wno-pedantic -Wno-sign-compare -Wno-unused-result -Ivendor -o $@ $< $(TEST_COMPAT_OBJ) -L. -lkeel $(LDFLAGS)
endif

test-win: $(WIN_TEST_BIN)
	@failed=0; \
	for t in $(WIN_TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME WINDOWS TESTS FAILED"; exit 1; fi

# Windows IOCP backend (BACKEND=iocp) test subset. Increment 2: the completion
# connection driver does not exist yet, so a server cannot run over IOCP — this
# runs only the IOCP backend-lifecycle + negotiation suite (no server-over-IOCP).
# The full suite over IOCP joins once the driver lands. Build with BACKEND=iocp so
# libkeel.a carries event_iocp.o. (test_event_caps is a *readiness*-backend suite —
# it asserts READINESS caps — so it runs in the WSAPoll/POSIX jobs, NOT here.)
WIN_IOCP_TEST_SUITES = iocp_engine
WIN_IOCP_TEST_BIN = $(addprefix tests/test_,$(addsuffix $(EXE),$(WIN_IOCP_TEST_SUITES)))
test-win-iocp: $(WIN_IOCP_TEST_BIN)
	@failed=0; \
	for t in $(WIN_IOCP_TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME WINDOWS IOCP TESTS FAILED"; exit 1; fi

# Plaintext TCP link + roundtrip smoke test — the cross-platform link gate
# (the Windows CI runs this to prove the TCP core links and serves). Standalone
# (not a utest suite), so it needs -lpthread explicitly (Windows LDFLAGS omits it).
SMOKE_BIN = tests/smoke_tcp$(EXE)
smoke-tcp: $(SMOKE_BIN)
	./$(SMOKE_BIN)
$(SMOKE_BIN): tests/smoke_tcp.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# End-to-end HTTP-over-IOCP roundtrip (Windows, BACKEND=iocp). The runtime gate for
# the completion connection driver — build libkeel with BACKEND=iocp first so the
# server runs on the IOCP completion loop.
SMOKE_IOCP_BIN = tests/smoke_iocp$(EXE)
smoke-iocp: $(SMOKE_IOCP_BIN)
	./$(SMOKE_IOCP_BIN)
$(SMOKE_IOCP_BIN): tests/smoke_iocp.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# TLS-over-IOCP roundtrip (Windows, BACKEND=iocp) via the identity mock TLS — runtime
# gate for event_iocp.c's IOCP-specific TLS mechanics (KL_IOCP_TLS_RECV, feed-in-drain,
# overlapped ciphertext WSASend). No mbedTLS needed.
SMOKE_IOCP_TLS_BIN = tests/smoke_iocp_tls$(EXE)
smoke-iocp-tls: $(SMOKE_IOCP_TLS_BIN)
	./$(SMOKE_IOCP_TLS_BIN)
$(SMOKE_IOCP_TLS_BIN): tests/smoke_iocp_tls.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# Async/thread-pool over IOCP (Windows, BACKEND=iocp) — runtime gate for the IOCP watcher
# relay (overlapped WSARecv on the loopback wakeup surfaces KL_COMP_WATCHER). 8e-2c.
SMOKE_IOCP_ASYNC_BIN = tests/smoke_iocp_async$(EXE)
smoke-iocp-async: $(SMOKE_IOCP_ASYNC_BIN)
	./$(SMOKE_IOCP_ASYNC_BIN)
$(SMOKE_IOCP_ASYNC_BIN): tests/smoke_iocp_async.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# End-to-end HTTP-over-completion roundtrip on POSIX (BACKEND=pollcomp). The runtime
# gate for the platform-independent completion driver off Windows — build libkeel with
# BACKEND=pollcomp first so the server runs on the poll() completion loop. Proves the
# completion axis is portable (driver reused verbatim) and makes it CI-testable.
SMOKE_POLLCOMP_BIN = tests/smoke_pollcomp$(EXE)
smoke-pollcomp: $(SMOKE_POLLCOMP_BIN)
	./$(SMOKE_POLLCOMP_BIN)
$(SMOKE_POLLCOMP_BIN): tests/smoke_pollcomp.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# TLS-over-completion roundtrip on POSIX via the identity mock TLS (no mbedTLS). Runs the
# completion driver's TLS paths (comp_tls_drive / send_response / file / stream) for real.
SMOKE_POLLCOMP_TLS_BIN = tests/smoke_pollcomp_tls$(EXE)
smoke-pollcomp-tls: $(SMOKE_POLLCOMP_TLS_BIN)
	./$(SMOKE_POLLCOMP_TLS_BIN)
$(SMOKE_POLLCOMP_TLS_BIN): tests/smoke_pollcomp_tls.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# WebSocket-over-completion roundtrip on POSIX via pollcomp — runs comp_ws_drive (8e-1).
SMOKE_POLLCOMP_WS_BIN = tests/smoke_pollcomp_ws$(EXE)
smoke-pollcomp-ws: $(SMOKE_POLLCOMP_WS_BIN)
	./$(SMOKE_POLLCOMP_WS_BIN)
$(SMOKE_POLLCOMP_WS_BIN): tests/smoke_pollcomp_ws.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# Async/thread-pool handler over completion via pollcomp — runs the watcher relay + async
# resume (8e-2b): a thread-pool wakeup watcher fires on the completion loop and resumes.
SMOKE_POLLCOMP_ASYNC_BIN = tests/smoke_pollcomp_async$(EXE)
smoke-pollcomp-async: $(SMOKE_POLLCOMP_ASYNC_BIN)
	./$(SMOKE_POLLCOMP_ASYNC_BIN)
$(SMOKE_POLLCOMP_ASYNC_BIN): tests/smoke_pollcomp_async.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# LC-0 PROOF: async KlClient connect+GET over the pollcomp completion loop — the completion
# CONNECT contract (KL_COMP_CONNECT / kl_comp_post_connect) driving the client's connect over
# the completion axis instead of the readiness WRITE-watcher shim. Build with BACKEND=pollcomp
# first so both the server and the client's KlEventCtx run on the poll() completion loop.
SMOKE_POLLCOMP_CLIENT_BIN = tests/smoke_pollcomp_client$(EXE)
# The async KlClient connect+GET over a pollcomp COMPLETION loop — the LC-0 proof. The smoke
# wires kl_socket_provider_pollcomp() + the compiled-in ctx, so it is built BACKEND=pollcomp
# (the completion backend compiled in), mirroring smoke-pollcomp-asan.
smoke-pollcomp-client:
	$(MAKE) clean
	$(MAKE) BACKEND=pollcomp $(LIB)
	$(CC) $(CFLAGS) -Isrc -o $(SMOKE_POLLCOMP_CLIENT_BIN) tests/smoke_pollcomp_client.c \
	      -L. -lkeel -lpthread $(LDFLAGS)
	./$(SMOKE_POLLCOMP_CLIENT_BIN)

# Completion roundtrips (plaintext, TLS-via-mock, and WebSocket) under ASan+UBSan with
# leak detection (Linux CI). Builds the pollcomp lib + the smokes WITH sanitizer flags (so
# they link the ASan runtime the instrumented lib needs), then runs them — leak/UAF/
# overflow coverage of the completion driver, the pollcomp op-table, and the TLS/h2/WS
# buffers the plain build can't give.
smoke-pollcomp-asan:
	$(MAKE) clean
	$(MAKE) BACKEND=pollcomp debug
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_POLLCOMP_BIN) tests/smoke_pollcomp.c -L. -lkeel -lpthread
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_POLLCOMP_TLS_BIN) tests/smoke_pollcomp_tls.c -L. -lkeel -lpthread
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_POLLCOMP_WS_BIN) tests/smoke_pollcomp_ws.c -L. -lkeel -lpthread
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_POLLCOMP_ASYNC_BIN) tests/smoke_pollcomp_async.c -L. -lkeel -lpthread
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_POLLCOMP_CLIENT_BIN) tests/smoke_pollcomp_client.c -L. -lkeel -lpthread
	# LeakSanitizer runs by default under ASan on Linux (CI); macOS ASan runs without it.
	./$(SMOKE_POLLCOMP_BIN)
	./$(SMOKE_POLLCOMP_TLS_BIN)
	./$(SMOKE_POLLCOMP_WS_BIN)
	./$(SMOKE_POLLCOMP_ASYNC_BIN)
	./$(SMOKE_POLLCOMP_CLIENT_BIN)

# RC-2 PROOF: a DEFAULT (epoll/kqueue, readiness-compiled) libkeel serving GET / over a
# RUNTIME-INJECTED pollcomp completion backend — impossible before the provider/glue split.
# Build the default lib (NO BACKEND=), compile event_pollcomp.c as an EXTRA object (the pure
# provider form: no kl_event_*_builtin / kl_comp_ops_builtin → no clash with the default
# backend's _builtin or the readiness kl_comp_ops_builtin stub already in libkeel.a), then
# link smoke_completion_inject.c against libkeel.a + that one object. The smoke sets
# KlConfig.event_provider = kl_event_provider_pollcomp() so the server runs on the injected
# completion loop. Proves the completion axis is genuinely runtime-injectable (RC-2).
SMOKE_INJECT_BIN = tests/smoke_completion_inject$(EXE)
SMOKE_INJECT_OBJ = src/event_pollcomp.inject.o
smoke-completion-inject:
	$(MAKE) clean
	$(MAKE) $(LIB)
	# The injected provider TU — compiled against the SAME CFLAGS as the default lib.
	$(CC) $(CFLAGS) -Isrc -c -o $(SMOKE_INJECT_OBJ) src/event_pollcomp.c
	$(CC) $(CFLAGS) -Isrc -o $(SMOKE_INJECT_BIN) tests/smoke_completion_inject.c \
	      $(SMOKE_INJECT_OBJ) -L. -lkeel -lpthread $(LDFLAGS)
	./$(SMOKE_INJECT_BIN)

# ASan+UBSan variant of the RC-2 proof (leak detection on the injected-completion path).
smoke-completion-inject-asan:
	$(MAKE) clean
	$(MAKE) debug
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp -Isrc \
	      -c -o $(SMOKE_INJECT_OBJ) src/event_pollcomp.c
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp -Isrc \
	      -o $(SMOKE_INJECT_BIN) tests/smoke_completion_inject.c \
	      $(SMOKE_INJECT_OBJ) -L. -lkeel -lpthread
	ASAN_OPTIONS=detect_leaks=1 ./$(SMOKE_INJECT_BIN)

# Same, for the io_uring completion backend (Linux only — needs io_uring). LeakSanitizer here
# covers the io_uring op/registered-buffer/splice/watcher lifecycle the plain smoke can't —
# the gap that let an 8f-5d/teardown watch leak reach main before this target existed.
smoke-iouring-asan:
	$(MAKE) clean
	$(MAKE) BACKEND=iouring debug
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_IOURING_BIN) tests/smoke_iouring.c -L. -lkeel -lpthread -luring
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_IOURING_ASYNC_BIN) tests/smoke_iouring_async.c -L. -lkeel -lpthread -luring
	$(CC) -std=c11 -g -O0 -fsanitize=address,undefined -Iinclude -Ivendor/llhttp \
	      -o $(SMOKE_IOURING_CLIENT_BIN) tests/smoke_iouring_client.c -L. -lkeel -lpthread -luring
	./$(SMOKE_IOURING_BIN)
	./$(SMOKE_IOURING_ASYNC_BIN)
	./$(SMOKE_IOURING_CLIENT_BIN)

# End-to-end HTTP-over-completion roundtrip on the completion-native io_uring backend
# (Linux, BACKEND=iouring). The runtime gate for event_iouring.c — build libkeel
# with BACKEND=iouring first so the server runs on the io_uring completion loop. The
# THIRD completion backend proving the driver is reused verbatim (after IOCP + pollcomp),
# and the first production Linux one.
SMOKE_IOURING_BIN = tests/smoke_iouring$(EXE)
smoke-iouring: $(SMOKE_IOURING_BIN)
	./$(SMOKE_IOURING_BIN)
$(SMOKE_IOURING_BIN): tests/smoke_iouring.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# Async/thread-pool over the io_uring completion loop — the watcher relay via a single-shot
# IORING_OP_POLL_ADD (a thread-pool wakeup fires on the completion loop and resumes).
SMOKE_IOURING_ASYNC_BIN = tests/smoke_iouring_async$(EXE)
smoke-iouring-async: $(SMOKE_IOURING_ASYNC_BIN)
	./$(SMOKE_IOURING_ASYNC_BIN)
$(SMOKE_IOURING_ASYNC_BIN): tests/smoke_iouring_async.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# LC-0: async KlClient connect+GET over the io_uring completion loop — connect driven via
# IORING_OP_CONNECT (kl_comp_post_connect → KL_COMP_CONNECT → he_on_writable). Build with
# BACKEND=iouring first.
SMOKE_IOURING_CLIENT_BIN = tests/smoke_iouring_client$(EXE)
smoke-iouring-client: $(SMOKE_IOURING_CLIENT_BIN)
	./$(SMOKE_IOURING_CLIENT_BIN)
$(SMOKE_IOURING_CLIENT_BIN): tests/smoke_iouring_client.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# Phase 8f-3: unit-test suites that run over the io_uring completion backend — a
# regression gate alongside the smokes. Two groups: backend-agnostic logic suites (they
# don't touch the event loop) + protocol/loop suites that drive the loop through the
# backend-neutral kl_server_run / kl_event_ctx_run path. Run under
# `make BACKEND=iouring test-iouring`.
#
# 56 suites. Grown incrementally: 8f-3 baseline (29) → 8f-5b +5 (the 5a provider auto-wire —
# client_happy_eyeballs, client_pool, error, server_stats, timeout — a default-provider
# server/client now auto-adopts the completion loop's overlapped provider instead of being
# rejected at kl_server_init) → +2 (integration, server_integration) once the completion
# run loop got prompt teardown (kl_server_stop self-pipe wakeup) and graceful-drain progress
# (kl_server_drain_progress ran in the completion branch too — it previously never exited
# drain mode, hanging server_integration's drain tests) → +1 (request) once the completion
# path null-terminated parsed request fields: that was done at the shared conn_dispatch_request
# core (not the readiness call site), so both event models get it and can't drift) → +1
# (udp_server) once the completion UDP recv captured the datagram's local (dest) address via
# an IP_PKTINFO cmsg — a shared kl_udp_parse_local() reused by the readiness recv and the
# completion backends (io_uring/pollcomp), so kl_udp_send_to_from reply-from works over completion)
# → +2 (udp, udp_offload) once the completion recv finished cmsg parity: GRO segment size
# (kl_udp_parse_gro, shared via udp_cmsg.h) + MSG_TRUNC truncation counting, carried on a
# KlUdpRxMeta to kl_udp_comp_on_recv. (udp_multicast's broadcast_flag_gates_send stays excluded:
# it asserts a *synchronous* send EACCES, which only holds for readiness — completion sends are
# queued async, so the error surfaces on the send completion, not the post call.)
# → +4 (client, client_stream, redirect, dns_resolver) once kl_comp_run fired due timers
# (kl_timer_fire) like the readiness kl_event_ctx_run does — without it every timer-driven async
# op stalled over completion (client Happy-Eyeballs delay + request deadline, DNS timeout /
# retransmit / deferred completion, redirect chains).
# → +4 (tls_integration, peer_cert, cross_module, unix_socket) once their per-file readiness-only
# mock TLS was replaced by the shared completion-capable tests/mock_tls.h (feed_input/drain_output),
# so comp_on_accept accepts the TLS conn and comp_tls_drive runs — the TLS-over-completion path the
# smokes already cover, now at unit granularity too (peer_cert installs its canned cert via the
# mock's mock_tls_peer_cert_fn hook). See docs/keel_axis_audit.md third pass.
#
# Still excluded, NOT blocked by backend bugs (see docs/phase8f5 §3 + the axis-audit third pass):
# raw kl_event_wait drivers (event, event_ctx — a completion loop has no readiness kl_event_wait,
# only kl_comp_run) and readiness-cap / provider-negotiation assertions (event_caps,
# socket_provider) are inherently readiness-axis. udp_multicast: broadcast_flag_gates_send asserts
# a *synchronous* EACCES that only holds for readiness (completion sends are queued async).
# Backend-specific: iocp_engine.
# → +1 (peer_addr) once the completion driver grew a PROXY-header phase (comp_drive_proxy +
# kl_conn_ingest_proxy): a trusted-source PROXY header is now parsed over the completion loop (the
# recv is plaintext during KL_CONN_PROXY_HEADER even for a TLS conn), so its proxy_v1/v2_trusted
# tests pass. (This replaced the #134 fail-loud init rejection.)
# → +5 (alpn, event_provider, sockaddr, stream_transport, version) — backend-agnostic unit/seam
# suites with no readiness kl_event_wait driver: pure value tests (sockaddr, version), the ALPN
# selection + stream-transport vtables (mock-TLS / in-memory, no event model), and the event-
# PROVIDER injection seam (which is backend-neutral — unlike the readiness-axis event/event_caps/
# event_ctx below). Verified passing under `make BACKEND=iouring test-iouring` in the Apple
# container (kernel 6.18) on an ext4 checkout. See docs/phase8f5_iouring_default_migration_design.md §3.
#
# → +1 (async) once two fixes landed (2026-08-03): (a) a TEST-FIXTURE gap — the suspend/resume
# tests acquired a bare conn and left conn->ctx NULL, so the completion resume path NULL-derefed in
# kl_comp_post_send (iou_state = c->ctx->loop._backend); now the fixtures set conn->ctx = &s.ev
# mirroring server.c:419, and the watcher/loop tests drive kl_event_ctx_run (the portable wait+
# dispatch tick) instead of a raw readiness kl_event_wait; and (b) a real io_uring BACKEND bug —
# kl_event_mod re-arming an already-armed watch to a new mask no-op'd (iou_arm_watch's
# `if (w->armed) return 0`), so READ→WRITE interest never fired when the old condition couldn't
# occur; kl_event_mod_builtin now retargets the in-flight poll atomically via
# io_uring_prep_poll_update (IORING_POLL_UPDATE_EVENTS). test_async is 19/19 over io_uring (verified
# under ASan+UBSan in the Apple container).
IOURING_TEST_SUITES = allocator alpn async body_reader chunked client client_happy_eyeballs client_pool \
                          client_stream compress connection cors cross_module decompress \
                          dns_resolver drain error event_provider file_io h2 h2_client integration \
                          multipart_stream overflow parser peer_addr peer_cert proxy \
                          proxy_protocol read_flow_control redirect request resolver_cache \
                          response response_parser router server_integration server_stats sockaddr sse \
                          stream_transport thread_pool timeout timer tls tls_integration udp udp_batching \
                          udp_offload udp_server udp_tos unix_socket url version websocket websocket_client
IOURING_TEST_BIN = $(addprefix tests/test_,$(IOURING_TEST_SUITES))
test-iouring: $(IOURING_TEST_BIN)
	@failed=0; \
	for t in $(IOURING_TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME iouring TESTS FAILED"; exit 1; fi

# Datagram link + roundtrip smoke test — the Windows CI gate for udp_io_win.c
# (WSARecvMsg/WSASendMsg + cmsg). Single-threaded event loop, no -lpthread.
SMOKE_UDP_BIN = tests/smoke_udp$(EXE)
smoke-udp: $(SMOKE_UDP_BIN)
	./$(SMOKE_UDP_BIN)
$(SMOKE_UDP_BIN): tests/smoke_udp.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

# DNS resolver link + init/resolve smoke test — the Windows CI gate for
# dns_sys_win.c (iphlpapi config discovery). Single-threaded event loop.
SMOKE_DNS_BIN = tests/smoke_dns$(EXE)
smoke-dns: $(SMOKE_DNS_BIN)
	./$(SMOKE_DNS_BIN)
$(SMOKE_DNS_BIN): tests/smoke_dns.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

# TLS handshake smoke test — a real mbedTLS handshake + request/response over
# loopback with an embedded self-signed cert. The local/BYO validation gate for
# the mbedTLS backend on both POSIX and Windows (mbedTLS stays out of CI). Needs
# KEEL_TLS=mbedtls (else tls_mbedtls.o isn't in the lib). Standalone → -lpthread
# explicitly (Windows LDFLAGS omits it); mbedTLS libs ride in LDFLAGS.
#   make KEEL_TLS=mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls) smoke-tls
SMOKE_TLS_BIN = tests/smoke_tls$(EXE)
smoke-tls: $(SMOKE_TLS_BIN)
	./$(SMOKE_TLS_BIN)
$(SMOKE_TLS_BIN): tests/smoke_tls.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# Buffered-BIO (completion-mode) TLS validation for 8b-5 — a full mbedTLS handshake
# + app data via feed_input/drain_output, no socket. Local/BYO gate (KEEL_TLS=mbedtls).
SMOKE_TLS_COMP_BIN = tests/smoke_tls_completion$(EXE)
smoke-tls-completion: $(SMOKE_TLS_COMP_BIN)
	./$(SMOKE_TLS_COMP_BIN)
$(SMOKE_TLS_COMP_BIN): tests/smoke_tls_completion.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# End-to-end real-mbedTLS over a completion event loop + real socket (broadens the in-memory
# smoke-tls-completion above): smoke_tls.c with the server pinned to the completion provider.
# Build against a completion backend so kl_socket_provider_pollcomp() links:
#   make BACKEND=pollcomp KEEL_TLS=mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls) smoke-tls-completion-e2e
# Local/BYO gate (mbedTLS out of CI).
SMOKE_TLS_E2E_BIN = tests/smoke_tls_completion_e2e$(EXE)
smoke-tls-completion-e2e: $(SMOKE_TLS_E2E_BIN)
	./$(SMOKE_TLS_E2E_BIN)
$(SMOKE_TLS_E2E_BIN): tests/smoke_tls.c $(LIB)
	$(CC) $(CFLAGS) -DSMOKE_TLS_COMPLETION -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

# Optional first-party integrations (integrations/) — bring-your-own libraries,
# never required by `make` / `make test`. Each target skips with a notice when
# its BYO library var is unset (MBEDTLS_DIR / NGHTTP2_DIR), so `integrations` and
# `integration-test` are safe with only some libraries present.
#   make integration-mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls)
#   make integration-nghttp2 NGHTTP2_DIR=$(brew --prefix nghttp2)
#   make integrations        MBEDTLS_DIR=... NGHTTP2_DIR=...
#   make integration-test    MBEDTLS_DIR=... NGHTTP2_DIR=...
integration-mbedtls:
	$(MAKE) -C integrations mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR)
integration-nghttp2:
	$(MAKE) -C integrations nghttp2 NGHTTP2_DIR=$(NGHTTP2_DIR)
integration-lwip:
	$(MAKE) -C integrations lwip LWIP_DIR=$(LWIP_DIR)
integrations:
	$(MAKE) -C integrations all MBEDTLS_DIR=$(MBEDTLS_DIR) NGHTTP2_DIR=$(NGHTTP2_DIR) LWIP_DIR=$(LWIP_DIR)
integration-test:
	$(MAKE) -C integrations test MBEDTLS_DIR=$(MBEDTLS_DIR) NGHTTP2_DIR=$(NGHTTP2_DIR)
.PHONY: integration-mbedtls integration-nghttp2 integration-lwip integrations integration-test

# Install / uninstall
PREFIX  ?= /usr/local
DESTDIR ?=

install: $(LIB) keel.pc
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include/keel
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/keel/*.h $(DESTDIR)$(PREFIX)/include/keel/
	install -m 644 keel.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB)
	rm -rf $(DESTDIR)$(PREFIX)/include/keel
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/keel.pc

keel.pc: keel.pc.in
	sed 's|@PREFIX@|$(PREFIX)|g' $< > $@

clean:
	rm -f $(CORE_OBJ) $(LLHTTP_OBJ) $(TLS_MBEDTLS_OBJ) $(LIB) $(TEST_BIN)
	rm -f tests/smoke_tcp tests/smoke_tcp.exe tests/smoke_udp tests/smoke_udp.exe \
	      tests/smoke_dns tests/smoke_dns.exe tests/smoke_tls tests/smoke_tls.exe
	rm -f $(WIN_TEST_BIN) tests/test_*.exe tests/net_compat_posix.o tests/net_compat_win.o
	rm -f src/event_epoll.o src/event_kqueue.o src/event_poll.o
	# Completion-backend + completion-axis objects are build-conditional (EVENT_SRC per
	# BACKEND; the readiness-stub / KEEL_NO_COMPLETION absent TU per config), so they
	# escape $(CORE_OBJ) on a default clean — remove them unconditionally to prevent a
	# stale cross-toolchain object (e.g. a MinGW completion_driver.o) surviving into a
	# later native build.
	rm -f src/event_iocp.o src/event_pollcomp.o src/event_pollcomp_builtin.o src/event_iouring.o src/completion_driver.o
	rm -f src/completion_dispatch.o src/completion_readiness_stub.o src/completion_absent.o
	rm -f tests/smoke_iouring tests/smoke_iouring_async tests/smoke_iouring_client
	rm -f tests/smoke_pollcomp_client tests/smoke_pollcomp_client.exe
	rm -f tests/smoke_iocp tests/smoke_iocp.exe tests/smoke_pollcomp tests/smoke_pollcomp.exe
	rm -f tests/smoke_iocp_tls tests/smoke_iocp_tls.exe tests/smoke_pollcomp_tls tests/smoke_pollcomp_tls.exe
	rm -f tests/smoke_iocp_async tests/smoke_iocp_async.exe
	rm -f tests/smoke_pollcomp_ws tests/smoke_pollcomp_ws.exe
	rm -f tests/smoke_pollcomp_async tests/smoke_pollcomp_async.exe
	rm -f tests/smoke_completion_inject tests/smoke_completion_inject.exe src/event_pollcomp.inject.o
	rm -f src/file_io.o
	rm -f src/async.o src/error.o src/timer.o src/thread_pool.o src/drain.o src/tls_mbedtls.o integrations/mbedtls/tls_mbedtls.o src/compress_miniz.o src/decompress_miniz.o
	rm -rf .aarch64 src/.aarch64 parsers/.aarch64 vendor/llhttp/.aarch64
	rm -f examples/hello examples/hello_server examples/rest_api examples/rest_api_server examples/middleware examples/static_files examples/streaming examples/body_readers examples/websocket examples/websocket_server examples/websocket_client examples/tls examples/tls_server examples/tls_client examples/async examples/thread_pool examples/h2_server examples/h2_client examples/client examples/async_client examples/async_thread_pool examples/custom_allocator examples/custom_socket_provider examples/connection_pool examples/url_parser examples/sse examples/streaming_client examples/timer examples/redirect_client examples/proxy_client examples/compress_server examples/decompress_client
	rm -f $(BENCH_SERVER)
	rm -f fuzz/fuzz_parser fuzz/fuzz_multipart fuzz/fuzz_websocket fuzz/fuzz_response_parser fuzz/fuzz_dns fuzz/fuzz_proxy fuzz/fuzz_url fuzz/fuzz_decompress
	-$(MAKE) -C integrations clean
	rm -f $(FUZZ_LIB) src/*.fuzz.o parsers/*.fuzz.o vendor/llhttp/*.fuzz.o
	find . -name '*.d' -delete
	rm -f keel.pc
	rm -f coverage.info
	rm -rf coverage_html
	find . -name '*.gcda' -delete
	find . -name '*.gcno' -delete

# Debug build with sanitizers: make debug
DEBUG_CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -g -O0 \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                -Iinclude -Ivendor/llhttp
ifeq ($(UNAME_S),Linux)
  DEBUG_CFLAGS += -D_DEFAULT_SOURCE
endif
DEBUG_LDFLAGS = -fsanitize=address,undefined

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)"

debug-test: debug
	$(MAKE) test CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)"

# Code coverage (Linux, requires lcov/genhtml)
COVERAGE_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
                  -g -O0 --coverage -Iinclude -Ivendor/llhttp
ifeq ($(UNAME_S),Linux)
  COVERAGE_CFLAGS += -D_DEFAULT_SOURCE
endif
COVERAGE_LDFLAGS = --coverage

coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(COVERAGE_CFLAGS)" LDFLAGS="$(COVERAGE_LDFLAGS)"
	$(MAKE) test CFLAGS="$(COVERAGE_CFLAGS)" LDFLAGS="$(COVERAGE_LDFLAGS)"
	lcov --capture --directory . --output-file coverage.info --ignore-errors inconsistent
	lcov --remove coverage.info '*/vendor/*' '*/tests/*' --output-file coverage.info --ignore-errors inconsistent
	genhtml coverage.info --output-directory coverage_html
	@echo "Coverage report: coverage_html/index.html"

# Static analysis
analyze:
	scan-build --status-bugs $(MAKE) clean all

# Address-neutrality gate (PAL: KlSockAddr). The HTTP/connection protocol layer
# must speak KlSockAddr only — no platform struct sockaddr, no getaddrinfo/inet_*,
# no direct socket-header include. Address<->platform marshalling is confined to
# the socket providers + the resolve_sync / sockaddr_native seams. Mechanical
# backstop for docs/keel_sockaddr_design.md (Phase F); mirrors axis-audit Goal 4.
AXIS_PROTO_TUS = src/client.c src/h2_client.c src/websocket_client.c \
                 src/connection.c src/server.c src/h2.c src/websocket.c \
                 src/sse.c src/response.c src/redirect.c src/client_pool.c \
                 src/resolver_cache.c
check-sockaddr-neutral:
	@bad=0; \
	for f in $(AXIS_PROTO_TUS); do \
	  if grep -nE 'struct sockaddr|\bgetaddrinfo[[:space:]]*\(|\bfreeaddrinfo[[:space:]]*\(|\binet_(pton|ntop)[[:space:]]*\(|#[[:space:]]*include[[:space:]]*<(netdb\.h|netinet/|arpa/inet|sys/socket|sys/un\.h)>' "$$f"; then \
	    echo "AXIS VIOLATION: $$f names a platform socket address type/call"; bad=1; \
	  fi; \
	done; \
	if [ $$bad -ne 0 ]; then echo "check-sockaddr-neutral: FAILED"; exit 1; fi; \
	echo "check-sockaddr-neutral: OK ($(words $(AXIS_PROTO_TUS)) protocol TUs are KlSockAddr-only)"

cppcheck:
	cppcheck --enable=all --inline-suppr --suppress=missingIncludeSystem \
	  --suppress=unusedFunction --suppress=checkersReport \
	  --suppress=toomanyconfigs --suppress=staticFunction \
	  --suppress=normalCheckLevelMaxBranches \
	  --suppress=unmatchedSuppression \
	  -UKEEL_PLATFORM_LWIP \
	  --error-exitcode=1 -Iinclude -Ivendor/llhttp src/ parsers/

# W^X / no-runtime-codegen regression guard.
# See SECURITY.md "Architectural Guarantees" for the invariant.
wx-guard:
	@sh tests/no_codegen_surface.sh

# Fuzz testing (requires clang with libFuzzer)
# On Linux: make fuzz CC=clang
# On macOS: make fuzz CC=/opt/homebrew/opt/llvm@18/bin/clang
FUZZ_CFLAGS = -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined \
              -fno-omit-frame-pointer -Iinclude -Ivendor/llhttp

# The fuzzers link a SEPARATE instrumented build of the library: every library
# object is compiled with SanitizerCoverage (-fsanitize=fuzzer-no-link) + ASan +
# UBSan, so libFuzzer actually explores the parsers AND memory-checks them.
# Linking the plain libkeel.a (uninstrumented) would fuzz only the harness — no
# coverage feedback and, worse, no ASan on the library code. Objects use a
# .fuzz.o suffix so they never collide with the normal build. `-w` because the
# production build already enforces -Werror; here we only want instrumentation.
FUZZ_INSTR_CFLAGS = -std=c11 -g -O1 -fsanitize=fuzzer-no-link,address,undefined \
                    -fno-omit-frame-pointer -w -Iinclude -Ivendor/llhttp \
                    $(FUZZ_PLATFORM_CFLAGS) $(MINIZ_CFLAGS)
FUZZ_LIB = libkeel_fuzz.a
FUZZ_LIB_OBJ = $(CORE_SRC:%.c=%.fuzz.o) $(LLHTTP_SRC:%.c=%.fuzz.o) \
               $(COMPRESS_MINIZ_SRC:%.c=%.fuzz.o)

%.fuzz.o: %.c
	$(CC) $(FUZZ_INSTR_CFLAGS) -c -o $@ $<

$(FUZZ_LIB): $(FUZZ_LIB_OBJ)
	$(AR) rcs $@ $^

fuzz/fuzz_parser: fuzz/fuzz_parser.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

fuzz/fuzz_multipart: fuzz/fuzz_multipart.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

fuzz/fuzz_websocket: fuzz/fuzz_websocket.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

fuzz/fuzz_response_parser: fuzz/fuzz_response_parser.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

fuzz/fuzz_dns: fuzz/fuzz_dns.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

fuzz/fuzz_proxy: fuzz/fuzz_proxy.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

fuzz/fuzz_url: fuzz/fuzz_url.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)

# Decompression fuzzer needs the miniz backend compiled into FUZZ_LIB:
#   make fuzz-decompress KEEL_COMPRESS=miniz MINIZ_DIR=/path/to/miniz CC=clang
fuzz/fuzz_decompress: fuzz/fuzz_decompress.c $(FUZZ_LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB) $(LDFLAGS)
fuzz-decompress: fuzz/fuzz_decompress

fuzz: fuzz/fuzz_parser fuzz/fuzz_multipart fuzz/fuzz_websocket fuzz/fuzz_response_parser fuzz/fuzz_dns \
      fuzz/fuzz_proxy fuzz/fuzz_url

# API documentation (requires Doxygen)
docs:
	doxygen Doxyfile

# Benchmark server + suite
BENCH_SERVER = bench/bench_server

$(BENCH_SERVER): bench/bench_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

bench: $(BENCH_SERVER)
	./bench/bench.sh

# Compare the Linux event backends (epoll / io_uring readiness / io_uring completion) on
# one host — PAL 8f step 4. Builds + benchmarks each in turn; see bench/bench_compare.sh.
bench-compare:
	./bench/bench_compare.sh

# Smoke test all examples end-to-end
smoke: examples
	sh tests/e2e_examples.sh

# ── Freestanding public-header gate (step A3) ──────────────────────────────
# Proves the client/protocol subset of the public headers compiles with NO
# hosted libc — -ffreestanding, full -Werror — and that none of them pull a
# POSIX socket/system header. The in-gate vs out-of-gate header list (and why
# response.h / file_io.h / resolver.h / udp*.h stay out) is documented at the
# top of tests/freestanding_headers.c.
#
# The dep proof matches the FORBIDDEN header paths *exactly at a leaf boundary*
# (e.g. .../sys/socket.h, .../strings.h) so it flags only headers our code
# actually includes — never the C-standard-header machinery a libc's <stddef.h>/
# <stdint.h> legitimately uses (macOS pulls sys/_types/_size_t.h; glibc pulls
# bits/*). That keeps the gate portable across libc implementations.
FREESTANDING_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
                      -ffreestanding -DKEEL_FREESTANDING -Iinclude -Ivendor/llhttp
# POSIX/system headers that MUST NOT appear as a dependency leaf.
FREESTANDING_FORBIDDEN = sys/socket.h sys/types.h sys/uio.h sys/un.h strings.h \
                         netinet/in.h netinet/ip.h netinet/tcp.h arpa/inet.h \
                         netdb.h poll.h sys/poll.h unistd.h sys/epoll.h \
                         sys/event.h winsock2.h ws2tcpip.h

freestanding-headers:
	@echo "== compile: freestanding public-header subset (-ffreestanding -Werror) =="
	$(CC) $(FREESTANDING_CFLAGS) -c tests/freestanding_headers.c -o /tmp/keel_freestanding.o
	@rm -f /tmp/keel_freestanding.o
	@echo "== dep proof: no POSIX/system header may be pulled =="
	@$(CC) -ffreestanding -DKEEL_FREESTANDING -Iinclude -Ivendor/llhttp -M \
	    tests/freestanding_headers.c 2>/dev/null \
	  | tr ' \\' '\n\n' | sed '/^$$/d' > /tmp/keel_freestanding.deps
	@bad=0; \
	for h in $(FREESTANDING_FORBIDDEN); do \
	  if grep -E "(^|/)$$h$$" /tmp/keel_freestanding.deps >/dev/null 2>&1; then \
	    echo "  FREESTANDING LEAK: a gate header pulls <$$h>"; \
	    grep -E "(^|/)$$h$$" /tmp/keel_freestanding.deps; bad=1; \
	  fi; \
	done; \
	if [ $$bad -ne 0 ]; then echo "freestanding-headers: FAILED"; rm -f /tmp/keel_freestanding.deps; exit 1; fi; \
	echo "  zero POSIX headers pulled — gate deps (keel + C-standard only):"; \
	grep -E 'include/keel/' /tmp/keel_freestanding.deps | sed 's/^/    /'; \
	rm -f /tmp/keel_freestanding.deps; \
	echo "freestanding-headers: OK"

.PHONY: check-sockaddr-neutral freestanding-headers
.PHONY: all test clean examples debug debug-test analyze cppcheck fuzz docs smoke \
        smoke-tcp smoke-udp smoke-dns install uninstall coverage bench \
        smoke-completion-inject smoke-completion-inject-asan
