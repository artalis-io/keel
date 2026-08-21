CC      = cc
AR      = ar
NM      = nm
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
  SERVER_PLAT_SRC = src/http_server_plat_win.c
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
SERVER_PLAT_SRC ?= src/http_server_plat_posix.c
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
# Completion axis core (RC-1; split in freestanding B2a). The generic tick
# (completion_core.c: kl_comp_run — routes non-generic completion kinds through the two
# opaque KlEventCtx hooks so it references neither the server nor UDP handlers) + the
# server/TLS leg (completion_http_server.c: the KlHttpConn state machine + kl_io_engine_*) + the
# h2/ws legs (completion_http2.c / completion_ws.c) + the runtime-dispatch surface
# (completion_dispatch.c: the kl_comp_* primitives, routed to the compiled-in backend or a
# runtime provider) are linked on EVERY build. On a readiness build they are never called
# (gated by KL_EVENT_CAP_COMPLETION); on a completion build the dispatch reaches the
# backend's kl_comp_ops_builtin. KEEL_NO_COMPLETION swaps in aborting stubs
# (completion_absent.c) — the axis is compiled out.
ifdef KEEL_NO_COMPLETION
  COMPLETION_CORE = src/completion_absent.c
else
  COMPLETION_CORE = src/completion_core.c src/completion_http_server.c \
                    src/completion_http2.c src/completion_ws.c src/completion_dispatch.c
  # A readiness EVENT_SRC (epoll/kqueue/poll/wsapoll) has no completion backend, so it
  # needs the kl_comp_ops_builtin→NULL stub the dispatch falls back to (never dereferenced).
  # A completion backend (COMPLETION_BACKEND=1) provides its own kl_comp_ops_builtin.
  ifndef COMPLETION_BACKEND
    COMPLETION_CORE += src/completion_readiness_stub.c
  endif
endif
CORE_SRC = src/allocator.c src/allocator_default_stdlib.c src/kl_cstr.c src/error.c src/version.c src/sockaddr.c $(SOCKET_SRC) $(PLATFORM_SRC) $(PLATFORM_WAKEUP_SRC) src/http_response.c src/http_router.c \
           src/http_connection.c src/http_server.c src/http_server_core.c src/http_server_activation.c src/http_proto_hooks.c $(SERVER_PLAT_SRC) src/event_ctx.c src/async.c src/timer.c \
           src/http_body_reader_buffer.c \
           src/http_body_reader_multipart.c src/http1_chunked.c src/http_cors.c \
           src/websocket.c src/http_server_ws.c src/websocket_client.c \
           src/http2_server.c src/http2_client.c src/thread_pool.c src/url.c \
           src/http_client_common.c src/http_client_sync.c src/http_client_async.c \
           src/http_client_proxy.c \
           src/http_client_pool.c src/http_redirect.c src/http_sse.c \
           src/resolver_cache.c src/proxy_protocol.c src/datagram_slots.c src/datagram_send.c src/datagram_recv.c src/datagram_close.c src/datagram_core.c src/datagram_life.c src/datagram.c src/datagram_batch.c src/datagram_open.c $(DGRAM_SRC) $(UDP_CMSG_SRC) \
           src/dns_resolver.c $(DNS_SYS_SRC) src/resolve_sync.c \
           src/compress.c src/decompress.c src/drain.c src/stream.c src/stream_write.c src/stream_read.c src/stream_close.c \
           src/connect_op.c src/listener.c \
           $(COMPLETION_CORE) $(FILE_IO_SRC) src/event_dispatch.c $(EVENT_SRC)

# The built-in DNS resolver now builds on every platform: dns_resolver.c is
# #ifdef-free (over the udp + socket.h seams) and DNS_SYS_SRC swaps the config-
# discovery TU (dns_sys_posix.c / dns_sys_win.c iphlpapi), so no filtering is
# needed here.

# Default parser backend (llhttp)
LLHTTP_SRC = parsers/http1_parser_llhttp.c parsers/http1_response_parser_llhttp.c \
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
# test_datagram_public.c drives a scripted COMPLETION mock (a KL_EVENT_CAP_COMPLETION loop +
# kl_comp_post_dgram_*) with no readiness path; the KEEL_NO_COMPLETION build stubs those entry points
# to abort() (completion_absent.c), so these completion-axis tests cannot run there — exclude them
# (mirrors the readiness-adapting test_datagram_live, which DOES run under KEEL_NO_COMPLETION).
ifdef KEEL_NO_COMPLETION
  TEST_SRC := $(filter-out tests/test_datagram_public.c tests/test_stream_single_shot.c \
                           tests/test_watcher_aba.c, $(TEST_SRC))
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
# A few suites are not listed. Genuinely POSIX/Linux-only: unix_socket (SO_PEERCRED), file_io (POSIX
# file-path assumptions), datagram_socket (copy-POSIX provider fixture). 2 build clean but have runtime failures needing Windows-native
# iteration, deferred for now: dns_resolver (mock-UDP-nameserver + hosts/resolv.conf
# harness) and proxy (CONNECT tunnel timing) — both still covered on Windows by
# smoke-dns and the POSIX suites. (The real mbedTLS backend is validated separately
# by `make KEEL_TLS=mbedtls smoke-tls`; mbedTLS is BYO and stays out of CI.)
WIN_TEST_SUITES = allocator http_body_reader http1_chunked http_cors decompress drain \
                  http_multipart_stream overflow http1_parser http1_response_parser http_router url \
                  http_client http_client_stream http_connection http2_client http_redirect \
                  http_server_stats thread_pool timer websocket_client \
                  error proxy_protocol resolver_cache http_request timeout \
                  http_integration http_server_integration peer_addr http_client_happy_eyeballs \
                  async http_client_pool cross_module event_ctx event_caps \
                  http2 http_response socket_provider websocket compress event http_sse \
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

# Windows IOCP backend (BACKEND=iocp) test subset. The completion connection driver
# does not exist over IOCP yet, so a server cannot run over IOCP — hence the
# backend-lifecycle + negotiation suite (no server-over-IOCP). stream_single_shot is a
# BARE-stream test (raw kl_comp_post_recv/_send + drain, no HTTP connection driver), so
# it validates the IOCP single-shot completion contract natively (R3b-T1); it skips on a
# readiness build. Build with BACKEND=iocp so libkeel.a carries event_iocp.o.
# (test_event_caps is a *readiness*-backend suite — it asserts READINESS caps — so it
# runs in the WSAPoll/POSIX jobs, NOT here.)
WIN_IOCP_TEST_SUITES = iocp_engine stream_single_shot
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

# LC-0 PROOF: async KlHttpClient connect+GET over the pollcomp completion loop — the completion
# CONNECT contract (KL_COMP_CONNECT / kl_comp_post_connect) driving the client's connect over
# the completion axis instead of the readiness WRITE-watcher shim. Build with BACKEND=pollcomp
# first so both the server and the client's KlEventCtx run on the poll() completion loop.
SMOKE_POLLCOMP_CLIENT_BIN = tests/smoke_pollcomp_client$(EXE)
# The async KlHttpClient connect+GET over a pollcomp COMPLETION loop — the LC-0 proof. The smoke
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
# KlHttpServerConfig.event_provider = kl_event_provider_pollcomp() so the server runs on the injected
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

# LC-0: async KlHttpClient connect+GET over the io_uring completion loop — connect driven via
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
# backend-neutral kl_http_server_run / kl_event_ctx_run path. Run under
# `make BACKEND=iouring test-iouring`.
#
# 56 suites. Grown incrementally: 8f-3 baseline (29) → 8f-5b +5 (the 5a provider auto-wire —
# client_happy_eyeballs, client_pool, error, server_stats, timeout — a default-provider
# server/client now auto-adopts the completion loop's overlapped provider instead of being
# rejected at kl_http_server_init) → +2 (integration, server_integration) once the completion
# run loop got prompt teardown (kl_http_server_stop self-pipe wakeup) and graceful-drain progress
# (kl_http_server_drain_progress ran in the completion branch too — it previously never exited
# drain mode, hanging server_integration's drain tests) → +1 (request) once the completion
# path null-terminated parsed request fields: that was done at the shared conn_dispatch_request
# core (not the readiness call site), so both event models get it and can't drift) → +1
# (the datagram completion UDP recv captured the datagram's local (dest) address via an IP_PKTINFO
# cmsg — a shared kl_udp_parse_local() reused by the readiness recv and the completion backends
# (io_uring/pollcomp), so source-pinned reply-from works over completion; the completion recv cmsg
# parity — GRO segment size via kl_udp_parse_gro + MSG_TRUNC truncation counting — is exercised through
# the datagram suites). D2 note: the former udp / udp_server / udp_multicast suites were migrated to
# datagram_socket + datagram_multicast (broadcast is now a deterministic getsockopt check, not a
# readiness-only synchronous-EACCES send probe).
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
# kl_http_conn_ingest_proxy): a trusted-source PROXY header is now parsed over the completion loop (the
# recv is plaintext during KL_HTTP_CONN_PROXY_HEADER even for a TLS conn), so its proxy_v1/v2_trusted
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
# mirroring http_server.c:419, and the watcher/loop tests drive kl_event_ctx_run (the portable wait+
# dispatch tick) instead of a raw readiness kl_event_wait; and (b) a real io_uring BACKEND bug —
# kl_event_mod re-arming an already-armed watch to a new mask no-op'd (iou_arm_watch's
# `if (w->armed) return 0`), so READ→WRITE interest never fired when the old condition couldn't
# occur; kl_event_mod_builtin now retargets the in-flight poll atomically via
# io_uring_prep_poll_update (IORING_POLL_UPDATE_EVENTS). test_async is 19/19 over io_uring (verified
# under ASan+UBSan in the Apple container).
IOURING_TEST_SUITES = allocator alpn async http_body_reader http1_chunked http_client http_client_happy_eyeballs http_client_pool \
                          http_client_stream compress http_connection http_cors cross_module \
                          datagram_batch datagram_life datagram_public datagram_live datagram_socket datagram_multicast \
                          dgram_close dgram_core dgram_recv dgram_recv_classify dgram_send dgram_slots decompress \
                          dns_resolver drain error event_provider file_io http2 http2_client http_integration \
                          http_multipart_stream overflow http1_parser peer_addr peer_cert http_client_proxy \
                          proxy_protocol read_flow_control http_redirect http_request resolver_cache \
                          http_response http1_response_parser http_router http_server_integration http_server_stats sockaddr http_sse \
                          stream_single_shot stream_transport thread_pool timeout timer tls tls_integration \
                          udp_cmsg unix_socket url version websocket websocket_client
IOURING_TEST_BIN = $(addprefix tests/test_,$(IOURING_TEST_SUITES))
test-iouring: $(IOURING_TEST_BIN)
	@failed=0; \
	for t in $(IOURING_TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME iouring TESTS FAILED"; exit 1; fi

# Public KlDatagram link + roundtrip smoke — the runtime proof of the facade's completion fd↔loop
# registration (7B-7). On BACKEND=iocp it exercises CreateIoCompletionPort (the Windows IOCP CI gate);
# pollcomp/io_uring/readiness run it too (their kl_event_add is inert / a readiness watcher).
SMOKE_DATAGRAM_BIN = tests/smoke_datagram$(EXE)
smoke-datagram: $(SMOKE_DATAGRAM_BIN)
	./$(SMOKE_DATAGRAM_BIN)
$(SMOKE_DATAGRAM_BIN): tests/smoke_datagram.c $(LIB)
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
integration-openssl:
	$(MAKE) -C integrations openssl OPENSSL_DIR=$(OPENSSL_DIR)
integration-boringssl:
	$(MAKE) -C integrations boringssl BORINGSSL_DIR=$(BORINGSSL_DIR)
integration-libressl:
	$(MAKE) -C integrations libressl LIBRESSL_DIR=$(LIBRESSL_DIR)
integration-nghttp2:
	$(MAKE) -C integrations nghttp2 NGHTTP2_DIR=$(NGHTTP2_DIR)
integration-lwip:
	$(MAKE) -C integrations lwip LWIP_DIR=$(LWIP_DIR)
integrations:
	$(MAKE) -C integrations all MBEDTLS_DIR=$(MBEDTLS_DIR) OPENSSL_DIR=$(OPENSSL_DIR) LIBRESSL_DIR=$(LIBRESSL_DIR) NGHTTP2_DIR=$(NGHTTP2_DIR) LWIP_DIR=$(LWIP_DIR)
integration-test:
	$(MAKE) -C integrations test MBEDTLS_DIR=$(MBEDTLS_DIR) OPENSSL_DIR=$(OPENSSL_DIR) LIBRESSL_DIR=$(LIBRESSL_DIR) NGHTTP2_DIR=$(NGHTTP2_DIR)
.PHONY: integration-mbedtls integration-openssl integration-boringssl integration-libressl integration-nghttp2 integration-lwip integrations integration-test

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
	rm -f tests/smoke_tcp tests/smoke_tcp.exe \
	      tests/smoke_dns tests/smoke_dns.exe tests/smoke_tls tests/smoke_tls.exe
	rm -f $(WIN_TEST_BIN) tests/test_*.exe tests/net_compat_posix.o tests/net_compat_win.o
	rm -f src/event_epoll.o src/event_kqueue.o src/event_poll.o
	# Completion-backend + completion-axis objects are build-conditional (EVENT_SRC per
	# BACKEND; the readiness-stub / KEEL_NO_COMPLETION absent TU per config), so they
	# escape $(CORE_OBJ) on a default clean — remove them unconditionally to prevent a
	# stale cross-toolchain object (e.g. a MinGW completion_driver.o) surviving into a
	# later native build.
	rm -f src/event_iocp.o src/event_pollcomp.o src/event_pollcomp_builtin.o src/event_iouring.o src/completion_driver.o
	rm -f src/completion_core.o src/completion_server.o src/completion_h2.o src/completion_ws.o
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
	rm -f src/async.o src/event_ctx.o src/error.o src/timer.o src/thread_pool.o src/drain.o src/tls_mbedtls.o integrations/mbedtls/tls_mbedtls.o src/compress_miniz.o src/decompress_miniz.o
	rm -f libkeel_freestanding.a libkeel_freestanding_selfcontained.a
	rm -f libkeel_freestanding*.a libkeel_freestanding_selfcontained*.a
	rm -f libkeel_freestanding_server*.a
	rm -f keel_freestanding.efi keel_freestanding_*.efi keel_freestanding*.lib
	find . -name '*.freestanding.o' -delete
	find . -name '*.fs_*.o' -delete
	find . -name '*.sc.o' -delete
	find . -name '*.link_*.o' -delete
	find . -name '*.compose_*.o' -delete
	rm -f libkeel_freestanding_compose_*.a keel_freestanding_dgram_compose*.efi keel_freestanding_dns_compose*.efi
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
AXIS_PROTO_TUS = src/http_client_common.c src/http_client_sync.c src/http_client_async.c \
                 src/http_client_proxy.c \
                 src/http2_client.c src/websocket_client.c \
                 src/http_connection.c src/http_server.c src/http2_server.c src/websocket.c src/http_server_ws.c \
                 src/http_sse.c src/http_response.c src/http_redirect.c src/http_client_pool.c \
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

# Tier-1 transport-boundary gate (R1). Complements check-sockaddr-neutral (host socket-ADDRESS types).
# DEFAULT-DENY: EVERY src/*.c + parsers/*.c is treated as an above-transport (protocol/util) TU that
# must NOT reach below the Tier-1 transports (KlListener/KlStream/KlDatagram) into engine/provider
# internals — no platform networking/event system header, no raw completion seam (completion.h), no
# completion tick (io_engine.h) — UNLESS it is in TIER1_INFRA: the layer that legitimately bridges
# Tier-1 to the engine/provider (event backends, socket providers, platform glue, the completion
# driver/adapters, the transport state machines, and the run-loop / async-connect drivers). This is
# the mechanical classification rule: a NEWLY ADDED protocol TU is governed automatically (it is not
# in TIER1_INFRA), so a new file cannot silently include completion.h/a platform header the way the
# old AXIS_PROTO_TUS-only manifest allowed (e.g. http_router.c / http_cors.c / http1_chunked.c / body_reader*.c /
# parsers/*.c are now covered). A new INFRASTRUCTURE TU that needs these headers must be added to
# TIER1_INFRA below, with a reason. Include-based → robust vs the WSA*/overlapped mentions that appear
# only in explanatory comments (http_connection.c / http_response.c / http_client_sync.c). Backstop for
# docs/architecture_invariants.md I10; mirrors axis-audit Goal 4.
#
# TIER1_INFRA — the engine/provider/bridge layer (wildcards so new backends auto-classify; explicit
# for the transport machines + run-loop/async drivers). http_server.c / http_client_async.c live here because
# they drive the loop / async connect via the Keel completion tick (io_engine.h). Everything NOT here
# is governed.
TIER1_INFRA = $(wildcard src/event_*.c) $(wildcard src/socket_*.c) $(wildcard src/completion_*.c) \
              $(wildcard src/platform_*.c) $(wildcard src/http_server_plat_*.c) $(wildcard src/dns_sys_*.c) \
              $(wildcard src/udp_cmsg*.c) $(wildcard src/stream*.c) $(wildcard src/datagram*.c) \
              src/listener.c src/connect_op.c \
              src/event_ctx.c src/async.c src/http_server_core.c src/http_server.c src/http_client_async.c
# The forbidden-header regex (shared by the file scan and the self-canary below). Covers the
# completion + readiness/event platform interfaces (epoll/kqueue/eventfd/poll/select/io_uring/IOCP)
# and the socket-ADDRESS headers, plus the internal completion.h / io_engine.h seams.
TIER1_FORBIDDEN_RE = \#[[:space:]]*include[[:space:]]*<(sys/epoll|sys/event|sys/eventfd|sys/poll|sys/select|poll|liburing|mswsock|winsock2|windows|netinet|arpa/inet|sys/socket|sys/un|netdb)\.h>|\#[[:space:]]*include[[:space:]]*<(netinet/|arpa/)|\#[[:space:]]*include[[:space:]]*"(completion|io_engine)\.h"
check-tier1-boundary:
	@bad=0; \
	for f in src/*.c parsers/*.c; do \
	  case " $(TIER1_INFRA) " in *" $$f "*) continue ;; esac; \
	  if grep -nE '$(TIER1_FORBIDDEN_RE)' "$$f"; then \
	    echo "TIER-1 VIOLATION: $$f (above the transport boundary) includes a platform/backend header"; bad=1; \
	  fi; \
	done; \
	if [ $$bad -ne 0 ]; then echo "check-tier1-boundary: FAILED — an above-transport TU reaches a backend header; if it is infrastructure, add it to TIER1_INFRA with a reason"; exit 1; fi; \
	for h in '<poll.h>' '<sys/poll.h>' '<sys/select.h>' '<sys/epoll.h>' '<winsock2.h>' '"completion.h"' '"io_engine.h"'; do \
	  if ! printf '#include %s\n' "$$h" | grep -qE '$(TIER1_FORBIDDEN_RE)'; then \
	    echo "check-tier1-boundary: SELF-TEST FAILED — the forbidden-header regex no longer matches $$h"; exit 1; \
	  fi; \
	done; \
	echo "check-tier1-boundary: OK (default-deny: every src/parsers TU stays above Tier-1 except $(words $(TIER1_INFRA)) allowlisted infrastructure TUs; self-canary green)"

# Documentation-reference gate (R0). Every in-repo path a living-architecture doc links to must
# resolve to a file that exists — so an architecture claim can never point at code/contract/gate
# that has been renamed or deleted. Narrow by construction: only the two living docs are scanned,
# only markdown links (`](target)`), and each target is resolved relative to the doc's directory
# (so `../include/keel/stream.h`, `datagram_contract.md`, `audits/README.md` all check). External
# (http/mailto) and pure `#anchor` links are skipped. Backstop for docs/architecture_invariants.md.
DOC_REF_FILES = docs/architecture.md docs/architecture_invariants.md
check-doc-refs:
	@bad=0; \
	for doc in $(DOC_REF_FILES); do \
	  if [ ! -f "$$doc" ]; then echo "check-doc-refs: MISSING doc $$doc"; bad=1; continue; fi; \
	  dir=`dirname "$$doc"`; \
	  refs=`grep -oE '\]\([^)]+\)' "$$doc" | sed -E 's/^\]\(//; s/\)$$//'`; \
	  for ref in $$refs; do \
	    case "$$ref" in \
	      http://*|https://*|mailto:*|\#*) continue ;; \
	    esac; \
	    path=`printf '%s' "$$ref" | sed -E 's/#.*$$//'`; \
	    [ -z "$$path" ] && continue; \
	    if [ ! -e "$$dir/$$path" ]; then echo "check-doc-refs: $$doc -> broken link: $$path"; bad=1; fi; \
	  done; \
	done; \
	if [ $$bad -ne 0 ]; then echo "check-doc-refs: FAILED"; exit 1; fi; \
	echo "check-doc-refs: OK ($(words $(DOC_REF_FILES)) living-architecture docs, all in-repo links resolve)"

# Stale-name gate (D3-3): the KlUdp/KlUdpServer object API was deleted; fail if it reappears in the
# code tree (src/ include/ tests/ integrations/). TWO INDEPENDENT scans, so an allowed cmsg helper on a
# line can NEVER conceal a forbidden name (a whole-line `grep -v` filter would let `KlUdp x; kl_udp_parse_tos()`
# through):
#   1. Object TYPES (KlUdp / KlUdpServer / KlUdpConfig / KlUdpTransport / KlUdpDatagram / KlUdpHandlerFn /
#      KlUdpRxMeta) — rejected EVERYWHERE, no allowlist, no file exclusion (they are fully deleted, incl.
#      inside the cmsg TUs).
#   2. Function TOKENS (kl_udp_<name>) — each occurrence extracted INDIVIDUALLY (grep -o → one token per
#      output line, keeping its file:line), then only the EXACT retained shared-cmsg helper FAMILIES are
#      allowed: kl_udp_parse_*, kl_udp_build_control, kl_udp_send_family, the Winsock kl_udp_win_*. A token
#      like kl_udp_init on a mixed line is surfaced on its own output line and cannot be masked.
# Historical design docs under docs/ are NOT scanned — their recorded history stands. Binary files are
# skipped (-I). Permanent mixed-line canary proves an allowed helper conceals neither KlUdp nor kl_udp_init.
KLUDP_TYPES_RE = \bKlUdp(Server|Config|Transport|Datagram|HandlerFn|RxMeta)?\b
KLUDP_FN_RE    = kl_udp_[A-Za-z0-9_]+
KLUDP_FN_ALLOW = kl_udp_parse_[a-z0-9_]+|kl_udp_build_control|kl_udp_send_family|kl_udp_win_[a-z0-9_]+
check-no-kludp:
	@bad=0; \
	canary='KlUdp x; kl_udp_parse_tos(a); kl_udp_init(b)'; \
	if ! printf '%s\n' "$$canary" | grep -qE '$(KLUDP_TYPES_RE)'; then \
	  echo "check-no-kludp: SELF-TEST FAILED — object-type regex no longer detects KlUdp on a mixed line"; exit 1; fi; \
	leak=`printf '%s\n' "$$canary" | grep -oE '$(KLUDP_FN_RE)' | grep -vxE '$(KLUDP_FN_ALLOW)'`; \
	if [ "$$leak" != "kl_udp_init" ]; then \
	  echo "check-no-kludp: SELF-TEST FAILED — a cmsg helper concealed kl_udp_init on a mixed line (surfaced: '$$leak')"; exit 1; fi; \
	types=`grep -rInE '$(KLUDP_TYPES_RE)' src include tests integrations 2>/dev/null`; \
	if [ -n "$$types" ]; then echo "$$types"; echo "check-no-kludp: FAILED — a deleted KlUdp object TYPE reappeared (rejected tree-wide)"; bad=1; fi; \
	fns=`grep -rInoE '$(KLUDP_FN_RE)' src include tests integrations 2>/dev/null | grep -vE ':($(KLUDP_FN_ALLOW))$$'`; \
	if [ -n "$$fns" ]; then echo "$$fns"; echo "check-no-kludp: FAILED — a deleted kl_udp_* object function reappeared (allowlist: cmsg helper families only)"; bad=1; fi; \
	if [ $$bad -ne 0 ]; then exit 1; fi; \
	echo "check-no-kludp: OK (object types rejected tree-wide; every kl_udp_* token is a retained cmsg helper)"

# Stale-name gate (T4): the HTTP taxonomy rename (KlServer->KlHttpServer, KlClient->KlHttpClient,
# KlRequest/KlResponse/KlConn/KlRouter/.../KlH2*->KlHttp*/KlHttp1*/KlHttp2*, kl_server_/kl_client_/kl_h2_
# -> kl_http_*, KL_H2_/KL_CLIENT_/... -> KL_HTTP*) is complete; fail if any OLD public name reappears in
# code or LIVING docs. FOUR INDEPENDENT scans (the check-no-kludp lesson -- a whole-line
# grep -v cannot mask a forbidden token co-located with an allowed one):
#   1. Object TYPES -- the exact freeze-S9 identifier list, \b-anchored (so KlConn never matches the
#      generic KlConnectOp; longer names precede their prefixes so KlClientPool wins over KlClient).
#   2. Constant PREFIXES/tokens -- the old KL_ families (KL_CONN_/KL_BODY_/KL_CLIENT_/KL_H2_/KL_PARSE_/
#      KL_CHUNK_/KL_MP_/KL_CORS_/KL_CPOOL_/KL_REDIRECT_/KL_TRANSPORT_/KL_LOG_) + the exact stragglers.
#   3. Function TOKENS -- old kl_ families extracted individually (grep -o); the retained generic roots
#      (kl_socket_/kl_stream_/kl_datagram_/kl_event_/kl_compress_/kl_decompress_/kl_ws_/...) never match,
#      and the new kl_http*/kl_comp_http2_ names are outside every pattern.
#   4. Deleted module FILENAMES -- the old header/TU names (server.h, request.h, connection.c, server_h2.c,
#      client_async.c, h2_client.h, proto_hooks.c, keel_h2_nghttp2.h, ...) so living docs/comments never
#      point at a nonexistent file. Bare-anchored for unambiguous names; the two basenames that also exist
#      as retained example scenarios (sse.c, h2_client.c) are flagged ONLY when path-qualified as src/, so
#      examples/sse.c and examples/h2_client.c (and the example-only h2_server.c) stay allowed.
# SCAN SET = code (src include parsers tests examples bench fuzz integrations) + the LIVING docs:
# README/CLAUDE/AGENTS/CONTRIBUTING, examples' & integrations' READMEs (via their dirs), site/index.html,
# and the docs/ that describe CURRENT public behavior/API -- the two living-architecture docs plus the
# contracts/policies/matrices (alpn_policy, async_lifecycle, capability_matrix, comparison, compatibility,
# roadmap, stream_contract, streaming_contract, transport_surface). OUTSIDE the scan set BY DESIGN (they
# record old names as history): genuine design/audit/phase records under docs/ (phase*/r3*/*_design/*audit,
# datagram/pal/dns/udp designs), this freeze, the taxonomy prompts, generated docs/api/, untracked
# site/build/, and the Makefile itself (it DEFINES the regexes+canaries below, so it would self-match --
# exactly as check-no-kludp's KLUDP_* block contains KlUdp/kl_udp_; its comments are reconciled by hand).
# Binary files skipped (-I). Permanent mixed-line canaries prove a co-located new name masks neither an old
# type/constant nor an old function token.
HTTPLEGACY_TYPES_RE = \b(KlServerStats|KlServer|KlConfig|KlClientPoolConfig|KlClientPoolConn|KlClientPoolEntry|KlClientPool|KlClientConfig|KlClientResponse|KlClientHeader|KlClientDoneFn|KlClientBodyFn|KlClientHeadersFn|KlClientReadFn|KlClientStreamCfg|KlClientState|KlClientConnectAttempt|KlClient|KlProxyConfig|KlRequestParser|KlRequest|KlResponseParserFactory|KlResponseParser|KlResponse|KlBodyMode|KlWriteFn|KlConnState|KlConnPool|KlConn|KlHandler|KlMiddlewareEntry|KlMiddleware|KlRouter|KlRoute|KlParam|KlBodyReaderFactory|KlCorsConfig|KlBodyReader|KlBufReader|KlMultipartReader|KlMultipartPartMeta|KlMultipartPart|KlMultipartConfig|KlMultipartEvent|KlMultipartErrorCode|KlSse|KlCompressStream|KlRedirectClient|KlRedirectConfig|KlRedirectDoneFn|KlChunkedDecoder|KlChunkedState|KlParserFactory|KlParseResult|KlParser|KlH2ServerSessionFactory|KlH2ServerSession|KlH2ServerConfig|KlH2ServerConn|KlH2ServerCallbacks|KlH2ServerStream|KlH2ServerHooks|KlH2ClientSessionFactory|KlH2ClientSession|KlH2ClientConfig|KlH2ClientConn|KlH2ClientCallbacks|KlH2ClientStream|KlH2ClientResponseFn|KlH2ClientResponse|KlH2ClientHeader|KlH2ClientErrorFn|KlH2Client|KlH2WriteFn|KlH2CompHooks|KlAccessLogFn|KlLogFn|KlTransport)\b
HTTPLEGACY_CONST_RE = KL_(CONN|BODY|CLIENT|H2|PARSE|CHUNK|MP|CORS|CPOOL|REDIRECT|TRANSPORT|LOG)_|\bKL_READ_BUF_SIZE\b|\bKL_PEER_(SOCKET|PROXY)\b|\bKL_MAX_PARAMS\b|\bKL_DEFAULT_(MAX_CONNS|READ_TIMEOUT|MAX_BODY_SIZE)\b
HTTPLEGACY_FN_RE = kl_(server|client|request|response|conn|router|cors|body_reader|buf_reader|multipart|sse|redirect|cpool|parser|chunked|h2|comp_h2|compress_stream)_[A-Za-z0-9_]*|\bkl_log(_errno)?\b
HTTPLEGACY_SCAN = src include parsers tests examples bench fuzz integrations README.md CLAUDE.md AGENTS.md CONTRIBUTING.md docs/architecture.md docs/architecture_invariants.md site/index.html docs/alpn_policy.md docs/async_lifecycle.md docs/capability_matrix.md docs/comparison.md docs/compatibility.md docs/roadmap.md docs/stream_contract.md docs/streaming_contract.md docs/transport_surface.md
HTTPLEGACY_FILES_RE = \b(body_reader\.h|body_reader_buffer\.c|body_reader_multipart\.c|body_reader_multipart\.h|chunked\.c|client\.h|client_async\.c|client_common\.c|client_internal\.h|client_pool\.c|client_pool\.h|client_proxy\.c|client_proxy\.h|client_sync\.c|completion_h2\.c|completion_server\.c|conn_internal\.h|connection\.c|connection\.h|cors\.c|cors\.h|h2\.h|h2_client\.h|h2_internal\.h|h2_nghttp2_client\.c|h2_nghttp2_server\.c|h2_server\.h|keel_h2_nghttp2\.h|parser_llhttp\.c|proto_hooks\.c|proto_hooks\.h|redirect\.c|redirect\.h|request\.h|response\.c|response\.h|response_internal\.h|response_parser_llhttp\.c|router\.c|router\.h|server\.c|server\.h|server_activation\.c|server_core\.c|server_h2\.c|server_plat\.h|server_plat_posix\.c|server_plat_win\.c|server_ws\.c|sse\.h)\b|\bsrc/(client|h2_client|sse)\.c\b
check-no-httplegacy:
	@bad=0; \
	tcanary='KlServer x; KlHttpServer ok'; \
	if ! printf '%s\n' "$$tcanary" | grep -qE '$(HTTPLEGACY_TYPES_RE)'; then \
	  echo "check-no-httplegacy: SELF-TEST FAILED -- type regex no longer detects KlServer"; exit 1; fi; \
	ccanary='KL_H2_DEFAULT_MAX_STREAMS x; KL_HTTP2_DEFAULT_MAX_STREAMS ok'; \
	cleak=`printf '%s\n' "$$ccanary" | grep -oE '$(HTTPLEGACY_CONST_RE)'`; \
	if [ "$$cleak" != "KL_H2_" ]; then \
	  echo "check-no-httplegacy: SELF-TEST FAILED -- constant regex leaked/masked (surfaced: '$$cleak')"; exit 1; fi; \
	fcanary='kl_server_init(a); kl_http_server_init(b)'; \
	fleak=`printf '%s\n' "$$fcanary" | grep -oE '$(HTTPLEGACY_FN_RE)'`; \
	if [ "$$fleak" != "kl_server_init" ]; then \
	  echo "check-no-httplegacy: SELF-TEST FAILED -- a new kl_http_* name masked/leaked (surfaced: '$$fleak')"; exit 1; fi; \
	types=`grep -rInE '$(HTTPLEGACY_TYPES_RE)' $(HTTPLEGACY_SCAN) 2>/dev/null`; \
	for keep in examples/sse.c examples/h2_client.c examples/client.c h2_server.c; do \
	  if printf '%s\n' "$$keep" | grep -qE '$(HTTPLEGACY_FILES_RE)'; then \
	    echo "check-no-httplegacy: SELF-TEST FAILED -- filename regex flagged a retained example ($$keep)"; exit 1; fi; \
	done; \
	for drop in src/connection.c src/sse.c src/h2_client.c src/client.c server.h h2_client.h; do \
	  if ! printf '%s\n' "$$drop" | grep -qE '$(HTTPLEGACY_FILES_RE)'; then \
	    echo "check-no-httplegacy: SELF-TEST FAILED -- filename regex no longer detects deleted $$drop"; exit 1; fi; \
	done; \
	if [ -n "$$types" ]; then echo "$$types"; echo "check-no-httplegacy: FAILED -- a renamed HTTP object TYPE reappeared"; bad=1; fi; \
	consts=`grep -rInE '$(HTTPLEGACY_CONST_RE)' $(HTTPLEGACY_SCAN) 2>/dev/null`; \
	if [ -n "$$consts" ]; then echo "$$consts"; echo "check-no-httplegacy: FAILED -- a renamed HTTP CONSTANT reappeared"; bad=1; fi; \
	fns=`grep -rInoE '$(HTTPLEGACY_FN_RE)' $(HTTPLEGACY_SCAN) 2>/dev/null`; \
	if [ -n "$$fns" ]; then echo "$$fns"; echo "check-no-httplegacy: FAILED -- a renamed kl_http* FUNCTION token reappeared"; bad=1; fi; \
	files=`grep -rInE '$(HTTPLEGACY_FILES_RE)' $(HTTPLEGACY_SCAN) 2>/dev/null`; \
	if [ -n "$$files" ]; then echo "$$files"; echo "check-no-httplegacy: FAILED -- a reference to a RENAMED/DELETED module filename reappeared (living docs must not point at nonexistent files)"; bad=1; fi; \
	if [ $$bad -ne 0 ]; then exit 1; fi; \
	echo "check-no-httplegacy: OK (types/constants/functions/filenames -- no legacy HTTP name or deleted-module path in code or living docs)"


# Scoped suppressions for documented false-positives (not real defects):
#  - dns_resolver.c unusedStructMember / knownConditionTrueFalse: cppcheck explores the
#    KEEL_FREESTANDING config, where the DNS-over-TCP fallback (the whole KlDnsTcp struct usage +
#    its functions) is `#ifndef KEEL_FREESTANDING`-compiled-out and dns_hosts_lookup is a stub that
#    returns 0 (no filesystem). Both are fully used in the hosted build; the findings exist only in
#    that stub config.
#  - event_{iocp,iouring,pollcomp}.c constParameterCallback: `life` on cancel_dgram/retire_dgram is
#    fixed non-const by the KlCompletionOps vtable signature — const-ing one impl would mismatch the
#    vtable and require casting the installed function pointers, which cppcheck itself warns about.
cppcheck:
	cppcheck --enable=all --inline-suppr --suppress=missingIncludeSystem \
	  --suppress=unusedFunction --suppress=checkersReport \
	  --suppress=toomanyconfigs --suppress=staticFunction \
	  --suppress=normalCheckLevelMaxBranches \
	  --suppress=unmatchedSuppression \
	  --suppress=unusedStructMember:src/dns_resolver.c \
	  --suppress=knownConditionTrueFalse:src/dns_resolver.c \
	  --suppress=constParameterCallback:src/event_iocp.c \
	  --suppress=constParameterCallback:src/event_iouring.c \
	  --suppress=constParameterCallback:src/event_pollcomp.c \
	  -UKEEL_PLATFORM_LWIP \
	  --error-exitcode=1 -Iinclude -Ivendor/llhttp src/ parsers/

# Readiness event-identity audit gate (step 6B-2): every readiness kl_event_add/mod must register
# the raw KlStream (&conn->stream) as udata, not a bare KlHttpConn. Pointer equality (stream is the
# leading member) hides regressions from behavioral tests. tools/check_readiness_identity.pl parses
# whole call expressions (balanced parens), so it catches BOTH single-line and multiline calls. The
# completion accept path (completion_http_server.c) still registers KlHttpConn until 6B-3 and is excluded.
check-readiness-identity:
	@perl tools/check_readiness_identity.pl \
	     src/http_server.c src/async.c src/http_server_core.c src/completion_http_server.c \
	  && echo "readiness-identity: OK — all connection registrations use &conn->stream"

# Self-test the audit gate against fixtures with single-line AND multiline violations (must FAIL)
# and a clean fixture (must PASS) — proving the gate actually detects multiline regressions.
check-readiness-identity-selftest:
	@perl tools/check_readiness_identity.pl tests/fixtures/readiness_identity_good.c \
	  && echo "selftest: good fixture PASSED (as expected)" \
	  || { echo "selftest FAIL: clean fixture was flagged"; exit 1; }
	@if perl tools/check_readiness_identity.pl tests/fixtures/readiness_identity_bad.c 2>/dev/null; then \
	  echo "selftest FAIL: bad fixture (single-line + multiline violations) was NOT flagged"; exit 1; \
	else \
	  echo "selftest: bad fixture flagged (as expected)"; \
	fi

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

# Compile-only benchmark build (no wrk run) — a public-API consumer outside src/tests/examples,
# so headline renames (KlHttpServer/Request/Response, …) that miss bench/ are caught at compile time.
# Part of the per-increment validation matrix.
bench-build: $(BENCH_SERVER)
	@echo "bench-build: OK ($(BENCH_SERVER) compiles against the public API)"

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
# POSIX socket/system header. The in-gate vs out-of-gate header list (http_response.h
# and file_io.h are now in-gate after off_t→uint64_t; why resolver.h / udp*.h /
# http_server.h stay out) is documented at the top of tests/freestanding_headers.c.
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
	echo "freestanding-headers (tests/freestanding_headers.c): OK"
	@echo "== compile: keel/freestanding.h umbrella (-ffreestanding -Werror) =="
	@printf '#include <keel/freestanding.h>\nint kl_fs_umbrella_probe;\n' > /tmp/keel_fs_umbrella.c
	$(CC) $(FREESTANDING_CFLAGS) -c /tmp/keel_fs_umbrella.c -o /tmp/keel_fs_umbrella.o
	@rm -f /tmp/keel_fs_umbrella.o
	@echo "== dep proof: keel/freestanding.h pulls no POSIX/system header =="
	@$(CC) -ffreestanding -DKEEL_FREESTANDING -Iinclude -Ivendor/llhttp -M \
	    /tmp/keel_fs_umbrella.c 2>/dev/null \
	  | tr ' \\' '\n\n' | sed '/^$$/d' > /tmp/keel_fs_umbrella.deps
	@bad=0; \
	for h in $(FREESTANDING_FORBIDDEN); do \
	  if grep -E "(^|/)$$h$$" /tmp/keel_fs_umbrella.deps >/dev/null 2>&1; then \
	    echo "  FREESTANDING LEAK: keel/freestanding.h pulls <$$h>"; \
	    grep -E "(^|/)$$h$$" /tmp/keel_fs_umbrella.deps; bad=1; \
	  fi; \
	done; \
	rm -f /tmp/keel_fs_umbrella.c /tmp/keel_fs_umbrella.deps; \
	if [ $$bad -ne 0 ]; then echo "freestanding-headers: FAILED (umbrella)"; exit 1; fi; \
	echo "freestanding-headers (keel/freestanding.h): OK"

# ── Freestanding client archive + undefined-symbol whitelist gate (F0) ─────────
# Builds libkeel_freestanding.a from a client-only, completion-only source
# manifest and proves — via nm + tests/freestanding_symbol_gate.sh — that the
# archive's undefined-symbol closure is within a documented whitelist: the
# C-runtime memory/string surface (memcpy/memmove/memset/memcmp/strlen/strcmp/
# strncmp/strcasecmp/strchr/strstr/strtol/snprintf + their FORTIFY __*_chk
# wrappers, + malloc/free/realloc + fprintf/abort/stderr), the KEEL platform +
# resolution hooks (kl_plat_* / kl_monotonic_ms / kl_resolve_sync), and the
# socket/event/completion PROVIDER ops reached via vtable (kl_sockdef_* /
# kl_event_*_builtin / kl_comp_ops_builtin) — the injection points a freestanding
# build fills with its own provider. It contains NO server (kl_http_conn_*/kl_http_server_*/
# comp_on_*/router), thread_pool/pthread, kl_udp_*, kl_dns_*, file_io, or
# OS-syscall/errno symbol; the gate FAILS if any appears. This is the concrete
# payoff of the freestanding phase and the launch point for the UEFI spike
# (docs/phase10_uefi_feasibility_design.md, F-0).
#
# The manifest is genuinely client + completion only: the KlEventCtx/watcher half
# of async.c was split into event_ctx.c (the server-suspend half — which pulls the
# server connection driver — stays in async.c and is EXCLUDED), and the built-in
# DNS auto-create in http_client_async.c is #ifdef'd out under KEEL_FREESTANDING (a
# freestanding client resolves via cfg->resolver or a numeric address; DNS/UDP is
# out of the minimal archive). socket_posix.c (the hosted socket PROVIDER) is
# deliberately NOT in the manifest — a freestanding build supplies its own
# provider, so the kl_sockdef_* ops are legitimately undefined (whitelisted).
FREESTANDING_CLIENT_SRC = \
    src/error.c src/version.c src/allocator.c src/kl_cstr.c \
    src/sockaddr.c src/url.c src/timer.c src/event_ctx.c src/event_dispatch.c \
    src/completion_dispatch.c src/completion_core.c \
    src/http_client_common.c src/http_client_async.c src/http_client_proxy.c src/http_client_pool.c src/decompress.c \
    src/connect_op.c \
    parsers/http1_response_parser_llhttp.c \
    vendor/llhttp/llhttp.c vendor/llhttp/api.c vendor/llhttp/http.c

# Freestanding, cross-representative toolchain. Prefer clang (SanitizerCoverage-
# grade freestanding + the exact flags a UEFI/PE build uses); fall back to cc.
FREESTANDING_LIB_CC := $(shell command -v clang >/dev/null 2>&1 && echo clang || echo $(CC))

# ── MULTI-ARCH (B1, F-6 "test AArch64 early") ────────────────────────────────
# The archive is cross-compiled + symbol-gated for BOTH x86_64 AND aarch64 PE
# targets: UEFI ships on ARM64 too, and the compiler-runtime helper closure
# differs by arch (this is exactly what B1 proves — see the __chkstk note in
# tests/freestanding_symbol_gate.sh, and that NO __aarch64_* outline atomics /
# division helpers appear). Each triple is built with clang --target=<triple>,
# archived, and run through the symbol gate. A triple whose clang backend/headers
# are unavailable is SKIPPED with a printed note (never a build failure), and the
# recipe reports which triples actually ran. When clang is not the freestanding
# toolchain (the cc/gcc fallback — no cross --target without a sysroot), the list
# collapses to the single native host target, preserving the pre-B1 behavior.
FREESTANDING_TARGETS ?= x86_64-unknown-windows aarch64-unknown-windows
FREESTANDING_IS_CLANG := $(shell $(FREESTANDING_LIB_CC) --version 2>/dev/null | grep -qi clang && echo yes)
# The cross archives hold PE/COFF objects; GNU binutils nm/ar may not read every
# COFF variant reliably. Prefer the LLVM tools when present (the task installs
# them alongside clang+lld); fall back to the project's $(NM)/$(AR).
FREESTANDING_NM := $(shell command -v llvm-nm >/dev/null 2>&1 && echo llvm-nm || echo $(NM))
FREESTANDING_AR := $(shell command -v llvm-ar >/dev/null 2>&1 && echo llvm-ar || echo $(AR))

# Freestanding cross-target shim headers: the few hosted C-library headers clang's
# -ffreestanding does NOT provide (string.h / stdlib.h / stdio.h / sys/types.h /
# errno.h) + the Winsock type/constant shims src/sockcompat.h's _WIN32 branch
# expects. Declarations only, no logic — a real UEFI/EDK2 build supplies the
# equivalents. Injected via -isystem for the cross-target builds only; see
# tests/freestanding/shim/README.md.
FREESTANDING_SHIM = tests/freestanding/shim

# -mno-red-zone is x86-only (UEFI interrupt safety). Per-target: only x86_64 gets
# it; the existing probe is x86-only, so the aarch64 pass drops it (clang would
# ignore it but we keep the flag set honest per the task). The cc fallback probes
# once so it stays portable on a native-ARM host.
FREESTANDING_REDZONE := $(shell echo 'int x;' | $(FREESTANDING_LIB_CC) -mno-red-zone -x c -c -o /dev/null - >/dev/null 2>&1 && echo -mno-red-zone)
# Base flags shared by native + cross builds (target triple + red-zone added per pass).
FREESTANDING_LIB_CFLAGS = -std=c11 -ffreestanding -fshort-wchar \
                          -fno-stack-protector -fno-builtin -DKEEL_FREESTANDING \
                          -Iinclude -Ivendor/llhttp -Isrc
FREESTANDING_LIB = libkeel_freestanding.a
FREESTANDING_LIB_OBJ = $(FREESTANDING_CLIENT_SRC:.c=.freestanding.o)

%.freestanding.o: %.c
	$(FREESTANDING_LIB_CC) $(FREESTANDING_LIB_CFLAGS) $(FREESTANDING_REDZONE) -w -c -o $@ $<

# fs_build_and_gate: $(1)=source list  $(2)=archive base name  $(3)=gate mode ("" or selfcontained)
#   $(4)=extra per-file CFLAGS (self-contained fortify/lowering knobs).
# Loops over FREESTANDING_TARGETS (clang) or a single native pass (cc fallback),
# skipping any triple clang can't target; fails only if a supported triple's gate
# fails. Reports the triples that ran.
define fs_build_and_gate
	@ran=""; skipped=""; \
	if [ "$(FREESTANDING_IS_CLANG)" = yes ]; then TARGETS="$(FREESTANDING_TARGETS)"; else TARGETS="__native__"; fi; \
	for tgt in $$TARGETS; do \
	  if [ "$$tgt" = __native__ ]; then \
	    TARGETFLAG=""; RZ="$(FREESTANDING_REDZONE)"; SHIMFLAG=""; label="native ($(FREESTANDING_LIB_CC))"; arch="$$tgt"; \
	  else \
	    if ! echo 'int x;' | $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -c -x c -o /dev/null - >/dev/null 2>&1; then \
	      echo "  SKIP $$tgt (clang backend/headers unavailable)"; skipped="$$skipped $$tgt"; continue; \
	    fi; \
	    TARGETFLAG="--target=$$tgt"; SHIMFLAG="-isystem $(FREESTANDING_SHIM)"; label="$$tgt"; \
	    case "$$tgt" in x86_64*) RZ="-mno-red-zone";; *) RZ="";; esac; \
	  fi; \
	  if [ "$$tgt" = __native__ ]; then archive="$(2).a"; else archive="$(2)_$${tgt%%-*}.a"; fi; \
	  echo "== [$$label] freestanding archive ($$archive) =="; \
	  objs=""; \
	  for f in $(1); do \
	    o=$${f%.c}.fs_$$tgt.o; \
	    $(FREESTANDING_LIB_CC) $$TARGETFLAG $(FREESTANDING_LIB_CFLAGS) $$RZ $$SHIMFLAG $(4) -w -c -o $$o $$f || { echo "  CC FAIL $$f ($$tgt)"; rm -f $$objs; exit 1; }; \
	    objs="$$objs $$o"; \
	  done; \
	  $(FREESTANDING_AR) rcs $$archive $$objs; \
	  sh tests/freestanding_symbol_gate.sh $$archive $(FREESTANDING_NM) $(3) || { rm -f $$objs; exit 1; }; \
	  rm -f $$objs; \
	  lastarchive="$$archive"; \
	  ran="$$ran $$label"; \
	done; \
	if [ -z "$$ran" ]; then echo "freestanding: NO target could be built (all skipped:$$skipped)"; exit 1; fi; \
	if [ "$(FREESTANDING_IS_CLANG)" = yes ] && [ -f "$(2)_x86_64.a" ]; then cp "$(2)_x86_64.a" "$(2).a"; \
	  echo "== canonical archive: $(2).a (from x86_64; also per-arch $(2)_<arch>.a) =="; \
	elif [ "$(FREESTANDING_IS_CLANG)" = yes ] && [ -n "$$lastarchive" ] && [ -f "$$lastarchive" ]; then cp "$$lastarchive" "$(2).a"; \
	  echo "== canonical archive: $(2).a (also per-arch $(2)_<arch>.a) =="; \
	else echo "== archive: $(2).a (native single-target build) =="; fi; \
	echo "== freestanding archive gated OK for:$$ran ==$${skipped:+ (skipped:$$skipped)}"
endef

freestanding-lib:
	@echo "== freestanding client archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f $(FREESTANDING_LIB)
	$(call fs_build_and_gate,$(FREESTANDING_CLIENT_SRC),libkeel_freestanding,,)

# ── Freestanding SERVER archive (Phase 10 UEFI server, S-1) ───────────────────
# The model-blind HTTP/1.1 server core (http_server_core.c) + the completion server
# driver (completion_http_server.c over completion_core.c) + protocol layer
# (http_connection.c, http_response.c, http_router.c, http1_chunked.c, drain.c, http_body_reader_buffer.c,
# the request parser, proxy_protocol.c) + the ws/h2 upgrade-seam storage
# (http_proto_hooks.c, tables NOT installed → HTTP/1.1 only). NO http_server.c (hosted:
# bind/listen/systemd/signals/readiness loop), NO http_server_ws.c/http2_server.c
# (WebSocket/HTTP-2 out of scope), NO OS sockets — a freestanding build injects its
# own socket + completion providers (EFI_TCP4 + the EFI completion backend). The
# symbol gate proves the undefined closure is the SAME documented whitelist as the
# client archive (C-runtime mem*/str* + kl_plat_*/kl_monotonic_ms + provider ops):
# a freestanding server pulls no OS-syscall/errno/thread/udp/dns/file_io symbol.
FREESTANDING_SERVER_SRC = \
    src/error.c src/version.c src/allocator.c src/kl_cstr.c src/sockaddr.c \
    src/timer.c src/event_ctx.c src/event_dispatch.c \
    src/completion_dispatch.c src/completion_core.c src/completion_http_server.c \
    src/listener.c src/stream.c \
    src/http_connection.c src/http_response.c src/http_router.c src/http1_chunked.c src/drain.c \
    src/http_body_reader_buffer.c src/http_server_core.c src/http_proto_hooks.c \
    parsers/http1_parser_llhttp.c \
    vendor/llhttp/llhttp.c vendor/llhttp/api.c vendor/llhttp/http.c

freestanding-lib-server:
	@echo "== freestanding SERVER archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f libkeel_freestanding_server.a
	$(call fs_build_and_gate,$(FREESTANDING_SERVER_SRC),libkeel_freestanding_server,,)

# ── Freestanding DATAGRAM archive (Phase 10 6.4a-1: KlUdp + the Tier-1 machine) ──
# OPT-IN layer — the DNS/UDP surface is deliberately OUT of the minimal client
# archive (FREESTANDING_CLIENT_SRC), so a datagram-free EFI client stays small; a
# consumer that wants KlUdp over a freestanding datagram provider (the future
# EFI_UDP4 backend, 6.4b) links THIS archive instead. The Tier-1 datagram machine
# (slots/send/recv/close/life) + KlUdp + the base TUs they need (allocator/sockaddr/
# event_ctx/event_dispatch/completion_dispatch+core). NO dns_resolver.c (that rides ON
# TOP of this layer — see the freestanding DNS layer / FREESTANDING_DNS_SRC below), NO
# socket PROVIDER (a freestanding build injects EFI_UDP4). The symbol gate proves the
# undefined closure is the SAME documented whitelist as the client/server archives
# (C-runtime mem* + kl_plat_*/provider ops) — no OS-syscall/errno/getaddrinfo/fopen.
FREESTANDING_DGRAM_SRC = \
    src/error.c src/allocator.c src/kl_cstr.c src/sockaddr.c src/timer.c \
    src/event_ctx.c src/event_dispatch.c \
    src/completion_dispatch.c src/completion_core.c \
    src/datagram_slots.c src/datagram_send.c src/datagram_recv.c \
    src/datagram_close.c src/datagram_core.c src/datagram_life.c src/datagram.c src/datagram_open.c

freestanding-lib-dgram:
	@echo "== freestanding DATAGRAM archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f libkeel_freestanding_dgram.a
	$(call fs_build_and_gate,$(FREESTANDING_DGRAM_SRC),libkeel_freestanding_dgram,,)

# The full 6.4a-1 gate: compile (fs archive, per target) + undefined-host-symbol
# (freestanding_symbol_gate.sh, run inside fs_build_and_gate) + COMPOSITION link
# (freestanding-dgram-link — the client + datagram archives link together with no
# duplicate/unresolved across their overlapping base objects) + forbidden-header
# (below). The cross-target build already proves no host POSIX header is reachable
# (the macOS/host include tree is off the --target=windows search path), so the
# dep-scan is belt-and-suspenders: it FAILS if any FREESTANDING_FORBIDDEN header is
# pulled from OUTSIDE the freestanding shim (the shim's winsock2.h/ws2tcpip.h — the
# intended replacements sockcompat's _WIN32 branch selects — are allowed).
freestanding-dgram: freestanding-lib-dgram freestanding-dgram-link
	@echo "== dep proof: datagram TUs pull no HOST forbidden header (shim replacements OK) =="
	@if [ "$(FREESTANDING_IS_CLANG)" != yes ]; then \
	  echo "  SKIP header dep-scan (non-clang; the cross-target compile + symbol gate above proves it)"; \
	else \
	  bad=0; \
	  for f in $(FREESTANDING_DGRAM_SRC); do \
	    $(FREESTANDING_LIB_CC) --target=x86_64-unknown-windows -ffreestanding -DKEEL_FREESTANDING \
	      -isystem $(FREESTANDING_SHIM) -Iinclude -Ivendor/llhttp -Isrc -M $$f 2>/dev/null \
	      | tr ' \\' '\n\n' | sed '/^$$/d' > /tmp/keel_dgram_fs.deps; \
	    for h in $(FREESTANDING_FORBIDDEN); do \
	      if grep -E "(^|/)$$h$$" /tmp/keel_dgram_fs.deps | grep -v "$(FREESTANDING_SHIM)/" >/dev/null 2>&1; then \
	        echo "  FREESTANDING LEAK: $$f pulls host <$$h>"; bad=1; \
	      fi; \
	    done; \
	  done; \
	  rm -f /tmp/keel_dgram_fs.deps; \
	  if [ $$bad -ne 0 ]; then echo "freestanding-dgram: header gate FAILED"; exit 1; fi; \
	  echo "  zero HOST forbidden headers pulled (only shim + keel + C-standard)"; \
	fi
	@echo "== freestanding-dgram gate OK (compile + forbidden-header + undefined-host-symbol + composition) =="

# Composition link probe (6.4a-1 review): the intended EFI build consumes the client
# AND datagram freestanding layers together, and both self-contained archives share
# overlapping base objects (allocator/event_ctx/sockaddr/completion_*/kl_cstr_builtin).
# Static-archive member extraction is link-order-sensitive, so PROVE they compose:
# link the shared entry (freestanding_link_main.c, compiled with -DKEEL_FS_LINK_DGRAM so
# efi_main also references the KlUdp API) against the datagram-SC then client-SC archive
# (the intended EFI order) into a CRT-less PE image, for BOTH PE targets. The link fails
# on any duplicate or unresolved symbol; on-demand extraction must pick ONE copy of each
# shared base object. Needs clang + lld PE (skips with a note otherwise, like freestanding-link).
FREESTANDING_DGRAM_SC_SRC = $(FREESTANDING_DGRAM_SRC) src/kl_cstr_builtin.c

freestanding-dgram-link:
	@echo "== freestanding COMPOSITION link (6.4a-1): client + datagram archives, both PE targets =="
	@if [ "$(FREESTANDING_IS_CLANG)" != yes ]; then echo "  SKIP (needs clang PE cross target + lld)"; exit 0; fi
	@lld_ok=0; \
	if echo 'int mainCRTStartup(void){return 0;}' | $(FREESTANDING_LIB_CC) --target=x86_64-unknown-windows -ffreestanding -nostdlib -fuse-ld=lld -Wl,-entry:mainCRTStartup -Wl,-subsystem:efi_application -x c - -o /tmp/keel_lld_probe2.efi >/dev/null 2>&1; then lld_ok=1; fi; \
	rm -f /tmp/keel_lld_probe2.efi; \
	if [ $$lld_ok -ne 1 ]; then echo "  SKIP (lld PE backend unavailable in this toolchain)"; exit 0; fi; \
	linked=""; \
	for tgt in $(FREESTANDING_TARGETS); do \
	  if ! echo 'int x;' | $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -c -x c -o /dev/null - >/dev/null 2>&1; then echo "  SKIP $$tgt (backend unavailable)"; continue; fi; \
	  case "$$tgt" in x86_64*) RZ="-mno-red-zone"; efi=keel_freestanding_dgram_compose.efi;; *) RZ=""; efi=keel_freestanding_dgram_compose_$${tgt%%-*}.efi;; esac; \
	  echo "== [$$tgt] build client-SC + datagram-SC archives + composition entry =="; \
	  cobjs=""; for f in $(FREESTANDING_SC_SRC); do o=$${f%.c}.compose_c_$$tgt.o; $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) $(FREESTANDING_SC_EXTRA) -w -c -o $$o $$f || { rm -f $$cobjs; exit 1; }; cobjs="$$cobjs $$o"; done; \
	  carc=libkeel_freestanding_compose_client_$$tgt.a; $(FREESTANDING_AR) rcs $$carc $$cobjs; \
	  dobjs=""; for f in $(FREESTANDING_DGRAM_SC_SRC); do o=$${f%.c}.compose_d_$$tgt.o; $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) $(FREESTANDING_SC_EXTRA) -w -c -o $$o $$f || { rm -f $$cobjs $$carc $$dobjs; exit 1; }; dobjs="$$dobjs $$o"; done; \
	  darc=libkeel_freestanding_compose_dgram_$$tgt.a; $(FREESTANDING_AR) rcs $$darc $$dobjs; \
	  mo=tests/freestanding_link_main.compose_$$tgt.o; \
	  $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) -DKEEL_FS_LINK_DGRAM -w -c -o $$mo $(FREESTANDING_LINK_MAIN) || { rm -f $$cobjs $$carc $$dobjs $$darc; exit 1; }; \
	  echo "== [$$tgt] LINK entry + datagram-SC + client-SC (intended EFI order), -nostdlib -fuse-ld=lld =="; \
	  if $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -fuse-ld=lld -Wl,-entry:efi_main -Wl,-subsystem:efi_application -o $$efi $$mo $$darc $$carc; then \
	    echo "  COMPOSE LINK OK (no duplicate/unresolved symbol): $$efi"; \
	    $(FREESTANDING_READOBJ) --file-headers $$efi 2>/dev/null | grep -iE 'Format|Machine|Subsystem' | sed 's/^/    /' || true; \
	    linked="$$linked $$efi"; \
	  else echo "  COMPOSE LINK FAILED for $$tgt"; rm -f $$cobjs $$carc $$dobjs $$darc $$mo $$efi; exit 1; fi; \
	  rm -f $$cobjs $$carc $$dobjs $$darc $$mo; \
	done; \
	if [ -z "$$linked" ]; then echo "freestanding-dgram-link: no arch linked"; exit 1; fi; \
	echo "== freestanding-dgram-link: OK — client + datagram archives compose:$$linked =="

# ── Freestanding DNS layer (6.4a-2) ───────────────────────────────────────────
# The stock async resolver (src/dns_resolver.c) freestanding-enabled: it rides the
# datagram layer (KlUdp over a freestanding provider) for UDP-only Do53 against an
# EXPLICIT cfg->nameserver. dns_resolver.c compiles its three hosted-only surfaces
# out under KEEL_FREESTANDING — RFC 7766 TCP fallback, /etc/hosts, resolv.conf
# discovery — so the archive's undefined closure stays the SAME documented whitelist
# as the dgram archive (mem* + kl_cstr helpers + kl_plat_*/provider ops): NO fopen /
# errno / getaddrinfo / dns_sys / TCP-fallback symbol. This is the archive the future
# EFI_UDP4 backend (6.4b) links the built-in resolver from; the bespoke dns_uefi.c was
# retired against it separately (2026-08).
FREESTANDING_DNS_SRC = $(FREESTANDING_DGRAM_SRC) src/dns_resolver.c

freestanding-lib-dns:
	@echo "== freestanding DNS archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f libkeel_freestanding_dns.a
	$(call fs_build_and_gate,$(FREESTANDING_DNS_SRC),libkeel_freestanding_dns,,)

# Full DNS gate: compile (fs archive, per target) + undefined-host-symbol (gate script,
# inside fs_build_and_gate) + COMPOSITION link (freestanding-dns-link) + forbidden-header
# dep-scan (mirrors freestanding-dgram; belt-and-suspenders over the cross-target compile).
freestanding-dns: freestanding-lib-dns freestanding-dns-link
	@echo "== dep proof: DNS TUs pull no HOST forbidden header (shim replacements OK) =="
	@if [ "$(FREESTANDING_IS_CLANG)" != yes ]; then \
	  echo "  SKIP header dep-scan (non-clang; the cross-target compile + symbol gate above proves it)"; \
	else \
	  bad=0; \
	  for f in $(FREESTANDING_DNS_SRC); do \
	    $(FREESTANDING_LIB_CC) --target=x86_64-unknown-windows -ffreestanding -DKEEL_FREESTANDING \
	      -isystem $(FREESTANDING_SHIM) -Iinclude -Ivendor/llhttp -Isrc -M $$f 2>/dev/null \
	      | tr ' \\' '\n\n' | sed '/^$$/d' > /tmp/keel_dns_fs.deps; \
	    for h in $(FREESTANDING_FORBIDDEN); do \
	      if grep -E "(^|/)$$h$$" /tmp/keel_dns_fs.deps | grep -v "$(FREESTANDING_SHIM)/" >/dev/null 2>&1; then \
	        echo "  FREESTANDING LEAK: $$f pulls host <$$h>"; bad=1; \
	      fi; \
	    done; \
	  done; \
	  rm -f /tmp/keel_dns_fs.deps; \
	  if [ $$bad -ne 0 ]; then echo "freestanding-dns: header gate FAILED"; exit 1; fi; \
	  echo "  zero HOST forbidden headers pulled (only shim + keel + C-standard)"; \
	fi
	@echo "== freestanding-dns gate OK (compile + forbidden-header + undefined-host-symbol + composition) =="

# Composition link probe: the intended EFI build consumes the client AND DNS layers
# together (DNS pulls the datagram layer transitively). Link the shared entry
# (freestanding_link_main.c, -DKEEL_FS_LINK_DNS so efi_main references kl_dns_resolver_create)
# against the DNS-SC then client-SC archive (intended EFI order) into a CRT-less PE image,
# for BOTH PE targets. Fails on any duplicate/unresolved symbol; on-demand extraction must
# pick ONE copy of each shared base object (allocator/sockaddr/event_ctx/completion_*/
# kl_cstr_builtin/udp/datagram_*). Needs clang + lld PE (skips with a note otherwise).
FREESTANDING_DNS_SC_SRC = $(FREESTANDING_DNS_SRC) src/kl_cstr_builtin.c

freestanding-dns-link:
	@echo "== freestanding COMPOSITION link (6.4a-2): client + DNS archives, both PE targets =="
	@if [ "$(FREESTANDING_IS_CLANG)" != yes ]; then echo "  SKIP (needs clang PE cross target + lld)"; exit 0; fi
	@lld_ok=0; \
	if echo 'int mainCRTStartup(void){return 0;}' | $(FREESTANDING_LIB_CC) --target=x86_64-unknown-windows -ffreestanding -nostdlib -fuse-ld=lld -Wl,-entry:mainCRTStartup -Wl,-subsystem:efi_application -x c - -o /tmp/keel_lld_probe3.efi >/dev/null 2>&1; then lld_ok=1; fi; \
	rm -f /tmp/keel_lld_probe3.efi; \
	if [ $$lld_ok -ne 1 ]; then echo "  SKIP (lld PE backend unavailable in this toolchain)"; exit 0; fi; \
	linked=""; \
	for tgt in $(FREESTANDING_TARGETS); do \
	  if ! echo 'int x;' | $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -c -x c -o /dev/null - >/dev/null 2>&1; then echo "  SKIP $$tgt (backend unavailable)"; continue; fi; \
	  case "$$tgt" in x86_64*) RZ="-mno-red-zone"; efi=keel_freestanding_dns_compose.efi;; *) RZ=""; efi=keel_freestanding_dns_compose_$${tgt%%-*}.efi;; esac; \
	  echo "== [$$tgt] build client-SC + DNS-SC archives + composition entry =="; \
	  cobjs=""; for f in $(FREESTANDING_SC_SRC); do o=$${f%.c}.compose_nc_$$tgt.o; $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) $(FREESTANDING_SC_EXTRA) -w -c -o $$o $$f || { rm -f $$cobjs; exit 1; }; cobjs="$$cobjs $$o"; done; \
	  carc=libkeel_freestanding_compose_dnsclient_$$tgt.a; $(FREESTANDING_AR) rcs $$carc $$cobjs; \
	  nobjs=""; for f in $(FREESTANDING_DNS_SC_SRC); do o=$${f%.c}.compose_n_$$tgt.o; $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) $(FREESTANDING_SC_EXTRA) -w -c -o $$o $$f || { rm -f $$cobjs $$carc $$nobjs; exit 1; }; nobjs="$$nobjs $$o"; done; \
	  narc=libkeel_freestanding_compose_dns_$$tgt.a; $(FREESTANDING_AR) rcs $$narc $$nobjs; \
	  mo=tests/freestanding_link_main.compose_dns_$$tgt.o; \
	  $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) -DKEEL_FS_LINK_DNS -w -c -o $$mo $(FREESTANDING_LINK_MAIN) || { rm -f $$cobjs $$carc $$nobjs $$narc; exit 1; }; \
	  echo "== [$$tgt] LINK entry + DNS-SC + client-SC (intended EFI order), -nostdlib -fuse-ld=lld =="; \
	  if $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -fuse-ld=lld -Wl,-entry:efi_main -Wl,-subsystem:efi_application -o $$efi $$mo $$narc $$carc; then \
	    echo "  COMPOSE LINK OK (no duplicate/unresolved symbol): $$efi"; \
	    $(FREESTANDING_READOBJ) --file-headers $$efi 2>/dev/null | grep -iE 'Format|Machine|Subsystem' | sed 's/^/    /' || true; \
	    linked="$$linked $$efi"; \
	  else echo "  COMPOSE LINK FAILED for $$tgt"; rm -f $$cobjs $$carc $$nobjs $$narc $$mo $$efi; exit 1; fi; \
	  rm -f $$cobjs $$carc $$nobjs $$narc $$mo; \
	done; \
	if [ -z "$$linked" ]; then echo "freestanding-dns-link: no arch linked"; exit 1; fi; \
	echo "== freestanding-dns-link: OK — client + DNS archives compose:$$linked =="

# ── Self-contained freestanding archive (optional; bare target, no libc/EDK2) ──
# The default archive leaves mem*/strlen undefined for the platform to supply
# (libc / EDK2 BaseMemoryLib). This variant ALSO links the optional reference
# impls (src/kl_cstr_builtin.c) so the archive provides them itself — for a bare
# target that has neither. The gate runs in "selfcontained" mode: mem*/strlen must
# be DEFINED (not undefined) and the ONLY undefined symbols are the KEEL platform/
# provider hooks + the vendored-llhttp residual (+ the PE __chkstk on the cross
# targets). -fno-builtin + -fno-tree-loop-distribute-patterns stop the compiler
# lowering kl_cstr_builtin.c's byte loops back into self-calls; -D_FORTIFY_SOURCE=0
# avoids __*_chk. Built + gated for BOTH arches too (B1).
FREESTANDING_SC_SRC = $(FREESTANDING_CLIENT_SRC) src/kl_cstr_builtin.c
FREESTANDING_SC_LIB = libkeel_freestanding_selfcontained.a
FREESTANDING_SC_NOLOWER := $(shell echo 'int x;' | $(FREESTANDING_LIB_CC) -fno-tree-loop-distribute-patterns -x c -c -o /dev/null - >/dev/null 2>&1 && echo -fno-tree-loop-distribute-patterns)
FREESTANDING_SC_EXTRA = -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 $(FREESTANDING_SC_NOLOWER)

freestanding-lib-selfcontained:
	@echo "== self-contained freestanding archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f $(FREESTANDING_SC_LIB)
	$(call fs_build_and_gate,$(FREESTANDING_SC_SRC),libkeel_freestanding_selfcontained,selfcontained,$(FREESTANDING_SC_EXTRA))

# Self-contained SERVER archive (Phase 10 UEFI server, S-4): the server core +
# protocol layer + in-archive mem*/strlen (kl_cstr_builtin.c), for a bare EFI target
# with no libc/EDK2 BaseMemoryLib. Same selfcontained gate as the client variant —
# mem*/strlen must be DEFINED; the only undefined symbols are the KEEL platform/
# provider hooks + the vendored-llhttp residual (+ PE __chkstk/_fltused). This is
# what build_s4.sh links the EFI_TCP4 plaintext HTTP server against.
FREESTANDING_SERVER_SC_SRC = $(FREESTANDING_SERVER_SRC) src/kl_cstr_builtin.c
FREESTANDING_SERVER_SC_LIB = libkeel_freestanding_server_selfcontained.a

freestanding-lib-server-selfcontained:
	@echo "== self-contained freestanding SERVER archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f $(FREESTANDING_SERVER_SC_LIB)
	$(call fs_build_and_gate,$(FREESTANDING_SERVER_SC_SRC),libkeel_freestanding_server_selfcontained,selfcontained,$(FREESTANDING_SC_EXTRA))

# Self-contained DATAGRAM+DNS archive (6.4c UEFI EFI_UDP4 e2e): the datagram machine
# (udp + datagram_* + completion) + the STOCK src/dns_resolver.c + in-archive mem*/
# strlen (kl_cstr_builtin.c), for a bare EFI target with no libc/EDK2 BaseMemoryLib.
# Same selfcontained gate — mem*/strlen must be DEFINED; the only undefined symbols are
# the KEEL platform/provider hooks (+ PE __chkstk/_fltused). build_dgram_dns.sh links
# this against the unified EFI socket provider (SOCK_DGRAM → EFI_UDP4) + event_efi so the
# stock resolver runs over KlUdp-over-EFI_UDP4 on real firmware (superseding the now-retired
# bespoke dns_uefi.c).
FREESTANDING_DNS_SC_LIB = libkeel_freestanding_dns_selfcontained.a

freestanding-lib-dns-selfcontained:
	@echo "== self-contained freestanding DNS archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f $(FREESTANDING_DNS_SC_LIB)
	$(call fs_build_and_gate,$(FREESTANDING_DNS_SC_SRC),libkeel_freestanding_dns_selfcontained,selfcontained,$(FREESTANDING_SC_EXTRA))

# Self-contained freestanding DATAGRAM archive: the FREESTANDING_DGRAM_SRC layer (udp +
# datagram_* + the PUBLIC kl_datagram_* facade src/datagram.c + completion + event_ctx) +
# in-archive mem*/strlen (kl_cstr_builtin.c), with NO dns_resolver dead weight — for a bare
# EFI target with no libc/EDK2 BaseMemoryLib. Same selfcontained gate as the DNS variant.
# build_dgram_public.sh links this against the unified EFI socket provider (SOCK_DGRAM →
# EFI_UDP4) + event_efi so the PUBLIC KlDatagram close (7B-9) runs on real firmware.
FREESTANDING_DGRAM_SC_LIB = libkeel_freestanding_dgram_selfcontained.a

freestanding-lib-dgram-selfcontained:
	@echo "== self-contained freestanding DATAGRAM archive: toolchain = $(FREESTANDING_LIB_CC); targets = $(if $(FREESTANDING_IS_CLANG),$(FREESTANDING_TARGETS),native) =="
	@rm -f $(FREESTANDING_DGRAM_SC_LIB)
	$(call fs_build_and_gate,$(FREESTANDING_DGRAM_SC_SRC),libkeel_freestanding_dgram_selfcontained,selfcontained,$(FREESTANDING_SC_EXTRA))

# ── CRT-less PE/COFF link (B2 — the milestone's last literal) ─────────────────
# LINKS libkeel_freestanding_selfcontained.a (mem*/strlen in-archive) into a
# PE/COFF EFI image with NO hosted CRT (-nostdlib, lld PE), proving the archive
# links freestanding. tests/freestanding_link_main.c is a minimal efi_main that
# references the client public API (pulling client_async/common + transitive
# objects) and DEFINES every seam the archive leaves undefined (platform hooks,
# kl_sockdef_*, kl_event_*_builtin/kl_comp_ops_builtin, the llhttp abort/fprintf/
# stderr residual, and __chkstk). It need not RUN — it must LINK with an empty
# undefined set. Success criterion: the link resolves with NO hosted CRT; the
# recipe prints the readobj header proving PE32+ EFI. Requires clang + lld (lld
# is clang's -fuse-ld=lld PE backend). If lld PE is unavailable, the recipe emits
# the strongest achievable proof (compile the entry to a COFF object for each arch
# + the empty self-contained undefined set from freestanding-lib-selfcontained)
# and documents the limitation rather than faking a PE image.
FREESTANDING_LINK_MAIN = tests/freestanding_link_main.c
FREESTANDING_EFI = keel_freestanding.efi
# readobj tool: prefer llvm-readobj (matches clang); fall back to a bare name.
FREESTANDING_READOBJ := $(shell command -v llvm-readobj >/dev/null 2>&1 && echo llvm-readobj || echo readobj)

freestanding-link:
	@echo "== CRT-less PE/COFF link (B2): toolchain = $(FREESTANDING_LIB_CC) =="
	@if [ "$(FREESTANDING_IS_CLANG)" != yes ]; then \
	  echo "  freestanding-link requires clang (PE cross target + lld); toolchain is $(FREESTANDING_LIB_CC) — SKIP"; exit 0; \
	fi
	@lld_ok=0; \
	if echo 'int mainCRTStartup(void){return 0;}' | $(FREESTANDING_LIB_CC) --target=x86_64-unknown-windows -ffreestanding -nostdlib -fuse-ld=lld -Wl,-entry:mainCRTStartup -Wl,-subsystem:efi_application -x c - -o /tmp/keel_lld_probe.efi >/dev/null 2>&1; then lld_ok=1; fi; \
	rm -f /tmp/keel_lld_probe.efi; \
	linked=""; \
	for tgt in $(FREESTANDING_TARGETS); do \
	  if ! echo 'int x;' | $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -c -x c -o /dev/null - >/dev/null 2>&1; then \
	    echo "  SKIP $$tgt (clang backend/headers unavailable)"; continue; \
	  fi; \
	  case "$$tgt" in x86_64*) RZ="-mno-red-zone"; efi="$(FREESTANDING_EFI)";; *) RZ=""; efi="keel_freestanding_$${tgt%%-*}.efi";; esac; \
	  echo "== [$$tgt] build self-contained archive + entry =="; \
	  objs=""; \
	  for f in $(FREESTANDING_SC_SRC); do \
	    o=$${f%.c}.link_$$tgt.o; \
	    $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) $(FREESTANDING_SC_EXTRA) -w -c -o $$o $$f || { rm -f $$objs; exit 1; }; \
	    objs="$$objs $$o"; \
	  done; \
	  arc=libkeel_freestanding_selfcontained_$$tgt.a; \
	  $(FREESTANDING_AR) rcs $$arc $$objs; \
	  mo=tests/freestanding_link_main.link_$$tgt.o; \
	  $(FREESTANDING_LIB_CC) --target=$$tgt $(FREESTANDING_LIB_CFLAGS) $$RZ -isystem $(FREESTANDING_SHIM) -w -c -o $$mo $(FREESTANDING_LINK_MAIN) || { rm -f $$objs $$arc; exit 1; }; \
	  if [ $$lld_ok -eq 1 ]; then \
	    echo "== [$$tgt] LINK -nostdlib -fuse-ld=lld -subsystem:efi_application -> $$efi =="; \
	    if $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -fuse-ld=lld \
	        -Wl,-entry:efi_main -Wl,-subsystem:efi_application -o $$efi $$mo $$arc; then \
	      echo "  LINK OK (no hosted CRT): $$efi"; \
	      file $$efi 2>/dev/null || true; \
	      $(FREESTANDING_READOBJ) --file-headers $$efi 2>/dev/null | grep -iE 'Format|Machine|Magic|Subsystem' | sed 's/^/    /' || true; \
	      linked="$$linked $$efi"; \
	    else \
	      echo "  LINK FAILED for $$tgt"; rm -f $$objs $$arc $$mo; exit 1; \
	    fi; \
	  else \
	    echo "== [$$tgt] lld PE UNAVAILABLE — fallback proof: COFF entry object + archive built, gate its undefined set =="; \
	    file $$mo 2>/dev/null || true; \
	    sh tests/freestanding_symbol_gate.sh $$arc $(FREESTANDING_NM) selfcontained | grep -E 'OK|FAILED'; \
	    echo "  (documented fallback: PE image NOT produced — lld PE backend unavailable in this toolchain)"; \
	  fi; \
	  rm -f $$objs $$arc $$mo; \
	done; \
	if [ $$lld_ok -eq 1 ]; then \
	  if [ -z "$$linked" ]; then echo "freestanding-link: no arch linked"; exit 1; fi; \
	  echo "== freestanding-link: OK — CRT-less PE/COFF EFI image(s) linked:$$linked =="; \
	else \
	  echo "== freestanding-link: fallback proof only (lld PE unavailable) — see note above =="; \
	fi

# ── Freestanding host mock harness (F-7) ──────────────────────────────────────
# Proves libkeel_freestanding.a's async client actually RUNS end-to-end on the
# host over MOCKED platform hooks + socket provider + completion event provider —
# no real syscalls, no loopback socket, deterministic (advanceable mock clock).
# The acceptance milestone from docs/phase10_uefi_feasibility_design.md ("performs
# an HTTP/1.1 GET over a mock completion provider") before any UEFI work.
#
# TOOLCHAIN CHOICE (documented, per the task): rather than linking the
# clang-freestanding ARCHIVE (whose objects are not ASan-instrumented), we compile
# the SAME FREESTANDING_CLIENT_SRC manifest with the HOST ASan/UBSan toolchain +
# -DKEEL_FREESTANDING into the harness. That way ASan/UBSan instrument the LIBRARY
# code too (not just the harness), so a use-after-free / leak inside the client's
# completion state machine — the whole point of F-7 — is actually caught. The
# archive's link closure is still enforced separately by `make freestanding-lib`.
FREESTANDING_HARNESS_BIN = tests/freestanding_harness$(EXE)
FREESTANDING_HARNESS_SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
# Freestanding client TUs + the F-5 host platform TU + the harness, all one CC pass.
FREESTANDING_HARNESS_SRC = $(FREESTANDING_CLIENT_SRC) \
                           tests/freestanding_host_platform.c \
                           tests/freestanding_harness.c
FREESTANDING_HARNESS_CFLAGS = -std=c11 -DKEEL_FREESTANDING \
                              -Iinclude -Ivendor/llhttp -Isrc -Itests \
                              $(FREESTANDING_HARNESS_SAN)
freestanding-harness:
	@echo "== freestanding host mock harness (ASan+UBSan+LSan, -DKEEL_FREESTANDING) =="
	$(CC) $(FREESTANDING_HARNESS_CFLAGS) -w -o $(FREESTANDING_HARNESS_BIN) \
	    $(FREESTANDING_HARNESS_SRC)
	@echo "== run =="
	@# LSan (detect_leaks) is Linux-only; macOS ASan aborts at startup if asked for it.
	@# On macOS the harness's own counting allocator asserts live==0 per scenario, so
	@# leaks are still caught. In the Linux container both LSan AND the counter run.
	@LEAKS=$$(uname -s | grep -qi linux && echo detect_leaks=1 || echo detect_leaks=0); \
	ASAN_OPTIONS=$$LEAKS UBSAN_OPTIONS=halt_on_error=1 $(FREESTANDING_HARNESS_BIN)
	@rm -f $(FREESTANDING_HARNESS_BIN)

# Freestanding DNS harness (6.4a-2 review): RUNS the freestanding (UDP-only) resolver
# on the host under -DKEEL_FREESTANDING over a mock datagram provider + readiness loop,
# EXECUTING the truncation branch (TC → settle-empty → KL_ERR_DNS, no TCP, no timeout).
# Links the same TUs as the freestanding-dns archive + the F-5 host platform TU.
FREESTANDING_DNS_HARNESS_BIN = tests/freestanding_dns_harness$(EXE)
FREESTANDING_DNS_HARNESS_SRC = $(FREESTANDING_DNS_SRC) \
                               tests/freestanding_host_platform.c \
                               tests/freestanding_dns_harness.c
freestanding-dns-harness:
	@echo "== freestanding DNS host mock harness (ASan+UBSan+LSan, -DKEEL_FREESTANDING) =="
	$(CC) $(FREESTANDING_HARNESS_CFLAGS) -w -o $(FREESTANDING_DNS_HARNESS_BIN) \
	    $(FREESTANDING_DNS_HARNESS_SRC)
	@echo "== run =="
	@LEAKS=$$(uname -s | grep -qi linux && echo detect_leaks=1 || echo detect_leaks=0); \
	ASAN_OPTIONS=$$LEAKS UBSAN_OPTIONS=halt_on_error=1 $(FREESTANDING_DNS_HARNESS_BIN)
	@rm -f $(FREESTANDING_DNS_HARNESS_BIN)

# ── EFI_UDP4 datagram provider — freestanding-PE compile gate (6.4b step 4) ────────────
# The BYO EFI datagram-provider TUs (socket_efi_tcp4 unified stream+datagram, socket_efi_udp4,
# event_efi datagram completion wiring) must keep compiling for the REAL UEFI target — a PE/COFF
# cross build with no hosted libc — on BOTH arches UEFI ships (x86_64 + AArch64). This gate compiles
# them with the SAME flags the QEMU build (build_s4.sh) uses (-ffreestanding -fshort-wchar
# -mno-red-zone -DKEEL_FREESTANDING + the freestanding shim), plus -Wpedantic -Werror. Compile-only
# (-c); the actual EFI link + run is the QEMU/OVMF e2e (6.4c). Skips with a note off clang / a missing
# PE backend. Host correctness is covered by the mock-EFI harness (build_mock_efi_test.sh).
# Datagram build (KEEL_UEFI_DATAGRAM on): the unified provider + event_efi datagram completion +
# the EFI_UDP4 substrate. TCP-only build (KEEL_UEFI_DATAGRAM off): event_efi + socket_efi_tcp4 must
# compile with NO datagram code and NO kl_uefi_udp_*/KlDgramLife references — the boundary that keeps
# U-3/U-4/U-7 + S-4/S-6/S-7 (which link event_efi.c but not the UDP provider) building. The gate
# proves BOTH configs compile, both arches. socket_efi_udp4.c is datagram-only (no TCP-only pass).
UEFI_DGRAM_TU     = integrations/uefi/socket_efi_tcp4.c integrations/uefi/socket_efi_udp4.c \
                    integrations/uefi/event_efi.c
UEFI_TCPONLY_TU   = integrations/uefi/socket_efi_tcp4.c integrations/uefi/event_efi.c
UEFI_DGRAM_GATE_CFLAGS = -ffreestanding -fshort-wchar -fno-stack-protector -fno-builtin -std=c11 \
                         -DKEEL_FREESTANDING -isystem $(FREESTANDING_SHIM) \
                         -Iinclude -Ivendor/llhttp -Isrc -Iintegrations/uefi -Ispikes/uefi \
                         -Wall -Wextra -Wpedantic -Werror
# UEFI_GATE_STRICT (set by CI, .github/workflows/ci.yml): every FREESTANDING_TARGETS arch MUST compile —
# no toolchain-absence SKIP is allowed to green-pass. Locally it is unset, so a dev without the clang PE
# cross target still gets an (announced) skip rather than a hard failure. Either way the gate counts the
# arches it actually compiled and refuses to print OK for arches it skipped (no false-green no-op).
uefi-dgram-gate:
	@echo "== EFI_UDP4 datagram provider: freestanding-PE compile gate (x86_64 + aarch64, -Werror -Wpedantic) =="
	@if [ "$(FREESTANDING_IS_CLANG)" != yes ]; then \
	  if [ -n "$(UEFI_GATE_STRICT)" ]; then echo "  FAIL: strict gate requires a clang PE cross target"; exit 1; fi; \
	  echo "  SKIP (needs a clang PE cross target — set UEFI_GATE_STRICT=1 to require it)"; exit 0; \
	fi
	@want=0; got=0; \
	for tgt in $(FREESTANDING_TARGETS); do \
	  want=$$((want+1)); \
	  if ! echo 'int x;' | $(FREESTANDING_LIB_CC) --target=$$tgt -ffreestanding -c -x c -o /dev/null - >/dev/null 2>&1; then \
	    if [ -n "$(UEFI_GATE_STRICT)" ]; then echo "  FAIL: strict gate requires the $$tgt PE backend"; exit 1; fi; \
	    echo "  SKIP $$tgt (backend unavailable)"; continue; \
	  fi; \
	  case "$$tgt" in x86_64*) RZ="-mno-red-zone";; *) RZ="";; esac; \
	  for f in $(UEFI_DGRAM_TU); do \
	    $(FREESTANDING_LIB_CC) --target=$$tgt $$RZ $(UEFI_DGRAM_GATE_CFLAGS) -DKEEL_UEFI_DATAGRAM -c $$f -o /dev/null || \
	      { echo "  FAIL [$$tgt] (datagram) $$f"; exit 1; }; \
	  done; \
	  for f in $(UEFI_TCPONLY_TU); do \
	    $(FREESTANDING_LIB_CC) --target=$$tgt $$RZ $(UEFI_DGRAM_GATE_CFLAGS) -c $$f -o /dev/null || \
	      { echo "  FAIL [$$tgt] (TCP-only: KEEL_UEFI_DATAGRAM off) $$f"; exit 1; }; \
	  done; \
	  echo "  [$$tgt] EFI TUs compiled clean (datagram + TCP-only)"; got=$$((got+1)); \
	done; \
	if [ -n "$(UEFI_GATE_STRICT)" ] && [ "$$got" -lt "$$want" ]; then \
	  echo "  FAIL: strict gate compiled $$got/$$want arches"; exit 1; \
	fi; \
	if [ "$$got" -eq 0 ]; then echo "  SKIP: no PE arch compiled (no false green)"; exit 0; fi; \
	echo "== uefi-dgram-gate OK ($$got/$$want arch(es): datagram [tcp4+udp4+event_efi] + TCP-only [tcp4+event_efi]) =="

.PHONY: check-sockaddr-neutral check-tier1-boundary check-doc-refs check-no-kludp check-no-httplegacy freestanding-headers freestanding-lib freestanding-lib-dgram freestanding-dgram freestanding-dgram-link freestanding-lib-dns freestanding-dns freestanding-dns-link freestanding-dns-harness uefi-dgram-gate freestanding-lib-selfcontained freestanding-lib-server freestanding-lib-server-selfcontained freestanding-lib-dns-selfcontained freestanding-lib-dgram-selfcontained freestanding-link freestanding-harness
.PHONY: all test clean examples debug debug-test analyze cppcheck fuzz docs smoke \
        smoke-tcp smoke-dns install uninstall coverage bench bench-build \
        smoke-completion-inject smoke-completion-inject-asan
