# integrations/lwip — Keel on the lwIP TCP/IP stack (socket + event providers).
#
# Bring-your-own lwIP: NOT vendored. Point LWIP_DIR at an lwIP checkout that also
# carries contrib/ports/unix (the host arch/sys port), e.g.:
#     make check LWIP_DIR=/path/to/lwip                 # build-gate (no lwIP lib)
#     make test  LWIP_DIR=/path/to/lwip                 # loopback runtime test
#
# `check` compiles the providers against your lwIP + the public Keel headers — a
# clean compile proves Keel's public KlSocketProvider + KlEventProvider API is
# sufficient to author an lwIP platform using no internal Keel or host types.
# `test` builds a small lwIP (core + unix port) and runs a real Keel server +
# client over lwIP's loopback netif. lwIP is readiness-only (lwip_poll).

KEEL_ROOT ?= ../..
LWIP_DIR  ?= ../../../lwip
CC        ?= cc

LWIP_INC = -I$(LWIP_DIR)/src/include -I$(LWIP_DIR)/contrib/ports/unix/port/include
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -fstack-protector-strong
# KEEL_PLATFORM_LWIP routes keel/net.h to lwip/sockets.h instead of the host
# <sys/socket.h>, so a TU that also includes lwIP's headers has one consistent set
# of socket types/macros (no PF_INET6 / MSG_* redefinition clashes).
# -I. first so our lwipopts.h + keel_lwip.h win; then Keel public headers + lwIP.
CFLAGS  += -DKEEL_PLATFORM_LWIP=1 -I. -I$(KEEL_ROOT)/include $(LWIP_INC)
# _DEFAULT_SOURCE: defines SSIZE_MAX so lwIP's arch.h uses the host ssize_t
# (else it typedefs ssize_t=int, clashing with <sys/types.h>); also exposes
# usleep() used by the loopback test under -std=c11.
CFLAGS  += -D_DEFAULT_SOURCE

# ── build-gate: providers compile against public Keel API + lwIP headers ──────
check: socket_lwip.o event_lwip.o resolve_sync_lwip.o platform_wakeup_lwip.o
	@echo "integrations/lwip: providers compile against $(LWIP_DIR) (public API sufficient)"

# socket_lwip now carries the datagram data-plane too (KlDatagramOps folded onto the
# provider — Stage 3 of the A2 refactor), so it uses only public Keel headers; there
# is no separate udp_io_lwip link artifact.
socket_lwip.o: socket_lwip.c keel_lwip.h lwipopts.h
	$(CC) $(CFLAGS) -c -o $@ $<
event_lwip.o: event_lwip.c keel_lwip.h lwipopts.h
	$(CC) $(CFLAGS) -c -o $@ $<
# resolve_sync_lwip / platform_wakeup_lwip override internal Keel seams, so they need
# internal headers (-Isrc: resolve_sync.h, platform.h) alongside lwIP's netdb/sockets.
resolve_sync_lwip.o: resolve_sync_lwip.c lwipopts.h
	$(CC) $(CFLAGS) -I$(KEEL_ROOT)/src -c -o $@ $<
platform_wakeup_lwip.o: platform_wakeup_lwip.c lwipopts.h
	$(CC) $(CFLAGS) -I$(KEEL_ROOT)/src -c -o $@ $<

# ── loopback runtime test: a real Keel server + client over lwIP loopback ─────
# Runs against a STOCK libkeel.a — no -DKEEL_PLATFORM_LWIP library build. Since the
# KlSockAddr neutralization (docs/keel_sockaddr_design.md) core speaks KlSockAddr
# only; socket_lwip.c marshals KlSockAddr <-> lwIP sockaddr at the provider
# boundary and server bind parses the numeric address via kl_sockaddr_parse, so no
# host-layout sockaddr ever reaches lwIP. `make -C ../.. && make loopback LWIP_DIR=...`
# builds the stock lib, a minimal lwIP (core+IPv4+api+unix port), and runs the test.
KEEL_LIB ?= $(KEEL_ROOT)/libkeel.a
LWIP_SRCS = $(wildcard $(LWIP_DIR)/src/core/*.c) \
            $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c) \
            $(wildcard $(LWIP_DIR)/src/api/*.c) \
            $(LWIP_DIR)/src/netif/ethernet.c \
            $(LWIP_DIR)/contrib/ports/unix/port/sys_arch.c
liblwip.a: $(LWIP_SRCS)
	@echo "building lwIP ($(words $(LWIP_SRCS)) files) ..."
	@$(CC) -w -std=c11 -O2 -D_GNU_SOURCE -I. $(LWIP_INC) -c $(LWIP_SRCS)
	@$(AR) rcs $@ $(notdir $(LWIP_SRCS:.c=.o))

loopback: socket_lwip.o event_lwip.o resolve_sync_lwip.o platform_wakeup_lwip.o liblwip.a
	@test -f "$(KEEL_LIB)" || { echo "build the stock lib first: make -C $(KEEL_ROOT)"; exit 1; }
	$(CC) $(CFLAGS) -I$(KEEL_ROOT)/src -o lwip_loopback_test lwip_loopback_test.c \
	  socket_lwip.o event_lwip.o resolve_sync_lwip.o platform_wakeup_lwip.o \
	  liblwip.a "$(KEEL_LIB)" -lpthread
	./lwip_loopback_test
	@echo "integrations/lwip: loopback runtime test OK (stock libkeel + lwIP provider)"

# ── optional HTTPS phase: TLS over lwIP (mbedTLS BIO → lwIP socket provider) ───
# Builds the mbedTLS adapter (../mbedtls) and links it into the loopback with
# -DLWT_TLS, so the test adds a genuine TLS handshake + request over lwIP — proving
# the mbedTLS socket-BIO routed through kl_socket_provider_lwip() (no lwIP-specific
# TLS code). BYO mbedTLS via MBEDTLS_DIR (a source tree or a system prefix):
#     make loopback-tls LWIP_DIR=/path/to/lwip MBEDTLS_DIR=/path/to/mbedtls
MBEDTLS_DIR ?=
MBEDTLS_INC  = $(if $(MBEDTLS_DIR),-I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library,)
MBEDTLS_LIB  = $(if $(MBEDTLS_DIR),-L$(MBEDTLS_DIR)/library -L$(MBEDTLS_DIR)/lib,) \
               -lmbedtls -lmbedx509 -lmbedcrypto

loopback-tls: socket_lwip.o event_lwip.o resolve_sync_lwip.o platform_wakeup_lwip.o liblwip.a
	@test -f "$(KEEL_LIB)" || { echo "build the stock lib first: make -C $(KEEL_ROOT)"; exit 1; }
	$(MAKE) -C ../mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR)
	$(CC) $(CFLAGS) -DLWT_TLS -I../mbedtls $(MBEDTLS_INC) -I$(KEEL_ROOT)/src \
	  -o lwip_loopback_test lwip_loopback_test.c \
	  socket_lwip.o event_lwip.o resolve_sync_lwip.o platform_wakeup_lwip.o \
	  ../mbedtls/libkeel_mbedtls.a liblwip.a "$(KEEL_LIB)" $(MBEDTLS_LIB) -lpthread
	./lwip_loopback_test
	@echo "integrations/lwip: loopback + HTTPS runtime test OK (stock libkeel + lwIP + mbedTLS)"

clean:
	rm -f *.o *.a lwip_loopback_test

.PHONY: check loopback loopback-tls clean
