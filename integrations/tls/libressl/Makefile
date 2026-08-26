# integrations/tls/libressl: build the SHARED OpenSSL/BoringSSL/LibreSSL KlTls adapter
# against a LibreSSL prefix.
#
# There is no LibreSSL-specific source: this compiles ../openssl/tls_openssl.c
# (the one shared adapter) against LibreSSL's headers/libs, proving the identical
# adapter works against LibreSSL. The adapter's few divergences are gated by
# `#if defined(LIBRESSL_VERSION_NUMBER)` (defined by LibreSSL's <openssl/opensslv.h>).
#
# Bring your own LibreSSL (Homebrew: `brew install libressl`, prefix
# $(brew --prefix libressl)). It is keg-only, so it is NOT on the default search
# path; pass LIBRESSL_DIR. Layout expected:
#   $(LIBRESSL_DIR)/include/openssl/*.h
#   $(LIBRESSL_DIR)/lib/libssl.a   $(LIBRESSL_DIR)/lib/libcrypto.a
#
# Usage:
#   make LIBRESSL_DIR=$(brew --prefix libressl)         # build libkeel_libressl.a
#   make test LIBRESSL_DIR=$(brew --prefix libressl)    # link smoke
#   make e2e  LIBRESSL_DIR=$(brew --prefix libressl)    # both-axes handshake (ASan+UBSan)
#
# See README.md.

KEEL_ROOT   ?= ../../..
CC          ?= cc
ADAPTER_SRC := ../openssl/tls_openssl.c

ifndef LIBRESSL_DIR
$(error set LIBRESSL_DIR to a LibreSSL prefix (include/ + lib/), e.g. $$(brew --prefix libressl))
endif

LIBRESSL_INC    := -I$(LIBRESSL_DIR)/include
LIBRESSL_LIBDIR ?= $(LIBRESSL_DIR)/lib

# -I$(KEEL_ROOT)/src: the adapter includes the internal socket seam ("socket.h").
# -I../openssl: the adapter's own public header keel_tls_openssl.h.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
          -fstack-protector-strong
CFLAGS += -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I../openssl -I. $(LIBRESSL_INC)

LIB = libkeel_libressl.a
OBJ = tls_libressl.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

# Compile the SHARED adapter source (no copy) into a libressl-named object.
tls_libressl.o: $(ADAPTER_SRC) ../openssl/keel_tls_openssl.h keel_tls_libressl.h
	$(CC) $(CFLAGS) -c -o $@ $(ADAPTER_SRC)

test: $(LIB)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/link_smoke tests/link_smoke.c \
	  -L. -lkeel_libressl -L$(KEEL_ROOT) -lkeel \
	  -L$(LIBRESSL_LIBDIR) -lssl -lcrypto -lpthread
	./tests/link_smoke
	@echo "integrations/tls/libressl: link smoke OK"

# Full both-axes end-to-end handshake (reuses the OpenSSL e2e, pure OpenSSL API).
E2E_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
              -fsanitize=address,undefined -fno-sanitize-recover=all \
              -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I../openssl $(LIBRESSL_INC)
e2e:
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(E2E_CFLAGS) -o tests/e2e_libressl ../openssl/tests/tls_e2e.c $(ADAPTER_SRC) \
	  -L$(KEEL_ROOT) -lkeel -L$(LIBRESSL_LIBDIR) -lssl -lcrypto -lpthread
	./tests/e2e_libressl

clean:
	rm -f $(LIB) $(OBJ) tests/link_smoke tests/e2e_libressl
	rm -rf tests/*.dSYM

.PHONY: all test e2e clean
