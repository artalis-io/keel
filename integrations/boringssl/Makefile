# integrations/boringssl — build the SHARED OpenSSL/BoringSSL KlTls adapter
# against a BoringSSL prefix.
#
# There is no BoringSSL-specific source: this compiles ../openssl/tls_openssl.c
# (the one shared adapter) against BoringSSL's headers/libs, proving the identical
# adapter works against BoringSSL. The adapter's few divergences are gated by
# `#if defined(OPENSSL_IS_BORINGSSL)` (defined by BoringSSL's <openssl/base.h>).
#
# Bring your own BoringSSL. It is NOT on Homebrew and NOT vendored/downloaded here.
# Build it from source (needs Go + cmake + ninja), then point BORINGSSL_DIR at a
# prefix that has:
#   $(BORINGSSL_DIR)/include/openssl/*.h
#   $(BORINGSSL_DIR)/lib/libssl.a   $(BORINGSSL_DIR)/lib/libcrypto.a
# (or the in-tree build layout — override BORINGSSL_LIBDIR accordingly).
#
# Usage:
#   make BORINGSSL_DIR=/path/to/boringssl/install          # build libkeel_boringssl.a
#   make test BORINGSSL_DIR=/path/to/boringssl/install     # link smoke
#
# See README.md for how to obtain/build BoringSSL.

KEEL_ROOT     ?= ../..
CC            ?= cc
ADAPTER_SRC   := ../openssl/tls_openssl.c

ifndef BORINGSSL_DIR
$(error set BORINGSSL_DIR to a BoringSSL prefix (include/ + lib/) — see README.md)
endif

BORINGSSL_INC    := -I$(BORINGSSL_DIR)/include
BORINGSSL_LIBDIR ?= $(BORINGSSL_DIR)/lib
# BoringSSL is C++: its static libs pull in the C++ runtime, so consumers must add it
# to the link line (unlike OpenSSL/LibreSSL, which are C). Override for your toolchain
# (e.g. -lc++ with libc++ instead of libstdc++).
BORINGSSL_EXTRA_LIBS ?= -lstdc++

# -I$(KEEL_ROOT)/src: the adapter includes the internal socket seam ("socket.h").
# -I../openssl: the adapter's own public header keel_tls_openssl.h.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
          -fstack-protector-strong
CFLAGS += -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I../openssl -I. $(BORINGSSL_INC)

LIB = libkeel_boringssl.a
OBJ = tls_boringssl.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

# Compile the SHARED adapter source (no copy) into a boringssl-named object.
tls_boringssl.o: $(ADAPTER_SRC) ../openssl/keel_tls_openssl.h keel_tls_boringssl.h
	$(CC) $(CFLAGS) -c -o $@ $(ADAPTER_SRC)

test: $(LIB)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o link_smoke link_smoke.c \
	  -L. -lkeel_boringssl -L$(KEEL_ROOT) -lkeel \
	  -L$(BORINGSSL_LIBDIR) -lssl -lcrypto -lpthread $(BORINGSSL_EXTRA_LIBS)
	./link_smoke
	@echo "integrations/boringssl: link smoke OK"

# Full both-axes end-to-end handshake + hardening suite (reuses the OpenSSL e2e —
# pure OpenSSL API). BoringSSL is C++, so add BORINGSSL_EXTRA_LIBS (-lstdc++).
E2E_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror \
              -fsanitize=address,undefined -fno-sanitize-recover=all \
              -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -I../openssl $(BORINGSSL_INC)
e2e:
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(E2E_CFLAGS) -o e2e_boringssl ../openssl/e2e/tls_e2e.c $(ADAPTER_SRC) \
	  -L$(KEEL_ROOT) -lkeel -L$(BORINGSSL_LIBDIR) -lssl -lcrypto -lpthread $(BORINGSSL_EXTRA_LIBS)
	./e2e_boringssl

clean:
	rm -f $(LIB) $(OBJ) link_smoke e2e_boringssl

.PHONY: all test e2e clean
