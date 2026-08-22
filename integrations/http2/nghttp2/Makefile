# integrations/http2/nghttp2 — standalone build of the nghttp2 HTTP/2 session adapters.
#
# Produces libkeel_http2_nghttp2.a (client + server KlHttp2*Session vtables over
# nghttp2). Bring-your-own nghttp2 (>= 1.57 for the size_t "*2" API); nothing is
# vendored or downloaded. The core `make` / `make test` never build this.
#
# Usage (from this dir or via the repo root's `make integration-nghttp2`):
#   make NGHTTP2_DIR=$(brew --prefix nghttp2)         # via pkg-config if unset
#   make test NGHTTP2_DIR=...                          # client<->server roundtrip
#
# Link into a consumer:
#   cc app.c -I<keel>/include -Iintegrations/http2/nghttp2 \
#            -L<keel> -lkeel -Lintegrations/http2/nghttp2 -lkeel_http2_nghttp2 \
#            $(pkg-config --libs libnghttp2)

KEEL_ROOT ?= ../../..
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
# _DEFAULT_SOURCE: e2e_socket.c uses nanosleep(), which needs a feature-test
# macro under -std=c11 on glibc (Apple libc exposes it without one).
CFLAGS += -D_DEFAULT_SOURCE -I$(KEEL_ROOT)/include -I. $(NGHTTP2_CFLAGS)

LIB = libkeel_http2_nghttp2.a
OBJ = http2_nghttp2_client.o http2_nghttp2_server.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

http2_nghttp2_client.o: http2_nghttp2_client.c keel_http2_nghttp2.h
	$(CC) $(CFLAGS) -c -o $@ $<
http2_nghttp2_server.o: http2_nghttp2_server.c keel_http2_nghttp2.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Functional roundtrip: pairs the client + server sessions in memory (no sockets,
# no KEEL driver), feeding each other's on_send output through recv, and asserts a
# GET request + 200 response with body survive real nghttp2 framing end to end.
test: $(LIB) e2e
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/test_roundtrip tests/test_roundtrip.c \
	  -L. -lkeel_http2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS)
	./tests/test_roundtrip
	@echo "integrations/http2/nghttp2: roundtrip OK"

# End-to-end over a real loopback socket + KEEL event loop: KEEL nghttp2 client
# (kl_http2_client_connect, cleartext h2c) <-> KEEL nghttp2 server (KlHttpServerConfig.h2,
# h2c prior-knowledge). Exercises both adapters through the real driver paths.
e2e: $(LIB)
	@test -f $(KEEL_ROOT)/libkeel.a || { echo "build core first: (cd $(KEEL_ROOT) && make)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/e2e_socket tests/e2e_socket.c \
	  -L. -lkeel_http2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS) -lpthread
	./tests/e2e_socket
	@echo "integrations/http2/nghttp2: e2e (real socket) OK"

# Third-party interop: run the KEEL nghttp2 server (h2c prior-knowledge) and hit
# it with curl's HTTP/2 client. Requires a curl built with HTTP2 (curl -V).
interop-curl: $(LIB)
	@command -v curl >/dev/null || { echo "curl not found"; exit 1; }
	@curl -V | grep -q HTTP2 || { echo "curl lacks HTTP2 support"; exit 1; }
	$(CC) $(CFLAGS) -o tests/interop_server tests/interop_server.c \
	  -L. -lkeel_http2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS) -lpthread
	./tests/interop_server & echo $$! > /tmp/keel_h2_interop.pid; sleep 1; \
	  out=$$(curl -s --http2-prior-knowledge http://127.0.0.1:18478/hello); rc=$$?; \
	  kill $$(cat /tmp/keel_h2_interop.pid) 2>/dev/null; rm -f /tmp/keel_h2_interop.pid; \
	  echo "curl body: $$out"; \
	  test $$rc -eq 0 || { echo "curl failed rc=$$rc"; exit 1; }; \
	  echo "$$out" | grep -q "hello h2 e2e" || { echo "body mismatch"; exit 1; }; \
	  echo "integrations/http2/nghttp2: curl --http2-prior-knowledge interop OK"

# HTTP/2 protocol conformance: run h2spec (https://github.com/summerwind/h2spec)
# against the KEEL nghttp2 server (h2c). Reports pass/fail; informational — the
# residual failures are shared with nghttp2's own reference server (nghttpd), see
# README. Requires h2spec on PATH (brew install h2spec).
INTEROP_PORT ?= 18478
h2spec: $(LIB)
	@command -v h2spec >/dev/null || { echo "h2spec not found (brew install h2spec)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/interop_server tests/interop_server.c \
	  -L. -lkeel_http2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS) -lpthread
	./tests/interop_server & echo $$! > /tmp/keel_h2_h2spec.pid; sleep 1; \
	  h2spec -h 127.0.0.1 -p $(INTEROP_PORT); rc=$$?; \
	  kill $$(cat /tmp/keel_h2_h2spec.pid) 2>/dev/null; rm -f /tmp/keel_h2_h2spec.pid; \
	  echo "integrations/http2/nghttp2: h2spec run complete (see summary above)"; \
	  exit $$rc

# Concurrency/load interop: h2load (nghttp2's load tool) drives many multiplexed
# streams at the KEEL server. Requires h2load on PATH (brew install nghttp2).
h2load: $(LIB)
	@command -v h2load >/dev/null || { echo "h2load not found (brew install nghttp2)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/interop_server tests/interop_server.c \
	  -L. -lkeel_http2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS) -lpthread
	./tests/interop_server & echo $$! > /tmp/keel_h2_load.pid; sleep 1; \
	  h2load -n 10000 -c 8 -m 20 http://127.0.0.1:$(INTEROP_PORT)/hello; rc=$$?; \
	  kill $$(cat /tmp/keel_h2_load.pid) 2>/dev/null; rm -f /tmp/keel_h2_load.pid; \
	  exit $$rc

# Last interop quadrant: the KEEL nghttp2 *client* against a third-party server
# (nghttpd, h2c). Requires nghttpd on PATH (brew install nghttp2).
interop-nghttpd: $(LIB)
	@command -v nghttpd >/dev/null || { echo "nghttpd not found (brew install nghttp2)"; exit 1; }
	$(CC) $(CFLAGS) -o tests/client_probe tests/client_probe.c \
	  -L. -lkeel_http2_nghttp2 -L$(KEEL_ROOT) -lkeel $(NGHTTP2_LIBS) -lpthread
	@d=$$(mktemp -d); echo '{"ok":true}' > $$d/index.html; \
	  nghttpd --no-tls -d $$d 18479 & echo $$! > /tmp/keel_ngd.pid; sleep 1; \
	  ./tests/client_probe 127.0.0.1 18479 /; rc=$$?; \
	  kill $$(cat /tmp/keel_ngd.pid) 2>/dev/null; rm -rf $$d /tmp/keel_ngd.pid; \
	  test $$rc -eq 0 && echo "integrations/http2/nghttp2: KEEL client <-> nghttpd interop OK"; \
	  exit $$rc

# End-to-end ALPN over real TLS: a Keel server advertising {h2,http/1.1}
# (mbedTLS ALPN) + nghttp2 h2, verified with external clients. Proves the two
# server-side matrix rows: external client --ALPN h2--> h2 adapter, and
# --ALPN http/1.1--> http/1.1 adapter, both on one shared handler. Requires BOTH
# NGHTTP2_DIR and MBEDTLS_DIR, plus openssl + curl (BYO, out of CI).
alpn-interop: $(LIB)
	@test -n "$(MBEDTLS_DIR)" || { echo "set MBEDTLS_DIR (and NGHTTP2_DIR)"; exit 1; }
	@command -v openssl >/dev/null || { echo "openssl not found"; exit 1; }
	@command -v curl >/dev/null && curl -V | grep -q HTTP2 || { echo "curl with HTTP2 not found"; exit 1; }
	$(MAKE) -C ../../tls/mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR)
	$(CC) $(CFLAGS) -I../../tls/mbedtls -I$(MBEDTLS_DIR)/include -o tests/alpn_server tests/alpn_server.c \
	  -L. -lkeel_http2_nghttp2 -L../../tls/mbedtls -lkeel_mbedtls -L$(KEEL_ROOT) -lkeel \
	  $(NGHTTP2_LIBS) -L$(MBEDTLS_DIR)/lib -L$(MBEDTLS_DIR)/library \
	  -lmbedtls -lmbedx509 -lmbedcrypto -lpthread
	$(CC) $(CFLAGS) -I../../tls/mbedtls -I$(MBEDTLS_DIR)/include -o tests/alpn_client tests/alpn_client.c \
	  -L. -lkeel_http2_nghttp2 -L../../tls/mbedtls -lkeel_mbedtls -L$(KEEL_ROOT) -lkeel \
	  $(NGHTTP2_LIBS) -L$(MBEDTLS_DIR)/lib -L$(MBEDTLS_DIR)/library \
	  -lmbedtls -lmbedx509 -lmbedcrypto -lpthread
	./tests/alpn_server & echo $$! >/tmp/keel_alpn.pid; sleep 1; \
	  ha=$$(echo | openssl s_client -alpn h2 -connect 127.0.0.1:18443 2>/dev/null | grep -i "ALPN protocol"); \
	  hb=$$(echo | openssl s_client -alpn http/1.1 -connect 127.0.0.1:18443 2>/dev/null | grep -i "ALPN protocol"); \
	  c2=$$(curl -s -k --http2 https://127.0.0.1:18443/ 2>/dev/null); \
	  c1=$$(curl -s -k --http1.1 https://127.0.0.1:18443/ 2>/dev/null); \
	  kc=$$(./tests/alpn_client https://127.0.0.1:18443 2>/dev/null); kcrc=$$?; \
	  kill $$(cat /tmp/keel_alpn.pid) 2>/dev/null; rm -f /tmp/keel_alpn.pid; \
	  echo "openssl -alpn h2       -> $$ha"; \
	  echo "openssl -alpn http/1.1 -> $$hb"; \
	  echo "curl --http2   -> $$c2"; \
	  echo "curl --http1.1 -> $$c1"; \
	  echo "keel client    -> $$kc"; \
	  echo "$$ha" | grep -qi "ALPN protocol: h2"       || { echo "FAIL: server did not select h2";       exit 1; }; \
	  echo "$$hb" | grep -qi "ALPN protocol: http/1.1" || { echo "FAIL: server did not select http/1.1"; exit 1; }; \
	  echo "$$c2" | grep -q '"http":2' || { echo "FAIL: curl --http2 not served by h2 adapter";   exit 1; }; \
	  echo "$$c1" | grep -q '"http":1' || { echo "FAIL: curl --http1.1 not served by h1 adapter"; exit 1; }; \
	  test $$kcrc -eq 0 || { echo "FAIL: KEEL client did not complete h2 over TLS"; exit 1; }; \
	  echo "integrations/http2/nghttp2: ALPN interop OK (external + KEEL client, h2 + http/1.1, one shared handler)"

clean:
	rm -f $(LIB) $(OBJ) http2_nghttp2_client.d http2_nghttp2_server.d \
	      tests/test_roundtrip tests/e2e_socket tests/interop_server tests/client_probe \
	      tests/alpn_server tests/alpn_client
	rm -rf tests/*.dSYM

.PHONY: all test e2e interop-curl h2spec h2load interop-nghttpd alpn-interop clean
