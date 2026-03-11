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
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -Iinclude -Ivendor/llhttp
  VENDOR_CFLAGS = -std=c11 -O2 -Iinclude -Ivendor/llhttp
  EVENT_SRC = src/event_poll.c
else
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -D_FORTIFY_SOURCE=2 -fstack-protector-strong -Iinclude -Ivendor/llhttp
  VENDOR_CFLAGS = -std=c11 -O2 -Iinclude -Ivendor/llhttp

  # Platform event loop backend
  ifeq ($(UNAME_S),Linux)
    ifeq ($(BACKEND),iouring)
      EVENT_SRC = src/event_iouring.c
      LDFLAGS += -luring
    else ifeq ($(BACKEND),poll)
      EVENT_SRC = src/event_poll.c
    else
      EVENT_SRC = src/event_epoll.c
    endif
    CFLAGS += -D_DEFAULT_SOURCE
    VENDOR_CFLAGS += -D_DEFAULT_SOURCE
    LDFLAGS += -lpthread
  else
    ifeq ($(BACKEND),poll)
      EVENT_SRC = src/event_poll.c
    else
      EVENT_SRC = src/event_kqueue.c
    endif
  endif
endif

# Core library — parser-agnostic
CORE_SRC = src/allocator.c src/response.c src/router.c \
           src/connection.c src/server.c src/async.c \
           src/body_reader_buffer.c \
           src/body_reader_multipart.c src/chunked.c src/cors.c \
           src/websocket.c src/websocket_client.c \
           src/h2.c src/h2_client.c src/thread_pool.c src/url.c \
           src/client.c $(EVENT_SRC)

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

CORE_OBJ = $(CORE_SRC:.c=.o)
LLHTTP_OBJ = $(LLHTTP_SRC:.c=.o)
LIB = libkeel.a

all: $(LIB)

$(LIB): $(CORE_OBJ) $(LLHTTP_OBJ) $(TLS_MBEDTLS_OBJ)
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
EXAMPLES = examples/hello examples/rest_api examples/middleware \
           examples/static_files examples/streaming examples/body_readers \
           examples/websocket_server examples/websocket_client \
           examples/async examples/thread_pool \
           examples/h2_server examples/h2_client

examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

# TLS example — only built when KEEL_TLS=mbedtls
ifeq ($(KEEL_TLS),mbedtls)
EXAMPLES += examples/tls
endif

examples: $(EXAMPLES)

# Tests — relax pedantic warnings triggered by utest.h vendor macros
TEST_SRC = $(wildcard tests/test_*.c)
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

clean:
	rm -f $(CORE_OBJ) $(LLHTTP_OBJ) $(TLS_MBEDTLS_OBJ) $(LIB) $(TEST_BIN)
	rm -f src/event_epoll.o src/event_kqueue.o src/event_iouring.o src/event_poll.o
	rm -f src/async.o src/thread_pool.o src/tls_mbedtls.o
	rm -rf .aarch64 src/.aarch64 parsers/.aarch64 vendor/llhttp/.aarch64
	rm -f examples/hello examples/rest_api examples/middleware examples/static_files examples/streaming examples/body_readers examples/websocket examples/websocket_server examples/websocket_client examples/tls examples/async examples/thread_pool examples/h2_server examples/h2_client
	rm -f fuzz/fuzz_parser fuzz/fuzz_multipart fuzz/fuzz_websocket fuzz/fuzz_response_parser

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

# Static analysis
analyze:
	scan-build --status-bugs $(MAKE) clean all

cppcheck:
	cppcheck --enable=all --inline-suppr --suppress=missingIncludeSystem \
	  --suppress=unusedFunction --suppress=checkersReport \
	  --error-exitcode=1 -Iinclude -Ivendor/llhttp src/ parsers/

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

.PHONY: all test clean examples debug debug-test analyze cppcheck fuzz docs
