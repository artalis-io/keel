# integrations/uefi/Makefile — U-1 spike-local build (NOT wired into the root build).
#
# Targets:
#   make build   — build the freestanding self-contained archive + BOOTX64.EFI
#   make run     — build + boot in QEMU/OVMF, capture serial (needs qemu/ovmf/mtools)
#   make clean   — remove build artifacts
#
# Requires clang + lld (PE cross target) for build; qemu-system-x86 + OVMF +
# mtools + dosfstools for run. See README.md for the Apple-container recipe.

KEEL_ROOT ?= ../..

.PHONY: build run clean

build:
	$(MAKE) -C $(KEEL_ROOT) freestanding-lib-selfcontained
	KEEL_ROOT=$(KEEL_ROOT) ./build.sh

run:
	KEEL_ROOT=$(KEEL_ROOT) ./run.sh

clean:
	rm -f *.obj BOOTX64.EFI esp.img vars_rw.fd
