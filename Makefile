CC      = cc
AR      = ar
UNAME_S := $(shell uname -s)
LDFLAGS =

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
else
  # Build hardening (parity with Hull's W^X posture in docs/security.md):
  #   -fPIE / -pie           — ASLR for executables linking libkeel.a
  #   -fstack-protector-strong — stack canaries on functions with buffers
  #   -D_FORTIFY_SOURCE=3    — runtime checks on str/mem calls (glibc 2.34+ / gcc 12+ / clang 9+).
  #                            Older toolchains emit a noisy warning and behave as =2; leave loud.
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE \
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
CORE_SRC = src/allocator.c src/error.c src/response.c src/router.c \
           src/connection.c src/server.c src/async.c src/timer.c \
           src/body_reader_buffer.c \
           src/body_reader_multipart.c src/chunked.c src/cors.c \
           src/websocket.c src/websocket_client.c \
           src/h2.c src/h2_client.c src/thread_pool.c src/url.c \
           src/client.c src/client_pool.c src/redirect.c src/sse.c \
           src/resolver_cache.c \
           src/compress.c src/decompress.c src/drain.c \
           $(FILE_IO_SRC) $(EVENT_SRC)

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
  CFLAGS += -I$(MINIZ_DIR) -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO
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
           examples/redirect_client examples/proxy_client

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
	rm -f src/event_epoll.o src/event_kqueue.o src/event_iouring.o src/event_poll.o
	rm -f src/file_io.o src/file_io_iouring.o
	rm -f src/async.o src/error.o src/timer.o src/thread_pool.o src/drain.o src/tls_mbedtls.o src/compress_miniz.o src/decompress_miniz.o
	rm -rf .aarch64 src/.aarch64 parsers/.aarch64 vendor/llhttp/.aarch64
	rm -f examples/hello examples/hello_server examples/rest_api examples/rest_api_server examples/middleware examples/static_files examples/streaming examples/body_readers examples/websocket examples/websocket_server examples/websocket_client examples/tls examples/tls_server examples/tls_client examples/async examples/thread_pool examples/h2_server examples/h2_client examples/client examples/async_client examples/async_thread_pool examples/custom_allocator examples/connection_pool examples/url_parser examples/sse examples/streaming_client examples/timer examples/redirect_client examples/proxy_client examples/compress_server examples/decompress_client
	rm -f tests/test_file_io_iouring
	rm -f $(BENCH_SERVER)
	rm -f fuzz/fuzz_parser fuzz/fuzz_multipart fuzz/fuzz_websocket fuzz/fuzz_response_parser
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

fuzz/fuzz_parser: fuzz/fuzz_parser.c $(LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

fuzz/fuzz_multipart: fuzz/fuzz_multipart.c $(LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

fuzz/fuzz_websocket: fuzz/fuzz_websocket.c $(LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

fuzz/fuzz_response_parser: fuzz/fuzz_response_parser.c $(LIB)
	$(CC) $(FUZZ_CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

fuzz: fuzz/fuzz_parser fuzz/fuzz_multipart fuzz/fuzz_websocket fuzz/fuzz_response_parser

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
        install uninstall coverage bench
