# integrations/ — umbrella build for the optional adapters.
#
# Delegates to each adapter's own Makefile. An adapter is built only when its
# BYO library variable is set; otherwise it is skipped with a notice, so
# `make all` / `make test` here are safe with only some libraries available.
#
# Prefer driving these from the repo root (`make integrations`, etc.), which
# passes MBEDTLS_DIR / NGHTTP2_DIR through. Each leaf Makefile derives its own
# default KEEL_ROOT depth for its nesting level, resolving to the repo root under
# `-C`, so no override is needed.

all: mbedtls openssl libressl nghttp2 lwip
test: mbedtls-test openssl-test libressl-test nghttp2-test

# lwIP: build-gate only (compile the providers against your lwIP). A running Keel
# server on lwIP needs a PLATFORM=lwip build of libkeel — see platform/lwip/README.md.
lwip:
ifndef LWIP_DIR
	@echo "integrations: skipping lwip (set LWIP_DIR to build-gate)"
else
	$(MAKE) -C platform/lwip check LWIP_DIR=$(LWIP_DIR)
endif

mbedtls:
ifndef MBEDTLS_DIR
	@echo "integrations: skipping mbedtls (set MBEDTLS_DIR to build)"
else
	$(MAKE) -C tls/mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR)
endif

mbedtls-test:
ifndef MBEDTLS_DIR
	@echo "integrations: skipping mbedtls test (set MBEDTLS_DIR)"
else
	$(MAKE) -C tls/mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR) test
endif

openssl:
ifndef OPENSSL_DIR
	@echo "integrations: skipping openssl (set OPENSSL_DIR to build)"
else
	$(MAKE) -C tls/openssl OPENSSL_DIR=$(OPENSSL_DIR)
endif

openssl-test:
ifndef OPENSSL_DIR
	@echo "integrations: skipping openssl test (set OPENSSL_DIR)"
else
	$(MAKE) -C tls/openssl OPENSSL_DIR=$(OPENSSL_DIR) test
endif

# BoringSSL: build-gate only (compile the SHARED openssl adapter against BoringSSL).
boringssl:
ifndef BORINGSSL_DIR
	@echo "integrations: skipping boringssl (set BORINGSSL_DIR to build-gate)"
else
	$(MAKE) -C tls/boringssl BORINGSSL_DIR=$(BORINGSSL_DIR)
endif

# LibreSSL: compile the SHARED openssl adapter against LibreSSL (brew: keg-only).
libressl:
ifndef LIBRESSL_DIR
	@echo "integrations: skipping libressl (set LIBRESSL_DIR to build)"
else
	$(MAKE) -C tls/libressl LIBRESSL_DIR=$(LIBRESSL_DIR)
endif

libressl-test:
ifndef LIBRESSL_DIR
	@echo "integrations: skipping libressl test (set LIBRESSL_DIR)"
else
	$(MAKE) -C tls/libressl LIBRESSL_DIR=$(LIBRESSL_DIR) test
endif

nghttp2:
ifndef NGHTTP2_DIR
	@echo "integrations: skipping nghttp2 (set NGHTTP2_DIR to build)"
else
	$(MAKE) -C http2/nghttp2 NGHTTP2_DIR=$(NGHTTP2_DIR)
endif

nghttp2-test:
ifndef NGHTTP2_DIR
	@echo "integrations: skipping nghttp2 test (set NGHTTP2_DIR)"
else
	$(MAKE) -C http2/nghttp2 NGHTTP2_DIR=$(NGHTTP2_DIR) test
endif

clean:
	-$(MAKE) -C tls/mbedtls clean
	-$(MAKE) -C tls/openssl clean
	-$(MAKE) -C tls/boringssl clean 2>/dev/null || true
	-$(MAKE) -C tls/libressl clean 2>/dev/null || true
	-$(MAKE) -C http2/nghttp2 clean

.PHONY: all test clean mbedtls mbedtls-test openssl openssl-test boringssl \
        libressl libressl-test nghttp2 nghttp2-test lwip
