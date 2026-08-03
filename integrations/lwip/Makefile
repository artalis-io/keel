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

# ── Phase 9 spike: raw (tcp_*) callback API, NO_SYS=1, over loopback ──────────
# Proves the raw+loopback+NO_SYS mainloop works in-process (no tap, no root) —
# the foundation a KEEL raw/completion provider would build on. This is a
# STANDALONE spike: it does NOT link libkeel, only a NO_SYS=1 lwIP built from
# lwipopts_raw.h.
#
# The trick to make the raw build pick up lwipopts_raw.h (lwIP's opt.h does an
# unconditional `#include "lwipopts.h"`): compile in a scratch dir (raw_build/)
# that contains a one-line lwipopts.h forwarding to ../lwipopts_raw.h, and put
# that dir FIRST on the include path — so it wins over the NO_SYS=0 lwipopts.h
# in `.`. Raw build uses ONLY src/core + src/core/ipv4 (+ netif/loopif when it
# exists as a .c; in STABLE-2_2_0 the loopif lives in core/netif.c under
# LWIP_HAVE_LOOPIF, so no extra netif file is needed). No src/api, no sys_arch.
# A self-contained arch port for NO_SYS=1: we deliberately do NOT use lwIP's
# contrib unix port (that's the threaded sys_arch — it defines LWIP_RAND() to
# lwip_port_rand() and drags in sys_arch.c). NO_SYS=1 needs only cc.h (compiler
# abstraction: byte order + printf format defines) and a trivial sys_arch.h.
RAW_BUILD  = raw_build
RAW_SRCS   = $(wildcard $(LWIP_DIR)/src/core/*.c) \
             $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c)
RAW_INC    = -I$(RAW_BUILD) -I$(LWIP_DIR)/src/include

$(RAW_BUILD)/lwipopts.h:
	@mkdir -p $(RAW_BUILD)/arch
	@echo '#include "../lwipopts_raw.h"' > $@

# Minimal compiler-abstraction header (cc.h) — no contrib port needed.
$(RAW_BUILD)/arch/cc.h: $(RAW_BUILD)/lwipopts.h
	@printf '%s\n' \
	  '#ifndef LWIP_ARCH_CC_H' \
	  '#define LWIP_ARCH_CC_H' \
	  '#include <sys/time.h>' \
	  '#define LWIP_TIMEVAL_PRIVATE 0' \
	  '#define LWIP_ERRNO_STDINCLUDE 1' \
	  '#include <endian.h>' \
	  '#ifndef BYTE_ORDER' \
	  '#define BYTE_ORDER LITTLE_ENDIAN' \
	  '#endif' \
	  '#include <inttypes.h>' \
	  '#define X8_F  "02" PRIx8' \
	  '#define U16_F PRIu16' \
	  '#define S16_F PRId16' \
	  '#define X16_F PRIx16' \
	  '#define U32_F PRIu32' \
	  '#define S32_F PRId32' \
	  '#define X32_F PRIx32' \
	  '#define SZT_F "zu"' \
	  '#endif' > $@

# Trivial sys_arch.h — NO_SYS=1 uses none of the sys_* prototypes, but lwIP still
# #includes this header. Empty guard is enough.
$(RAW_BUILD)/arch/sys_arch.h: $(RAW_BUILD)/lwipopts.h
	@printf '%s\n' \
	  '#ifndef LWIP_ARCH_SYS_ARCH_H' \
	  '#define LWIP_ARCH_SYS_ARCH_H' \
	  '#endif' > $@

RAW_ARCH = $(RAW_BUILD)/arch/cc.h $(RAW_BUILD)/arch/sys_arch.h

liblwip_raw.a: $(RAW_BUILD)/lwipopts.h $(RAW_ARCH) $(RAW_SRCS)
	@echo "building NO_SYS=1 lwIP ($(words $(RAW_SRCS)) files) ..."
	@$(CC) -w -std=c11 -O2 -D_DEFAULT_SOURCE $(RAW_INC) -c $(RAW_SRCS)
	@$(AR) rcs $@ $(notdir $(RAW_SRCS:.c=.o))

raw-spike: liblwip_raw.a $(RAW_BUILD)/lwipopts.h $(RAW_ARCH)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE $(RAW_INC) \
	  -o raw_loopback_spike raw_loopback_spike.c liblwip_raw.a
	./raw_loopback_spike
	@echo "integrations/lwip: raw+loopback+NO_SYS spike OK"

# ── Phase 9 P9-1: lwIP raw COMPLETION backend — build a lwipraw libkeel + tick test ──
# The completion counterpart of `loopback` (which rides a STOCK libkeel over the
# readiness lwIP provider). This backend is a COMPLETION build: it needs a libkeel BUILT
# with BACKEND=lwipraw (completion_driver.c linked, io_engine.c stub not) — see
# docs/phase9_lwip_raw_design.md "it's a completion BACKEND, not a runtime drop-in".
#
# Steps:
#   1. Build the NO_SYS=1 lwIP archive (liblwip_raw.a) + the raw_build scratch dir
#      (lwipopts.h forwarder + arch/cc.h) — reusing the raw-spike mechanism.
#   2. Build libkeel with BACKEND=lwipraw (root Makefile), passing LWIP_DIR +
#      LWIPRAW_BUILD so event_lwip_raw.c finds lwIP's NO_SYS=1 headers + our lwipopts.
#   3. Compile + link + run raw_tick_test against that lwipraw libkeel + liblwip_raw.a.
#
# The test proves the completion loop drives lwIP (sys_check_timeouts + netif_poll each
# tick) with no server — the P9-1 bar. Existing readiness targets are untouched (they
# build their own liblwip.a from lwipopts.h / NO_SYS=0 and a STOCK libkeel).
LWIPRAW_KEEL_LIB = $(KEEL_ROOT)/libkeel.a
# raw_tick_test uses the public API + one internal seam (event_caps.h, the
# compatibility negotiation) — hence -Isrc alongside -I. for keel_lwip_raw.h.
loopback-raw: liblwip_raw.a $(RAW_BUILD)/lwipopts.h $(RAW_ARCH)
	@echo "building lwipraw libkeel (BACKEND=lwipraw) ..."
	$(MAKE) -C $(KEEL_ROOT) clean
	$(MAKE) -C $(KEEL_ROOT) BACKEND=lwipraw LWIP_DIR=$(abspath $(LWIP_DIR)) \
	  LWIPRAW_BUILD=$(abspath $(RAW_BUILD))
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_tick_test raw_tick_test.c \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	./raw_tick_test
	@echo "integrations/lwip: P9-1 lwIP raw completion tick test OK"

clean:
	rm -f *.o *.a lwip_loopback_test raw_loopback_spike raw_tick_test
	rm -rf $(RAW_BUILD)

.PHONY: check loopback loopback-tls raw-spike loopback-raw clean
