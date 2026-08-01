# integrations/nghttp2 — standalone build of the nghttp2 HTTP/2 session adapters.
#
# Produces libkeel_h2_nghttp2.a (client + server KlH2*Session vtables over
# nghttp2). Bring-your-own nghttp2 (>= 1.57 for the size_t "*2" API); nothing is
# vendored or downloaded. The core `make` / `make test` never build this.
#
# Usage (from this dir or via the repo root's `make integration-nghttp2`):
#   make NGHTTP2_DIR=$(brew --prefix nghttp2)         # via pkg-config if unset
#   make test NGHTTP2_DIR=...                          # client<->server roundtrip
#
# Link into a consumer:
#   cc app.c -I<keel>/include -Iintegrations/nghttp2 \
#            -L<keel> -lkeel -Lintegrations/nghttp2 -lkeel_h2_nghttp2 \
#            $(pkg-config --libs libnghttp2)

KEEL_ROOT ?= ../..
CC        ?= cc

# nghttp2 flags: prefer NGHTTP2_DIR (a system prefix or source tree), else
# fall back to pkg-config.
ifdef NGHTTP2_DIR
  NGHTTP2_CFLAGS = -I$(NGHTTP2_DIR)/include
  NGHTTP2_LIBS   = -L$(NGHTTP2_DIR)/lib -lnghttp2
else
  NGHTTP2_CFLAGS = $(shell pkg-config --cflags libnghttp2)
  NGHTTP2_LIBS   = $(shell pkg-config --libs libnghttp2)
endif

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
          -fstack-protector-strong
CFLAGS += -I$(KEEL_ROOT)/include -I. $(NGHTTP2_CFLAGS)

LIB = libkeel_h2_nghttp2.a
OBJ = h2_nghttp2_client.o h2_nghttp2_server.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

h2_nghttp2_client.o: h2_nghttp2_client.c keel_h2_nghttp2.h
	$(CC) $(CFLAGS) -c -o $@ $<
h2_nghttp2_server.o: h2_nghttp2_server.c keel_h2_nghttp2.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Functional roundtrip: pairs the client + server sessions in memory (no sockets,
# no KEEL driver), feeding each other's on_send output through recv, and asserts a
# GET request + 200 response with body survive real nghttp2 framing end to end.
test: $(LIB)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o test_roundtrip test_roundtrip.c \
	  -L. -lkeel_h2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS)
	./test_roundtrip
	@echo "integrations/nghttp2: roundtrip OK"

clean:
	rm -f $(LIB) $(OBJ) h2_nghttp2_client.d h2_nghttp2_server.d test_roundtrip link_smoke

.PHONY: all test clean
