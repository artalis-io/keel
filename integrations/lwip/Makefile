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

# ── Phase 9 / RC-3: lwIP raw COMPLETION backend as a RUNTIME PROVIDER on a STOCK libkeel ──
# The completion counterpart of `loopback` (which rides a STOCK libkeel over the readiness
# lwIP provider) — and now built the SAME way. RC-3 retired BACKEND=lwipraw: event_lwip_raw.c
# is a pure runtime KlEventProvider (no kl_event_*_builtin / kl_comp_ops_builtin), so it drops
# onto a STOCK libkeel whose always-linked completion driver + dispatch (completion_driver.c /
# completion_dispatch.c, present since RC-1) reach it via loop->ops->completion. See
# docs/completion_axis_runtime_design.md (RC-3) + docs/phase9_lwip_raw_design.md.
#
# Steps:
#   1. Build the NO_SYS=1 lwIP archive (liblwip_raw.a) + the raw_build scratch dir
#      (lwipopts.h forwarder + arch/cc.h) — reusing the raw-spike mechanism.
#   2. Build a STOCK libkeel (the DEFAULT readiness backend — epoll/kqueue; always carries
#      the completion driver + dispatch from RC-1). NO BACKEND= override.
#   3. Compile the lwip-raw PROVIDER objects (event_lwip_raw.o + lwip_raw_glue.o) against
#      the raw lwIP includes + the internal src/ seams.
#   4. Link + run raw_tick_test against those objects + the stock libkeel + liblwip_raw.a.
#
# raw_tick_test injects the provider at runtime: kl_event_ctx_init_ex(.., kl_event_provider_
# lwip_raw()) for P9-1, and KlConfig.event_provider = kl_event_provider_lwip_raw() for the
# server cases (P9-2/3/4) — the server auto-wires the paired overlapped socket provider from
# the loop's native_provider(). Existing readiness targets (loopback/loopback-tls/raw-spike)
# are untouched.
LWIPRAW_KEEL_LIB = $(KEEL_ROOT)/libkeel.a
# The lwip-raw provider objects. event_lwip_raw.c is OUR code (no lwIP headers) — build it
# -Wall -Wextra -Werror against the internal src/ seams (completion.h/socket.h) + keel_lwip_raw.h.
# lwip_raw_glue.c is the lwIP-only TU — relaxed warnings (-w) against the NO_SYS=1 lwIP headers.
# event_lwip_raw.c is OUR code (no lwIP headers). lwip_raw_glue.c + lwip_raw_testclient.c are
# the lwIP-only TUs — relaxed warnings (-w) against the NO_SYS=1 lwIP headers. The glue also
# includes keel/allocator.h (KlAllocator only — no socket/net type, so the header seam holds),
# so it needs -I$(KEEL_ROOT)/include.
event_lwip_raw.o: event_lwip_raw.c keel_lwip_raw.h lwip_raw_glue.h
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fstack-protector-strong -D_DEFAULT_SOURCE \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -c -o $@ $<

lwip_raw_glue.o: lwip_raw_glue.c lwip_raw_glue.h $(RAW_BUILD)/lwipopts.h $(RAW_ARCH)
	$(CC) -std=c11 -O2 -w -D_DEFAULT_SOURCE $(RAW_INC) -Iintegrations/lwip -I. \
	  -I$(KEEL_ROOT)/include -c -o $@ $<

lwip_raw_testclient.o: lwip_raw_testclient.c lwip_raw_testclient.h $(RAW_BUILD)/lwipopts.h $(RAW_ARCH)
	$(CC) -std=c11 -O2 -w -D_DEFAULT_SOURCE $(RAW_INC) -Iintegrations/lwip -I. -c -o $@ $<

# raw_tick_test (P9-1..P9-4) + raw_recv_test (Stage A: receive/concurrency/saturation/context)
# use the public API + two internal seams (event_caps.h for the compatibility negotiation; the
# glue/testclient headers) — hence -Isrc alongside -I. for keel_lwip_raw.h.
LWIPRAW_OBJS = event_lwip_raw.o lwip_raw_glue.o lwip_raw_testclient.o
loopback-raw: $(LWIPRAW_OBJS) liblwip_raw.a
	@echo "building STOCK libkeel (default readiness backend + completion driver/dispatch) ..."
	$(MAKE) -C $(KEEL_ROOT)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_tick_test raw_tick_test.c \
	  $(LWIPRAW_OBJS) \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_recv_test raw_recv_test.c \
	  $(LWIPRAW_OBJS) \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	$(CC) -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_send_test raw_send_test.c \
	  $(LWIPRAW_OBJS) \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	./raw_tick_test
	./raw_recv_test
	./raw_send_test
	@echo "integrations/lwip: P9 lwIP raw completion tests OK (runtime injection on stock libkeel)"

# ASan+UBSan+LSan variant — the lifetime/close/cancel + Stage-A receive/context cases need it.
# Builds a STOCK DEBUG libkeel (make -C .. debug) + sanitizer-instrumented provider objects + tests.
LWIPRAW_ASAN = -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -D_DEFAULT_SOURCE
loopback-raw-asan: $(RAW_BUILD)/lwipopts.h $(RAW_ARCH) liblwip_raw.a
	@echo "building STOCK DEBUG libkeel (ASan+UBSan) ..."
	$(MAKE) -C $(KEEL_ROOT) debug
	$(CC) $(LWIPRAW_ASAN) -Wall -Wextra -Werror \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src -c -o event_lwip_raw.o event_lwip_raw.c
	$(CC) $(LWIPRAW_ASAN) -w $(RAW_INC) -Iintegrations/lwip -I. -I$(KEEL_ROOT)/include \
	  -c -o lwip_raw_glue.o lwip_raw_glue.c
	$(CC) $(LWIPRAW_ASAN) -w $(RAW_INC) -Iintegrations/lwip -I. -c -o lwip_raw_testclient.o \
	  lwip_raw_testclient.c
	$(CC) $(LWIPRAW_ASAN) -Wall -Wextra \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_tick_test raw_tick_test.c \
	  $(LWIPRAW_OBJS) \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	$(CC) $(LWIPRAW_ASAN) -Wall -Wextra \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_recv_test raw_recv_test.c \
	  $(LWIPRAW_OBJS) \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	$(CC) $(LWIPRAW_ASAN) -Wall -Wextra \
	  -I. -I$(KEEL_ROOT)/include -I$(KEEL_ROOT)/src $(RAW_INC) \
	  -o raw_send_test raw_send_test.c \
	  $(LWIPRAW_OBJS) \
	  "$(LWIPRAW_KEEL_LIB)" liblwip_raw.a -lpthread
	ASAN_OPTIONS=detect_leaks=1 ./raw_tick_test
	ASAN_OPTIONS=detect_leaks=1 ./raw_recv_test
	ASAN_OPTIONS=detect_leaks=1 ./raw_send_test
	@echo "integrations/lwip: P9 lwIP raw completion tests OK under ASan+UBSan+LSan"

clean:
	rm -f *.o *.a lwip_loopback_test raw_loopback_spike raw_tick_test raw_recv_test raw_send_test
	rm -rf $(RAW_BUILD)

.PHONY: check loopback loopback-tls raw-spike loopback-raw loopback-raw-asan clean
