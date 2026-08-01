# integrations/ — umbrella build for the optional adapters.
#
# Delegates to each adapter's own Makefile. An adapter is built only when its
# BYO library variable is set; otherwise it is skipped with a notice, so
# `make all` / `make test` here are safe with only some libraries available.
#
# Prefer driving these from the repo root (`make integrations`, etc.), which
# passes MBEDTLS_DIR / NGHTTP2_DIR through. Each leaf Makefile's default
# KEEL_ROOT=../.. resolves to the repo root under `-C`, so no override is needed.

all: mbedtls nghttp2 lwip
test: mbedtls-test nghttp2-test

# lwIP: build-gate only (compile the providers against your lwIP). A running Keel
# server on lwIP needs a PLATFORM=lwip build of libkeel — see lwip/README.md.
lwip:
ifndef LWIP_DIR
	@echo "integrations: skipping lwip (set LWIP_DIR to build-gate)"
else
	$(MAKE) -C lwip check LWIP_DIR=$(LWIP_DIR)
endif

mbedtls:
ifndef MBEDTLS_DIR
	@echo "integrations: skipping mbedtls (set MBEDTLS_DIR to build)"
else
	$(MAKE) -C mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR)
endif

mbedtls-test:
ifndef MBEDTLS_DIR
	@echo "integrations: skipping mbedtls test (set MBEDTLS_DIR)"
else
	$(MAKE) -C mbedtls MBEDTLS_DIR=$(MBEDTLS_DIR) test
endif

nghttp2:
ifndef NGHTTP2_DIR
	@echo "integrations: skipping nghttp2 (set NGHTTP2_DIR to build)"
else
	$(MAKE) -C nghttp2 NGHTTP2_DIR=$(NGHTTP2_DIR)
endif

nghttp2-test:
ifndef NGHTTP2_DIR
	@echo "integrations: skipping nghttp2 test (set NGHTTP2_DIR)"
else
	$(MAKE) -C nghttp2 NGHTTP2_DIR=$(NGHTTP2_DIR) test
endif

clean:
	-$(MAKE) -C mbedtls clean
	-$(MAKE) -C nghttp2 clean

.PHONY: all test clean mbedtls mbedtls-test nghttp2 nghttp2-test lwip
