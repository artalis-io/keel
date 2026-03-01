CC      = cc
AR      = ar
UNAME_S := $(shell uname -s)
LDFLAGS =

# Detect Cosmopolitan toolchain (cosmocc, x86_64-unknown-cosmo-cc, etc.)
ifneq ($(findstring cosmo,$(CC)),)
  COSMO := 1
endif
ifneq ($(findstring cosmocc,$(CC)),)
  AR      = cosmoar
endif

ifdef COSMO
  # Cosmopolitan: force poll backend, omit -D_DEFAULT_SOURCE and -fstack-protector-strong
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -Iinclude -Ivendor/llhttp
  VENDOR_CFLAGS = -std=c11 -O2 -Iinclude -Ivendor/llhttp
  EVENT_SRC = src/event_poll.c
else
  CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
            -fstack-protector-strong -Iinclude -Ivendor/llhttp
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
           src/connection.c src/server.c src/body_reader_buffer.c \
           src/body_reader_multipart.c src/chunked.c src/cors.c \
           src/websocket.c src/h2.c $(EVENT_SRC)

# Default parser backend (llhttp)
LLHTTP_SRC = parsers/parser_llhttp.c \
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
	$(AR) rcs $@ $^

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
examples/hello: examples/hello.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/rest_api: examples/rest_api.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/streaming_json: examples/streaming_json.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/static_files: examples/static_files.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/stream_body: examples/stream_body.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/multipart: examples/multipart.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/websocket_echo: examples/websocket_echo.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples/h2_server: examples/h2_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lkeel $(LDFLAGS)

examples: examples/hello examples/rest_api examples/streaming_json examples/static_files examples/stream_body examples/multipart examples/websocket_echo examples/h2_server

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
	rm -f src/tls_mbedtls.o
	rm -f examples/hello examples/rest_api examples/streaming_json examples/static_files examples/stream_body examples/multipart examples/websocket_echo examples/h2_server
	rm -f fuzz/fuzz_parser fuzz/fuzz_multipart

# Debug build with sanitizers: make debug
DEBUG_CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O0 \
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

fuzz: fuzz/fuzz_parser fuzz/fuzz_multipart

# API documentation (requires Doxygen)
docs:
	doxygen Doxyfile

.PHONY: all test clean examples debug debug-test analyze cppcheck fuzz docs
