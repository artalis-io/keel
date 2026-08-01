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
check: socket_lwip.o event_lwip.o
	@echo "integrations/lwip: providers compile against $(LWIP_DIR) (public API sufficient)"

socket_lwip.o: socket_lwip.c keel_lwip.h lwipopts.h
	$(CC) $(CFLAGS) -c -o $@ $<
event_lwip.o: event_lwip.c keel_lwip.h lwipopts.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── loopback runtime test: a real Keel server + client over lwIP loopback ─────
# NOTE: requires KEEL_LIB to point at a libkeel.a built with -DKEEL_PLATFORM_LWIP
# (a PLATFORM=lwip build). A stock host libkeel does NOT work: it emits host-layout
# sockaddr / calls host getaddrinfo, which lwip_bind rejects (bind: EIO). See
# README.md ("The runtime boundary") + docs/lwip_platform_design.md. lwIP loopback
# itself is confirmed working; this target is the proof once a platform libkeel
# exists. Builds a minimal lwIP (core + IPv4 + api + unix sys port) into liblwip.a.
KEEL_LIB ?= $(KEEL_ROOT)/libkeel.a
LWIP_SRCS = $(wildcard $(LWIP_DIR)/src/core/*.c) \
            $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c) \
            $(wildcard $(LWIP_DIR)/src/api/*.c) \
            $(LWIP_DIR)/src/netif/ethernet.c \
            $(LWIP_DIR)/contrib/ports/unix/port/sys_arch.c
liblwip.a: $(LWIP_SRCS)
	@echo "building lwIP ($(words $(LWIP_SRCS)) files) ..."
	@$(CC) -w -std=c11 -O2 -I. $(LWIP_INC) -c $(LWIP_SRCS)
	@$(AR) rcs $@ $(notdir $(LWIP_SRCS:.c=.o))

loopback: socket_lwip.o event_lwip.o liblwip.a
	@test -f "$(KEEL_LIB)" || { echo "set KEEL_LIB to a KEEL_PLATFORM_LWIP-built libkeel.a (see README)"; exit 1; }
	$(CC) $(CFLAGS) -o lwip_loopback_test lwip_loopback_test.c \
	  socket_lwip.o event_lwip.o liblwip.a "$(KEEL_LIB)" -lpthread
	./lwip_loopback_test
	@echo "integrations/lwip: loopback runtime test OK"

clean:
	rm -f *.o *.a lwip_loopback_test

.PHONY: check loopback clean
