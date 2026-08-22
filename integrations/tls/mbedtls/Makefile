# integrations/tls/mbedtls — standalone build of the mbedTLS KlTls adapter.
#
# Produces libkeel_mbedtls.a, a static archive containing just the adapter
# object. Bring-your-own mbedTLS (3.x); nothing here is vendored or downloaded.
# The core `make` / `make test` never build this — it is opt-in.
#
# Usage (from this directory or via the repo root's `make integration-mbedtls`):
#   make MBEDTLS_DIR=$(brew --prefix mbedtls)        # build libkeel_mbedtls.a
#   make test MBEDTLS_DIR=$(brew --prefix mbedtls)   # relocation/link smoke
#
# Link into a consumer (order matters — adapter before core, mbedTLS last):
#   cc app.c -L<keel> -lkeel -L. -lkeel_mbedtls \
#            -L$(MBEDTLS_DIR)/lib -lmbedtls -lmbedx509 -lmbedcrypto
#
# Include from a consumer:
#   cc -I<keel>/include -Iintegrations/tls/mbedtls ...   #  <keel_tls_mbedtls.h>

KEEL_ROOT ?= ../../..
CC        ?= cc

# MBEDTLS_DIR: a source tree (include/ + library/) OR a system prefix
# (include/ + lib/). If unset, the compiler's default search paths are used.
MBEDTLS_INC = $(if $(MBEDTLS_DIR),-I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library,)

# -I$(KEEL_ROOT)/src: the adapter includes the internal socket seam ("socket.h").
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
          -fstack-protector-strong
CFLAGS += -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I. $(MBEDTLS_INC)

LIB = libkeel_mbedtls.a
OBJ = tls_mbedtls.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

tls_mbedtls.o: tls_mbedtls.c keel_tls_mbedtls.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Link smoke: confirms the adapter archives + resolves against core libkeel.a and
# mbedTLS. Requires the core lib to be built first (repo root `make`).
MBEDTLS_LDPATH = $(if $(MBEDTLS_DIR),-L$(MBEDTLS_DIR)/library -L$(MBEDTLS_DIR)/lib,)
test: $(LIB)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/link_smoke tests/link_smoke.c \
	  -L. -lkeel_mbedtls -L$(KEEL_ROOT) -lkeel \
	  $(MBEDTLS_LDPATH) -lmbedtls -lmbedx509 -lmbedcrypto -lpthread
	./tests/link_smoke
	@echo "integrations/tls/mbedtls: link smoke OK"

# End-to-end: both transport axes (socket-BIO + memory-BIO), ALPN, peer_cert,
# clean-shutdown at_eof, plus a hardening/regression suite. Built with ASan+UBSan;
# links the adapter source + core libkeel.a + mbedTLS.
E2E_CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
             -fsanitize=address,undefined -fno-sanitize-recover=all \
             -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I. $(MBEDTLS_INC)
e2e: $(OBJ)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(E2E_CFLAGS) -o tests/tls_e2e tests/tls_e2e.c tls_mbedtls.c \
	  -L$(KEEL_ROOT) -lkeel $(MBEDTLS_LDPATH) -lmbedtls -lmbedx509 -lmbedcrypto -lpthread
	./tests/tls_e2e

clean:
	rm -f $(LIB) $(OBJ) tls_mbedtls.d tests/link_smoke tests/tls_e2e
	rm -rf tests/*.dSYM

.PHONY: all test e2e clean
