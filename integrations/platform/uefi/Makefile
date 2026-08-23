# integrations/platform/uefi/Makefile — U-1 self-test build (NOT wired into the root build).
#
# Targets:
#   make build   — build the freestanding self-contained archive + BOOTX64.EFI
#   make run     — build + boot in QEMU/OVMF, capture serial (needs qemu/ovmf/mtools)
#   make clean   — remove build artifacts
#
# Requires clang + lld (PE cross target) for build; qemu-system-x86 + OVMF +
# mtools + dosfstools for run. See README.md for the Apple-container recipe.

KEEL_ROOT ?= ../../..

.PHONY: build run clean

# The self-test scripts live in tests/ and derive their own KEEL_ROOT from that
# depth (../../../..), so we don't pass ours (computed from this backend-root dir).
build:
	$(MAKE) -C $(KEEL_ROOT) freestanding-lib-selfcontained
	./tests/build.sh

run:
	./tests/run.sh

# The self-test scripts now emit into tests/ (their CWD); older runs (and the
# pre-move layout) left artifacts at the backend root. .gitignore's patterns are
# location-agnostic, so reclaim every one of them at BOTH levels — leaving
# `find integrations/platform/uefi` free of build residue.
UEFI_CLEAN_FILES = *.obj *.o *.so libkeel_freestanding*.a \
                   BOOTX64.EFI BOOTX64.lib BOOTAA64.EFI BOOTAA64.lib \
                   esp*.img vars*.fd u4_ca_embed.h s6_cert_embed.h mock_efi_test
UEFI_CLEAN_DIRS  = esp_s4_dir wwwroot u4_prod_cert mbedtls_obj *.dSYM
clean:
	rm -f  $(addprefix ./,$(UEFI_CLEAN_FILES)) $(addprefix tests/,$(UEFI_CLEAN_FILES))
	rm -rf $(addprefix ./,$(UEFI_CLEAN_DIRS))  $(addprefix tests/,$(UEFI_CLEAN_DIRS))
