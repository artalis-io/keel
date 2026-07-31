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
    # Completion axis: the platform-independent driver (completion_driver.c)
    # provides kl_io_engine_run_completion over the completion.h backend that
    # event_iocp.c implements — so the io_engine.c stub is not linked here.
    IO_ENGINE_SRC =
    COMPLETION_SRC = src/completion_driver.c
  else
    EVENT_SRC = src/event_wsapoll.c
  endif
  SOCKET_SRC = src/socket_winsock.c
  PLATFORM_SRC = src/platform_win.c
  SERVER_PLAT_SRC = src/server_plat_win.c
  UDP_IO_SRC = src/udp_io_win.c
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
      IO_ENGINE_SRC =
      COMPLETION_SRC = src/completion_driver.c
      LDFLAGS += -luring
    else ifeq ($(BACKEND),poll)
      EVENT_SRC = src/event_poll.c
      FILE_IO_SRC = src/file_io.c
    else ifeq ($(BACKEND),pollcomp)
      # Portable completion backend over poll() (PAL 8d-0.5): the completion axis
      # (completion.h) + driver (completion_driver.c) on POSIX, so the completion
      # model is runtime-testable off Windows. IO_ENGINE_SRC empty — the driver
      # provides kl_io_engine_run_completion over event_pollcomp.c's backend.
      EVENT_SRC = src/event_pollcomp.c
      FILE_IO_SRC = src/file_io.c
      IO_ENGINE_SRC =
      COMPLETION_SRC = src/completion_driver.c
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
      EVENT_SRC = src/event_pollcomp.c
      IO_ENGINE_SRC =
      COMPLETION_SRC = src/completion_driver.c
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
SERVER_PLAT_SRC ?= src/server_plat_posix.c
UDP_IO_SRC ?= src/udp_io_posix.c
# Test-harness network helpers: per-platform sibling TU (mirrors SOCKET_SRC), so
# tests/net_compat.h stays logic-#ifdef-free. Linked into each test binary.
TEST_COMPAT_SRC ?= tests/net_compat_posix.c
# DNS config discovery (nameservers/hosts/search): POSIX resolv.conf/hosts; the
# Windows branch swaps the iphlpapi sibling. dns_resolver.c itself is #ifdef-free
# and runs over the udp + socket.h seams.
DNS_SYS_SRC ?= src/dns_sys_posix.c
# Completion-tick stub for the io_engine seam (PAL Phase 8). Linked on every build
# except a completion backend, where completion_driver.c provides the real
# kl_io_engine_run_completion over the completion.h axis.
IO_ENGINE_SRC ?= src/io_engine.c
# The platform-independent completion driver (empty except on completion backends).
COMPLETION_SRC ?=
CORE_SRC = src/allocator.c src/error.c $(SOCKET_SRC) $(PLATFORM_SRC) src/response.c src/router.c \
           src/connection.c src/server.c $(SERVER_PLAT_SRC) src/async.c src/timer.c \
           src/body_reader_buffer.c \
           src/body_reader_multipart.c src/chunked.c src/cors.c \
           src/websocket.c src/websocket_client.c \
           src/h2.c src/h2_client.c src/thread_pool.c src/url.c \
           src/client.c src/client_pool.c src/redirect.c src/sse.c \
           src/resolver_cache.c src/proxy_protocol.c src/udp.c $(UDP_IO_SRC) src/udp_server.c \
           src/dns_resolver.c $(DNS_SYS_SRC) \
           src/compress.c src/decompress.c src/drain.c \
           $(IO_ENGINE_SRC) $(COMPLETION_SRC) $(FILE_IO_SRC) $(EVENT_SRC)

# The built-in DNS resolver now builds on every platform: dns_resolver.c is
# #ifdef-free (over the udp + socket.h seams) and DNS_SYS_SRC swaps the config-
# discovery TU (dns_sys_posix.c / dns_sys_win.c iphlpapi), so no filtering is
# needed here.

# Default parser backend (llhttp)
LLHTTP_SRC = parsers/parser_llhttp.c parsers/response_parser_llhttp.c \
             vendor/llhttp/llhttp.c vendor/llhttp/api.c vendor/llhttp/http.c

# Optional mbedTLS backend (bring-your-own — mbedTLS is not vendored). Portable:
# the same src/tls_mbedtls.c builds on POSIX and Windows (its BIO I/O goes through
# the socket seam). Point MBEDTLS_DIR at a source tree (include/ + library/) OR a
# system prefix (include/ + lib/, e.g. `MBEDTLS_DIR=$(brew --prefix mbedtls)`); if
# unset, the compiler's default search paths are used (e.g. MSYS2 /mingw64).
#   make KEEL_TLS=mbedtls [MBEDTLS_DIR=/path]
ifdef KEEL_TLS
ifeq ($(KEEL_TLS),mbedtls)
  ifdef MBEDTLS_DIR
    CFLAGS  += -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library
    LDFLAGS += -L$(MBEDTLS_DIR)/library -L$(MBEDTLS_DIR)/lib
  endif
  ifdef MBEDTLS_CONFIG_FILE
    CFLAGS += -I$(MBEDTLS_DIR) -DMBEDTLS_CONFIG_FILE='"$(MBEDTLS_CONFIG_FILE)"'
  endif
  LDFLAGS += -lmbedtls -lmbedx509 -lmbedcrypto   # after -lkeel in the link line
  TLS_MBEDTLS_SRC = src/tls_mbedtls.c
  TLS_MBEDTLS_OBJ = src/tls_mbedtls.o
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
	# LeakSanitizer runs by default under ASan on Linux (CI); macOS ASan runs without it.
	./$(SMOKE_POLLCOMP_BIN)
	./$(SMOKE_POLLCOMP_TLS_BIN)
	./$(SMOKE_POLLCOMP_WS_BIN)
	./$(SMOKE_POLLCOMP_ASYNC_BIN)

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
	./$(SMOKE_IOURING_BIN)
	./$(SMOKE_IOURING_ASYNC_BIN)

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

# Phase 8f-3: unit-test suites that run over the io_uring completion backend — a
# regression gate alongside the smokes. Two groups: backend-agnostic logic suites (they
# don't touch the event loop) + protocol/loop suites that drive the loop through the
# backend-neutral kl_server_run / kl_event_ctx_run path. Run under
# `make BACKEND=iouring test-iouring`.
#
# 38 suites. Grown incrementally: 8f-3 baseline (29) → 8f-5b +5 (the 5a provider auto-wire —
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
# completion backends (io_uring/pollcomp), so kl_udp_send_to_from reply-from works over completion.
#
# Still excluded, NOT blocked by backend bugs (see docs/phase8f5 §3): raw kl_event_wait
# drivers (event, event_ctx, async — a completion loop has no readiness kl_event_wait, only
# kl_comp_run; async also builds a conn with no ctx) and readiness-cap / provider-negotiation
# assertions (event_caps, socket_provider) are inherently readiness-axis. The remaining
# default-provider suites (client, client_stream, redirect, peer_addr, peer_cert,
# tls_integration, udp, udp_multicast, udp_offload, unix_socket, dns_resolver,
# cross_module) init over completion (5a) but have per-suite behavioural gaps left to
# triage — incremental, not a correctness prerequisite (the smokes cover the full protocol
# surface). Backend-specific: iocp_engine.
IOURING_TEST_SUITES = allocator body_reader chunked client_happy_eyeballs client_pool \
                          compress connection cors decompress drain error file_io h2 \
                          h2_client integration multipart_stream overflow parser proxy \
                          proxy_protocol request resolver_cache response response_parser router \
                          server_integration server_stats sse thread_pool timeout timer tls \
                          udp_batching udp_server udp_tos url websocket websocket_client
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
	# Completion-backend objects are conditional (COMPLETION_SRC/EVENT_SRC only set
	# under BACKEND=iocp|pollcomp), so they escape $(CORE_OBJ) on a default clean —
	# remove them unconditionally to prevent a stale cross-toolchain object (e.g. a
	# MinGW completion_driver.o) surviving into a later native build.
	rm -f src/event_iocp.o src/event_pollcomp.o src/event_iouring.o src/completion_driver.o
	rm -f tests/smoke_iouring tests/smoke_iouring_async
	rm -f tests/smoke_iocp tests/smoke_iocp.exe tests/smoke_pollcomp tests/smoke_pollcomp.exe
	rm -f tests/smoke_iocp_tls tests/smoke_iocp_tls.exe tests/smoke_pollcomp_tls tests/smoke_pollcomp_tls.exe
	rm -f tests/smoke_iocp_async tests/smoke_iocp_async.exe
	rm -f tests/smoke_pollcomp_ws tests/smoke_pollcomp_ws.exe
	rm -f tests/smoke_pollcomp_async tests/smoke_pollcomp_async.exe
	rm -f src/file_io.o
	rm -f src/async.o src/error.o src/timer.o src/thread_pool.o src/drain.o src/tls_mbedtls.o src/compress_miniz.o src/decompress_miniz.o
	rm -rf .aarch64 src/.aarch64 parsers/.aarch64 vendor/llhttp/.aarch64
	rm -f examples/hello examples/hello_server examples/rest_api examples/rest_api_server examples/middleware examples/static_files examples/streaming examples/body_readers examples/websocket examples/websocket_server examples/websocket_client examples/tls examples/tls_server examples/tls_client examples/async examples/thread_pool examples/h2_server examples/h2_client examples/client examples/async_client examples/async_thread_pool examples/custom_allocator examples/custom_socket_provider examples/connection_pool examples/url_parser examples/sse examples/streaming_client examples/timer examples/redirect_client examples/proxy_client examples/compress_server examples/decompress_client
	rm -f $(BENCH_SERVER)
	rm -f fuzz/fuzz_parser fuzz/fuzz_multipart fuzz/fuzz_websocket fuzz/fuzz_response_parser fuzz/fuzz_dns fuzz/fuzz_proxy fuzz/fuzz_url fuzz/fuzz_decompress
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

cppcheck:
	cppcheck --enable=all --inline-suppr --suppress=missingIncludeSystem \
	  --suppress=unusedFunction --suppress=checkersReport \
	  --suppress=toomanyconfigs \
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

.PHONY: all test clean examples debug debug-test analyze cppcheck fuzz docs smoke \
        smoke-tcp smoke-udp smoke-dns install uninstall coverage bench
