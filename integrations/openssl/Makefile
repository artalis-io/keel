# integrations/openssl — standalone build of the OpenSSL/BoringSSL KlTls adapter.
#
# Produces libkeel_openssl.a, a static archive containing the shared adapter
# object (tls_openssl.c). Bring-your-own OpenSSL 3.x (or BoringSSL — see
# integrations/boringssl/, which compiles THIS source against a BoringSSL
# prefix). Nothing here is vendored or downloaded. The core `make` / `make test`
# never build this — it is opt-in.
#
# Usage:
#   make OPENSSL_DIR=$(brew --prefix openssl@3)        # build libkeel_openssl.a
#   make test OPENSSL_DIR=$(brew --prefix openssl@3)   # relocation/link smoke
#   make e2e  OPENSSL_DIR=$(brew --prefix openssl@3)   # both-axes ASan+UBSan e2e
#
# Link into a consumer (order matters — adapter before core, OpenSSL last):
#   cc app.c -L<keel> -lkeel -L. -lkeel_openssl \
#            -L$(OPENSSL_DIR)/lib -lssl -lcrypto
#
# Include from a consumer:
#   cc -I<keel>/include -Iintegrations/openssl ...   #  <keel_tls_openssl.h>

KEEL_ROOT ?= ../..
CC        ?= cc

# OPENSSL_DIR: a system prefix (include/ + lib/), e.g. Homebrew openssl@3.
# If unset, the compiler's default search paths are used.
OPENSSL_INC = $(if $(OPENSSL_DIR),-I$(OPENSSL_DIR)/include,)
OPENSSL_LDPATH = $(if $(OPENSSL_DIR),-L$(OPENSSL_DIR)/lib,)

# -I$(KEEL_ROOT)/src: the adapter includes the internal socket seam ("socket.h").
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
          -fstack-protector-strong
CFLAGS += -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I. $(OPENSSL_INC)

LIB = libkeel_openssl.a
OBJ = tls_openssl.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

tls_openssl.o: tls_openssl.c keel_tls_openssl.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Link smoke: confirms the adapter archives + resolves against core libkeel.a and
# OpenSSL. Requires the core lib to be built first (repo root `make`).
test: $(LIB)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o link_smoke link_smoke.c \
	  -L. -lkeel_openssl -L$(KEEL_ROOT) -lkeel \
	  $(OPENSSL_LDPATH) -lssl -lcrypto
	./link_smoke
	@echo "integrations/openssl: link smoke OK"

# End-to-end: both transport axes (socket-BIO + memory-BIO), ALPN, mTLS peer_cert,
# clean-shutdown at_eof. Built with ASan+UBSan; links the adapter + core + OpenSSL.
E2E_CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
             -fsanitize=address,undefined -fno-sanitize-recover=all \
             -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I. $(OPENSSL_INC)
e2e: $(OBJ)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(E2E_CFLAGS) -o e2e/tls_e2e e2e/tls_e2e.c tls_openssl.c \
	  -L$(KEEL_ROOT) -lkeel $(OPENSSL_LDPATH) -lssl -lcrypto
	./e2e/tls_e2e

clean:
	rm -f $(LIB) $(OBJ) tls_openssl.d link_smoke e2e/tls_e2e
	rm -rf e2e/tls_e2e.dSYM link_smoke.dSYM

.PHONY: all test e2e clean
