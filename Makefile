# Makefile — the single entry point for building, running and testing VNU.
#
# There is no build logic here: it lives in tools/build_userspace.sh and
# vnu/build_iso.sh, which the README documents and which the Makefile
# simply calls. This file is a thin, self-documenting front-end
# (`make help`), so the old script sequence keeps working unchanged.
#
# Note: `make run virtio-gpu` asks make for *two* goals, not for an
# argument, so QEMU would start twice. Each variant therefore has its own
# target (make run-gpu), and make run RUN_ARGS=virtio-gpu covers the rest.

SHELL := /bin/bash
.DEFAULT_GOAL := help

# The build is a chain (userspace -> kernel -> iso), so a parallel make
# (-j) buys nothing here.
.NOTPARALLEL:

VNU        := vnu
KERNEL_DIR := $(VNU)/kernel
BUILD_DIR  := $(KERNEL_DIR)/build
ISO        := $(VNU)/vnu.iso
VHD        := $(VNU)/vnu.vhd
VHD_SIZE   ?= 64
JOBS       ?= $(shell nproc 2>/dev/null || echo 1)
RUN_ARGS   ?=
TEST_ARGS  ?=

# embedded_*.h are generated headers holding the userspace blobs the
# kernel links against; the kernel cannot be built without them.
EMBEDDED := $(wildcard $(KERNEL_DIR)/proc/embedded_*.h)

.PHONY: help all iso kernel userspace vhd \
        run run-headless run-gpu run-headless-gpu run-vhd run-headless-vhd \
        test test-gpu test-install test-all \
        doctor size clean clean-vhd distclean

## @build

help: ## list every target (this is the default goal)
	@printf 'VNU — build, run and test through make\n\n'
	@awk 'BEGIN{FS=":.*?## "} \
	    /^## @/ {sub(/^## @/, "", $$0); printf "\n%s\n", $$0; next} \
	    /^[a-z][a-z-]*:.*## / {printf "  %-18s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf '\nVariables: VHD_SIZE=%s (MiB)  JOBS=%s  RUN_ARGS=  TEST_ARGS=\n' '$(VHD_SIZE)' '$(JOBS)'
	@printf 'Examples:\n'
	@printf '  make iso                      build userspace + kernel + ISO\n'
	@printf '  make iso run-headless         build, then boot (serial console)\n'
	@printf '  make run RUN_ARGS=virtio-gpu  QEMU window on a virtio-gpu display\n'
	@n=$$(./tools/qemu_test.py --list 2>/dev/null | wc -l); \
	 if [ "$$n" -gt 0 ]; then \
	     printf "  make test                     automated guest tests (%s cases)\n" "$$n"; \
	 else \
	     printf '  make test                     automated guest tests\n'; \
	 fi
	@printf '  make test-install             install to a disk, boot that disk\n'

all: iso ## same as iso (reads better in a script)

iso: ## build userspace, the kernel and vnu/vnu.iso
	@./vnu/build_iso.sh

userspace: ## build the userspace binaries and embedded_*.h
	@./tools/build_userspace.sh

kernel: $(if $(EMBEDDED),,userspace) ## build only the kernel (cmake, no ISO)
	@cmake -S $(KERNEL_DIR) -B $(BUILD_DIR)
	@cmake --build $(BUILD_DIR) -j$(JOBS)

## @running in QEMU

run: ## boot the ISO in a QEMU window (builds the ISO if it is missing)
	@./vnu/run.sh $(RUN_ARGS)

run-headless: ## boot without graphics, serial console in this terminal
	@./vnu/run.sh --headless $(RUN_ARGS)

run-gpu: ## boot in a window with the desktop on virtio-gpu
	@./vnu/run.sh virtio-gpu $(RUN_ARGS)

run-headless-gpu: ## boot headless, but still on a virtio-gpu display
	@./vnu/run.sh virtio-gpu --headless $(RUN_ARGS)

run-vhd: vhd ## boot in a window with the test disk vnu/vnu.vhd attached
	@./vnu/run.sh vhd $(RUN_ARGS)

run-headless-vhd: vhd ## the same, with the serial console in this terminal
	@./vnu/run.sh vhd --headless $(RUN_ARGS)

vhd: ## create the test disk vnu/vnu.vhd (VHD_SIZE=size in MiB)
	@if [ -f $(VHD) ]; then \
	    echo "disk already exists: $(VHD)"; \
	else \
	    qemu-img create -f vpc $(VHD) $(VHD_SIZE)M >/dev/null && \
	    echo "created $(VHD) ($(VHD_SIZE) MiB)"; \
	fi

## @tests

test: $(if $(wildcard $(ISO)),,iso) ## automated guest tests in QEMU (ISO boot)
	@python3 tools/qemu_test.py $(TEST_ARGS)

test-gpu: $(if $(wildcard $(ISO)),,iso) ## the same suite on a virtio-gpu display
	@python3 tools/qemu_test.py --gpu $(TEST_ARGS)

test-install: $(if $(wildcard $(ISO)),,iso) ## install to a disk, boot that disk
	@python3 tools/qemu_test.py --install-only $(TEST_ARGS)

test-all: $(if $(wildcard $(ISO)),,iso) ## everything: ISO suite + virtio-gpu + install
	@python3 tools/qemu_test.py $(TEST_ARGS)
	@python3 tools/qemu_test.py --gpu $(TEST_ARGS)
	@python3 tools/qemu_test.py --install-only $(TEST_ARGS)

## @utilities

doctor: ## check that the host has everything the build and the tests need
	@missing=0; \
	for t in make bash python3 cmake nasm xorriso qemu-system-i386 qemu-img; do \
	    if command -v $$t >/dev/null 2>&1; then \
	        printf '  ok      %s\n' "$$t"; \
	    else \
	        printf '  MISSING %s\n' "$$t"; missing=1; \
	    fi; \
	done; \
	if command -v g++ >/dev/null 2>&1; then \
	    printf '  ok      g++\n'; \
	    if echo 'int main(){}' | g++ -m32 -x c++ - -o /dev/null 2>/dev/null; then \
	        printf '  ok      g++ -m32 (multilib)\n'; \
	    else \
	        printf '  MISSING g++ -m32: install g++-multilib\n'; missing=1; \
	    fi; \
	else \
	    printf '  MISSING g++\n'; missing=1; \
	fi; \
	if command -v grub-mkrescue >/dev/null 2>&1; then \
	    printf '  ok      grub-mkrescue\n'; \
	else \
	    printf '  note    no grub-mkrescue — the ISO is built the portable way (xorriso + grub-seed)\n'; \
	fi; \
	if [ $$missing -ne 0 ]; then printf '\nsome tools are missing, see README -> Build and run\n'; exit 1; fi; \
	printf '\neverything is in place\n'

size: ## sizes of the built artifacts (the host-side counterpart of `vnu size`)
	@for f in $(BUILD_DIR)/kernel.elf $(ISO) $(VHD); do \
	    if [ -f $$f ]; then \
	        printf '  %-28s %s bytes\n' "$$f" "$$(wc -c < $$f | tr -d ' ')"; \
	    fi; \
	done; \
	if [ -d sysroot/bin ]; then \
	    printf '  %-28s %s bytes (%s files)\n' "sysroot/bin" \
	        "$$(cat sysroot/bin/* 2>/dev/null | wc -c | tr -d ' ')" "$$(ls sysroot/bin | wc -l)"; \
	fi; \
	if ls $(KERNEL_DIR)/proc/embedded_*.h >/dev/null 2>&1; then \
	    printf '  %-28s %s headers\n' "embedded_*.h" "$$(ls $(KERNEL_DIR)/proc/embedded_*.h | wc -l)"; \
	fi

## @cleaning

clean: ## drop the build directories and stray .o files (keeps the ISO and the disk)
	@rm -rf $(BUILD_DIR) $(VNU)/iso
	@find . -name '*.o' -not -path './$(BUILD_DIR)/*' -not -path './sysroot/*' -print -delete | sed 's/^/  removed /'
	@echo "  removed build/ and iso/"

clean-vhd: ## delete the test disk vnu/vnu.vhd
	@rm -f $(VHD) && echo "  removed $(VHD)"

distclean: clean ## drop everything that can be regenerated: ISO, sysroot, embedded_*.h, disk
	@rm -f $(ISO) $(KERNEL_DIR)/proc/embedded_*.h
	@rm -rf sysroot $(VNU)/.tmp_grub
	@echo "  removed the ISO, sysroot/, embedded_*.h, vnu.vhd"
	@echo "  grub-seed is kept: on a host without grub-mkrescue it is taken from an already built ISO"
