CC      = cc
UNAME_S := $(shell uname -s)
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 \
          -fstack-protector-strong -Iinclude -Ivendor/llhttp
LDFLAGS =

# Vendor code — no warnings, no -Werror
VENDOR_CFLAGS = -std=c11 -O2 -Iinclude -Ivendor/llhttp

# Platform event loop backend
ifeq ($(UNAME_S),Linux)
  ifeq ($(BACKEND),iouring)
    EVENT_SRC = src/event_iouring.c
    LDFLAGS += -luring
  else
    EVENT_SRC = src/event_epoll.c
  endif
  CFLAGS += -D_DEFAULT_SOURCE
  VENDOR_CFLAGS += -D_DEFAULT_SOURCE
else
  EVENT_SRC = src/event_kqueue.c
endif

# Core library — parser-agnostic
CORE_SRC = src/allocator.c src/response.c src/router.c \
           src/connection.c src/server.c src/body_reader_buffer.c \
           src/body_reader_multipart.c $(EVENT_SRC)

# Default parser backend (llhttp)
LLHTTP_SRC = parsers/parser_llhttp.c \
             vendor/llhttp/llhttp.c vendor/llhttp/api.c vendor/llhttp/http.c

CORE_OBJ = $(CORE_SRC:.c=.o)
LLHTTP_OBJ = $(LLHTTP_SRC:.c=.o)
LIB = libkeel.a

all: $(LIB)

$(LIB): $(CORE_OBJ) $(LLHTTP_OBJ)
	ar rcs $@ $^

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

examples: examples/hello examples/rest_api examples/streaming_json examples/static_files examples/stream_body examples/multipart

# Tests — relax warnings triggered by utest.h vendor macros
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:.c=)

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) -Wno-extra-semi -Wno-sign-compare -Ivendor -o $@ $< -L. -lkeel $(LDFLAGS)

test: $(TEST_BIN)
	@failed=0; \
	for t in $(TEST_BIN); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 1 ]; then echo "SOME TESTS FAILED"; exit 1; fi

clean:
	rm -f $(CORE_OBJ) $(LLHTTP_OBJ) $(LIB) $(TEST_BIN)
	rm -f examples/hello examples/rest_api examples/streaming_json examples/static_files examples/stream_body examples/multipart

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

.PHONY: all test clean examples debug
