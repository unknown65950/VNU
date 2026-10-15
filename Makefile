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
VHD        := $(VNU)/vnu.vhd
VHD_SIZE   ?= 64
JOBS       ?= $(shell nproc 2>/dev/null || echo 1)
RUN_ARGS   ?=
TEST_ARGS  ?=

# Architectures are the directories under vnu/kernel/arch/, so this list
# grows by itself when a new port lands. Pick one with `ARCH=<name>`
# (make iso ARCH=x86_64) or as a bare word in RUN_ARGS (make run
# RUN_ARGS=x86_64); i386 is the default.
ARCHES      := $(sort $(notdir $(patsubst %/,%,$(wildcard $(KERNEL_DIR)/arch/*/))))
ARCH_FROM_RUN := $(firstword $(filter $(ARCHES),$(RUN_ARGS)))
ARCH        ?= $(if $(ARCH_FROM_RUN),$(ARCH_FROM_RUN),i386)
ifeq ($(filter $(ARCH),$(ARCHES)),)
$(error unknown ARCH '$(ARCH)' -- available: $(ARCHES))
endif

# i386 keeps the historic names (the README, the test suite and
# `vnu install` all say vnu/kernel/build and vnu/vnu.iso); every other
# port gets its own build tree and image so two architectures can sit
# side by side without clobbering each other.
ifeq ($(ARCH),i386)
BUILD_DIR := $(KERNEL_DIR)/build
ISO       := $(VNU)/vnu.iso
else
BUILD_DIR := $(KERNEL_DIR)/build-$(ARCH)
ISO       := $(VNU)/vnu-$(ARCH).iso
endif

# The guest test suite boots the default image with qemu-system-i386, so
# it stays on the i386 ISO whatever ARCH says.
TEST_ISO := $(VNU)/vnu.iso

# RUN_VARS is RUN_ARGS with the architecture words removed: ARCH already
# carries that choice, and the rest (vhd, virtio-gpu, VHD=..., SIZE=...)
# goes through to run.sh untouched.
RUN_VARS := $(filter-out $(ARCHES),$(RUN_ARGS))

# embedded_*.h are generated headers holding the userspace blobs the
# kernel links against; the kernel cannot be built without them.
EMBEDDED := $(wildcard $(KERNEL_DIR)/proc/embedded_*.h)

# Everything vnu/vnu.iso is built from. Two kinds of file are left out
# on purpose. The build artifacts land in these very trees (embedded_*.h,
# the kernel and vlibc build dirs, __pycache__), so counting them would
# make the ISO look older than its own sources for ever and rebuild it on
# every run. The rest is pure documentation (READMEs and other *.md, CI
# *.yml) and backup/editor debris, none of which any build step reads, so
# editing a doc does not relink the image. Everything a build consumes
# stays in -- the embedded pictures/sounds, tham.conf, the man pages
# (*.1 and the extensionless ones). CMakeLists.txt is a source, not
# documentation, which is why *.txt is not excluded here.
ISO_SOURCES := $(shell find $(KERNEL_DIR) $(VNU)/userspace $(VNU)/initrd \
                      vlibc tools thams \
                      -type f -not -path '$(KERNEL_DIR)/build/*' \
                      -not -path '$(KERNEL_DIR)/build-*/*' \
                      -not -path 'vlibc/build/*' \
                      -not -path '*/__pycache__/*' \
                      -not -name 'embedded_*.h' -not -name '.off' \
                      -not -name '*.md' -not -name '*.yml' -not -name '*.yaml' \
                      -not -name '*.pyc' -not -name '*.bak' -not -name '*.swp' \
                      2>/dev/null)

.PHONY: help all iso kernel userspace vhd \
        thambuild thamupdate thamon thamoff thamstatus \
        run run-headless run-gpu run-headless-gpu run-vhd run-headless-vhd \
        run-installed run-headless-installed \
        test test-gpu test-install test-fat16 test-all \
        doctor size clean clean-vhd distclean

## @build

help: ## list every target (this is the default goal)
	@printf 'VNU — build, run and test through make\n\n'
	@awk 'BEGIN{FS=":.*?## "} \
	    /^## @/ {sub(/^## @/, "", $$0); printf "\n%s\n", $$0; next} \
	    /^[a-z][a-z-]*:.*## / {printf "  %-18s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf '\nVariables: ARCH=%s (%s)  VHD_SIZE=%s (MiB)  JOBS=%s  RUN_ARGS=  TEST_ARGS=\n' '$(ARCH)' '$(ARCHES)' '$(VHD_SIZE)' '$(JOBS)'
	@printf 'Examples:\n'
	@printf '  make iso                      build userspace + kernel + ISO\n'
	@printf '  make iso ARCH=x86_64          build another port -> vnu/vnu-x86_64.iso\n'
	@printf '  make run RUN_ARGS=x86_64      boot that port (ARCH= works too)\n'
	@printf '  make iso run-headless         build, then boot (serial console)\n'
	@printf '  make run RUN_ARGS=virtio-gpu  QEMU window on a virtio-gpu display\n'
	@n=$$(./tools/qemu_test.py --list 2>/dev/null | wc -l); \
	 if [ "$$n" -gt 0 ]; then \
	     printf "  make test                     automated guest tests (%s cases)\n" "$$n"; \
	 else \
	     printf '  make test                     automated guest tests\n'; \
	 fi
	@printf '  make test-install             install to a disk, boot that disk\n'
	@printf '  make test-fat16               host-side FAT16 library test (no QEMU)\n'

all: iso ## same as iso (reads better in a script)

iso: $(ISO) ## build userspace, the kernel and the ISO (if a source changed)

# The ISO is a file target, so everything that boots or tests it gets the
# image the sources actually describe. Without it a target depending on
# the mere existence of an old vnu/vnu.iso would test yesterday's kernel.
$(ISO): $(ISO_SOURCES)
	@./vnu/build_iso.sh ARCH=$(ARCH)

userspace: ## build the userspace binaries and embedded_*.h
	@./tools/build_userspace.sh

kernel: $(if $(EMBEDDED),,userspace) ## build only the kernel (cmake, no ISO)
	@cmake -S $(KERNEL_DIR) -B $(BUILD_DIR) -DVNU_KERNEL_ARCH=$(ARCH)
	@cmake --build $(BUILD_DIR) -j$(JOBS)

## @third-party modules (thams/)

# A THAM is a program kept out of the system tree: its own directory in
# thams/, its own sources, its own manual page, and no line of the OS
# edited to add it. See thams/README.md for the contract. The module
# binaries live in sysroot/thams/ and their bytes go into the kernel
# image through the generated embedded_thams.h.

thamstatus: ## list the modules in thams/ and what each one brings
	@python3 ./tools/thams.py status

thambuild: ## compile the modules in thams/ once (no kernel, no ISO)
	@python3 ./tools/thams.py build

# Rebuilding a module means putting new bytes in the kernel, so this is
# the whole chain and not just the compile — which is the reason it is
# a target of its own rather than part of thambuild.
#
# The order matters and is the reason this is not just `iso`: the
# header has to be written before the kernel is compiled, and the ISO
# is a file target that would happily be rebuilt first and leave the
# module header stale. So: regenerate, drop the image, build it again.
#
# It depends on `userspace` because build_iso.sh skips that step when
# the embedded headers are already there, and a module build must never
# quietly ship a stale OS binary beside it: change man.c and run
# thamupdate, and the man in the image has to be the new one.
thamupdate: userspace ## put the modules in the image (rebuilds the ISO)
	@python3 ./tools/thams.py generate
	@rm -f $(ISO)
	@$(MAKE) --no-print-directory iso
	@echo "  the modules are in $(ISO)"

thamoff: ## leave the modules out of the image, keeping the sources
	@python3 ./tools/thams.py off
	@echo "  run 'make thamupdate' to rebuild the ISO without them"

thamon: ## build the modules into the image again
	@python3 ./tools/thams.py on
	@echo "  run 'make thamupdate' to rebuild the ISO with them"

## @running in QEMU

run: $(ISO) ## boot the ISO in a QEMU window (rebuilt first if a source changed)
	@./vnu/run.sh $(ARCH) $(RUN_VARS)

run-headless: $(ISO) ## boot without graphics, serial console in this terminal
	@./vnu/run.sh $(ARCH) --headless $(RUN_VARS)

run-gpu: $(ISO) ## boot in a window with the desktop on virtio-gpu
	@./vnu/run.sh $(ARCH) virtio-gpu $(RUN_VARS)

run-headless-gpu: $(ISO) ## boot headless, but still on a virtio-gpu display
	@./vnu/run.sh $(ARCH) virtio-gpu --headless $(RUN_VARS)

run-vhd: vhd $(ISO) ## boot in a window with the test disk vnu/vnu.vhd attached
	@./vnu/run.sh $(ARCH) vhd $(RUN_VARS)

run-headless-vhd: vhd $(ISO) ## the same, with the serial console in this terminal
	@./vnu/run.sh $(ARCH) vhd --headless $(RUN_VARS)

run-installed: ## boot the installed disk alone, no ISO (see `vnu install`)
	@./vnu/run.sh $(ARCH) installed $(RUN_VARS)

run-headless-installed: ## the same, with the serial console in this terminal
	@./vnu/run.sh $(ARCH) installed --headless $(RUN_VARS)

vhd: ## create the test disk vnu/vnu.vhd (VHD_SIZE=size in MiB)
	@if [ -f $(VHD) ]; then \
	    echo "disk already exists: $(VHD)"; \
	else \
	    qemu-img create -f vpc $(VHD) $(VHD_SIZE)M >/dev/null && \
	    echo "created $(VHD) ($(VHD_SIZE) MiB)"; \
	fi

## @tests

test: $(TEST_ISO) ## automated guest tests in QEMU (ISO boot)
	@python3 tools/qemu_test.py $(TEST_ARGS)

test-gpu: $(TEST_ISO) ## the same suite on a virtio-gpu display
	@python3 tools/qemu_test.py --gpu $(TEST_ARGS)

test-install: $(TEST_ISO) ## install to a disk, boot that disk
	@python3 tools/qemu_test.py --install-only $(TEST_ARGS)

test-fat16: ## host-side test for the FAT16 library (no QEMU)
	@mkdir -p $(BUILD_DIR)
	@g++ -std=c++17 -O1 -I $(KERNEL_DIR)/include \
	    $(KERNEL_DIR)/fs/fat16.cpp tools/fat16_test.cpp -o $(BUILD_DIR)/fat16_test
	@$(BUILD_DIR)/fat16_test

test-all: $(TEST_ISO) ## everything: ISO suite + virtio-gpu + install
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
	for a in $(filter-out i386,$(ARCHES)); do \
	    q="qemu-system-$$a"; \
	    if command -v $$q >/dev/null 2>&1; then \
	        printf '  ok      %s (the %s port)\n' "$$q" "$$a"; \
	    else \
	        printf '  note    no %s — the %s port will not run here\n' "$$q" "$$a"; \
	    fi; \
	done; \
	if [ $$missing -ne 0 ]; then printf '\nsome tools are missing, see README -> Build and run\n'; exit 1; fi; \
	$(MAKE) --no-print-directory mirrors; \
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

mirrors: ## check that the headers kept in two places are still identical
	@bad=0; \
	for pair in \
	    '$(KERNEL_DIR)/include/vnu/abi.h vlibc/include/vnu/abi.h' \
	    '$(KERNEL_DIR)/include/vnu/px.h vnu/userspace/gui/px.h'; do \
	    set -- $$pair; \
	    if [ ! -f "$$1" ] || [ ! -f "$$2" ]; then \
	        printf '  MISSING %s or %s\n' "$$1" "$$2"; bad=1; \
	    elif cmp -s "$$1" "$$2"; then \
	        printf '  ok      %s\n' "$$1"; \
	    else \
	        printf '  DIFFER  %s\n            vs %s\n' "$$1" "$$2"; bad=1; \
	    fi; \
	done; \
	if [ $$bad -ne 0 ]; then \
	    printf '\ntwo copies of one header have drifted apart\n'; exit 1; \
	fi; \
	printf '\nno mirrored header has drifted\n'

## @cleaning

clean: ## drop the build directories and stray .o files (keeps the ISO and the disk)
	@rm -rf $(KERNEL_DIR)/build $(KERNEL_DIR)/build-* $(VNU)/iso
	@find . -name '*.o' -not -path './$(KERNEL_DIR)/build/*' -not -path './$(KERNEL_DIR)/build-*/*' -not -path './sysroot/*' -print -delete | sed 's/^/  removed /'
	@echo "  removed build/ and iso/"

clean-vhd: ## delete the test disk vnu/vnu.vhd
	@rm -f $(VHD) && echo "  removed $(VHD)"

distclean: clean ## drop everything that can be regenerated: ISO, sysroot, embedded_*.h, disk
	@rm -f $(VNU)/vnu.iso $(VNU)/vnu-*.iso $(KERNEL_DIR)/proc/embedded_*.h
	@rm -rf sysroot $(VNU)/.tmp_grub
	@echo "  removed the ISO, sysroot/, embedded_*.h, vnu.vhd"
	@echo "  grub-seed is kept: on a host without grub-mkrescue it is taken from an already built ISO"
