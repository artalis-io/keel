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
  EVENT_SRC = src/event_wsapoll.c
  SOCKET_SRC = src/socket_winsock.c
  PLATFORM_SRC = src/platform_win.c
  SERVER_PLAT_SRC = src/server_plat_win.c
  UDP_IO_SRC = src/udp_io_win.c
  DNS_SYS_SRC = src/dns_sys_win.c
  FILE_IO_SRC = src/file_io.c
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
      EVENT_SRC = src/event_iouring.c
      FILE_IO_SRC = src/file_io_iouring.c
      LDFLAGS += -luring
    else ifeq ($(BACKEND),poll)
      EVENT_SRC = src/event_poll.c
      FILE_IO_SRC = src/file_io.c
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
# DNS config discovery (nameservers/hosts/search): POSIX resolv.conf/hosts; the
# Windows branch swaps the iphlpapi sibling. dns_resolver.c itself is #ifdef-free
# and runs over the udp + socket.h seams.
DNS_SYS_SRC ?= src/dns_sys_posix.c
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
           $(FILE_IO_SRC) $(EVENT_SRC)

# The built-in DNS resolver now builds on every platform: dns_resolver.c is
# #ifdef-free (over the udp + socket.h seams) and DNS_SYS_SRC swaps the config-
# discovery TU (dns_sys_posix.c / dns_sys_win.c iphlpapi), so no filtering is
# needed here.

# Default parser backend (llhttp)
LLHTTP_SRC = parsers/parser_llhttp.c parsers/response_parser_llhttp.c \
             vendor/llhttp/llhttp.c vendor/llhttp/api.c vendor/llhttp/http.c

# Optional mbedTLS backend: make KEEL_TLS=mbedtls MBEDTLS_DIR=/path/to/mbedtls
ifdef KEEL_TLS
ifeq ($(KEEL_TLS),mbedtls)
  MBEDTLS_DIR ?= ../mbedtls
  CFLAGS += -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library
  ifdef MBEDTLS_CONFIG_FILE
    CFLAGS += -I$(MBEDTLS_DIR) -DMBEDTLS_CONFIG_FILE='"$(MBEDTLS_CONFIG_FILE)"'
  endif
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
           examples/custom_allocator examples/connection_pool examples/url_parser \
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
# test_file_io_iouring.c requires io_uring — exclude from default builds
TEST_SRC = $(filter-out tests/test_file_io_iouring.c, $(wildcard tests/test_*.c))
ifeq ($(BACKEND),iouring)
  TEST_SRC += tests/test_file_io_iouring.c
endif
TEST_BIN = $(TEST_SRC:.c=)

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) -Wno-pedantic -Wno-sign-compare -Wno-unused-result -Ivendor -o $@ $< -L. -lkeel $(LDFLAGS)

test: $(TEST_BIN)
	@failed=0; \
	for t in $(TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME TESTS FAILED"; exit 1; fi

# Windows unit-test subset (staged toward parity — see docs/phase6_winsock_design.md
# Part C). Tier 1: platform-neutral suites (pure in-memory logic). Tier 2:
# socket/thread runtime suites, validated on the Windows runner (they exercise the
# same WSAPoll/Winsock/winpthreads machinery the smoke tests prove). The remaining
# suites need their direct POSIX network includes (<netinet/in.h>, <sys/socket.h>,
# ...) routed through the shim before they compile under MinGW — see Tier 3 in the
# design doc.
WIN_TEST_SUITES = allocator body_reader chunked cors decompress drain \
                  multipart_stream overflow parser response_parser router url \
                  client client_stream connection h2_client redirect \
                  server_stats thread_pool timer websocket_client
WIN_TEST_BIN = $(addprefix tests/test_,$(addsuffix $(EXE),$(WIN_TEST_SUITES)))

# On Windows the test binaries need the `.exe` suffix and the win_prelude.h
# force-include (utest.h QueryPerformanceCounter clash). POSIX builds fall through
# to the extension-less `tests/%` rule above, so `test-win` also runs natively as
# a subset sanity check.
ifeq ($(WINDOWS),1)
tests/test_%$(EXE): tests/test_%.c $(LIB)
	$(CC) $(CFLAGS) -include tests/win_prelude.h -Wno-pedantic -Wno-sign-compare -Wno-unused-result -Ivendor -o $@ $< -L. -lkeel $(LDFLAGS)
endif

test-win: $(WIN_TEST_BIN)
	@failed=0; \
	for t in $(WIN_TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME WINDOWS TESTS FAILED"; exit 1; fi

# Plaintext TCP link + roundtrip smoke test — the cross-platform link gate
# (the Windows CI runs this to prove the TCP core links and serves). Standalone
# (not a utest suite), so it needs -lpthread explicitly (Windows LDFLAGS omits it).
SMOKE_BIN = tests/smoke_tcp$(EXE)
smoke-tcp: $(SMOKE_BIN)
	./$(SMOKE_BIN)
$(SMOKE_BIN): tests/smoke_tcp.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel -lpthread $(LDFLAGS)

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
	      tests/smoke_dns tests/smoke_dns.exe
	rm -f $(WIN_TEST_BIN) tests/test_*.exe
	rm -f src/event_epoll.o src/event_kqueue.o src/event_iouring.o src/event_poll.o
	rm -f src/file_io.o src/file_io_iouring.o
	rm -f src/async.o src/error.o src/timer.o src/thread_pool.o src/drain.o src/tls_mbedtls.o src/compress_miniz.o src/decompress_miniz.o
	rm -rf .aarch64 src/.aarch64 parsers/.aarch64 vendor/llhttp/.aarch64
	rm -f examples/hello examples/hello_server examples/rest_api examples/rest_api_server examples/middleware examples/static_files examples/streaming examples/body_readers examples/websocket examples/websocket_server examples/websocket_client examples/tls examples/tls_server examples/tls_client examples/async examples/thread_pool examples/h2_server examples/h2_client examples/client examples/async_client examples/async_thread_pool examples/custom_allocator examples/connection_pool examples/url_parser examples/sse examples/streaming_client examples/timer examples/redirect_client examples/proxy_client examples/compress_server examples/decompress_client
	rm -f tests/test_file_io_iouring
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

# Smoke test all examples end-to-end
smoke: examples
	sh tests/e2e_examples.sh

.PHONY: all test clean examples debug debug-test analyze cppcheck fuzz docs smoke \
        smoke-tcp smoke-udp smoke-dns install uninstall coverage bench
