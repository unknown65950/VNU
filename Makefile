# Makefile — единая точка входа для сборки, запуска и тестов VNU.
#
# Логики сборки здесь нет: она живёт в tools/build_userspace.sh и
# vnu/build_iso.sh, и именно этими же скриптами пользуются README и
# документированный поток разработчика. Makefile — тонкая,
# самодокументирующаяся обёртка над ними (`make help`), так что старая
# последовательность команд продолжает работать как раньше.
#
# Обратите внимание: `make run virtio-gpu` в make означает ДВЕ цели, а не
# аргумент, поэтому QEMU стартовал бы дважды. Для вариантов запуска есть
# отдельные цели (make run-gpu) и переменная (make run RUN_ARGS=virtio-gpu).

SHELL := /bin/bash
.DEFAULT_GOAL := help

# Стадии сборки образуют цепочку userspace -> kernel -> iso, поэтому
# параллельный make (-j) для них бессмысленен.
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

# embedded_*.h — генерируемые заголовки, ради которых ядро и пересобирает
# ссылки на userspace; без них kernel-сборка невозможна.
EMBEDDED := $(wildcard $(KERNEL_DIR)/proc/embedded_*.h)

.PHONY: help all iso kernel userspace vhd \
        run run-headless run-gpu run-headless-gpu run-vhd run-headless-vhd \
        test test-gpu test-install test-all \
        doctor size clean clean-vhd distclean

## @сборка

help: ## список целей (это цель по умолчанию)
	@printf 'VNU — сборка, запуск и тесты через make\n\n'
	@awk 'BEGIN{FS=":.*?## "} \
	    /^## @/ {sub(/^## @/, "", $$0); printf "\n%s\n", $$0; next} \
	    /^[a-z][a-z-]*:.*## / {printf "  %-18s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf '\nПеременные: VHD_SIZE=%s (MiB)  JOBS=%s  RUN_ARGS=  TEST_ARGS=\n' '$(VHD_SIZE)' '$(JOBS)'
	@printf 'Примеры:\n'
	@printf '  make iso                     собрать userspace + ядро + ISO\n'
	@printf '  make iso run-headless        собрать и сразу загрузиться (serial)\n'
	@printf '  make run RUN_ARGS=virtio-gpu окно QEMU с virtio-gpu\n'
	@n=$$(./tools/qemu_test.py --list 2>/dev/null | wc -l); \
	 if [ "$$n" -gt 0 ]; then \
	     printf "  make test                    автотесты в QEMU (%s проверок)\n" "$$n"; \
	 else \
	     printf '  make test                    автотесты в QEMU\n'; \
	 fi
	@printf '  make test-install            установка на диск + загрузка с него\n'

all: iso ## то же, что iso (удобнее читается)

iso: ## собрать userspace, ядро и vnu/vnu.iso
	@./vnu/build_iso.sh

userspace: ## собрать userspace-бинарники и embedded_*.h
	@./tools/build_userspace.sh

kernel: $(if $(EMBEDDED),,userspace) ## собрать только ядро (cmake, без ISO)
	@cmake -S $(KERNEL_DIR) -B $(BUILD_DIR)
	@cmake --build $(BUILD_DIR) -j$(JOBS)

## @запуск в QEMU

run: ## QEMU с окном (соберёт ISO, если его нет)
	@./vnu/run.sh $(RUN_ARGS)

run-headless: ## QEMU без графики, вывод через serial в этот терминал
	@./vnu/run.sh --headless $(RUN_ARGS)

run-gpu: ## QEMU с окном и дисплеем virtio-gpu
	@./vnu/run.sh virtio-gpu $(RUN_ARGS)

run-headless-gpu: ## QEMU без графики, но с virtio-gpu
	@./vnu/run.sh virtio-gpu --headless $(RUN_ARGS)

run-vhd: vhd ## QEMU с окном и подключённым тестовым диском vnu/vnu.vhd
	@./vnu/run.sh vhd $(RUN_ARGS)

run-headless-vhd: vhd ## то же, но вывод через serial
	@./vnu/run.sh vhd --headless $(RUN_ARGS)

vhd: ## создать тестовый диск vnu/vnu.vhd (VHD_SIZE=MiB)
	@if [ -f $(VHD) ]; then \
	    echo "диск уже есть: $(VHD)"; \
	else \
	    qemu-img create -f vpc $(VHD) $(VHD_SIZE)M >/dev/null && \
	    echo "создан $(VHD) ($(VHD_SIZE) MiB)"; \
	fi

## @тесты

test: $(if $(wildcard $(ISO)),,iso) ## автотесты в QEMU (загрузка с ISO)
	@python3 tools/qemu_test.py $(TEST_ARGS)

test-gpu: $(if $(wildcard $(ISO)),,iso) ## те же тесты, но на virtio-gpu
	@python3 tools/qemu_test.py --gpu $(TEST_ARGS)

test-install: $(if $(wildcard $(ISO)),,iso) ## install 0 на диск, загрузка с диска
	@python3 tools/qemu_test.py --install-only $(TEST_ARGS)

test-all: $(if $(wildcard $(ISO)),,iso) ## полный прогон: ISO + virtio-gpu + установка
	@python3 tools/qemu_test.py $(TEST_ARGS)
	@python3 tools/qemu_test.py --gpu $(TEST_ARGS)
	@python3 tools/qemu_test.py --install-only $(TEST_ARGS)

## @утилиты

doctor: ## проверить, что на хосте есть всё нужное для сборки и тестов
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
	        printf '  MISSING g++ -m32: поставьте g++-multilib\n'; missing=1; \
	    fi; \
	else \
	    printf '  MISSING g++\n'; missing=1; \
	fi; \
	if command -v grub-mkrescue >/dev/null 2>&1; then \
	    printf '  ok      grub-mkrescue\n'; \
	else \
	    printf '  note    grub-mkrescue нет — ISO соберётся портативным путём (xorriso + grub-seed)\n'; \
	fi; \
	if [ $$missing -ne 0 ]; then printf '\nне хватает инструментов, см. README -> Build and run\n'; exit 1; fi; \
	printf '\nвсё на месте\n'

size: ## размеры собранных артефактов (хостовая сводка, аналог vnu size)
	@for f in $(BUILD_DIR)/kernel.elf $(ISO) $(VHD); do \
	    if [ -f $$f ]; then \
	        printf '  %-28s %s bytes\n' "$$f" "$$(wc -c < $$f | tr -d ' ')"; \
	    fi; \
	done; \
	if [ -d sysroot/bin ]; then \
	    printf '  %-28s %s bytes (%s файлов)\n' "sysroot/bin" \
	        "$$(cat sysroot/bin/* 2>/dev/null | wc -c | tr -d ' ')" "$$(ls sysroot/bin | wc -l)"; \
	fi; \
	if ls $(KERNEL_DIR)/proc/embedded_*.h >/dev/null 2>&1; then \
	    printf '  %-28s %s headers\n' "embedded_*.h" "$$(ls $(KERNEL_DIR)/proc/embedded_*.h | wc -l)"; \
	fi

## @очистка

clean: ## убрать сборочные каталоги и осиротевшие .o (ISO и диск остаются)
	@rm -rf $(BUILD_DIR) $(VNU)/iso
	@find . -name '*.o' -not -path './$(BUILD_DIR)/*' -not -path './sysroot/*' -print -delete | sed 's/^/  удалён /'
	@echo "  удалены build/ и iso/"

clean-vhd: ## удалить тестовый диск vnu/vnu.vhd
	@rm -f $(VHD) && echo "  удалён $(VHD)"

distclean: clean ## убрать всё, что пересобирается заново: ISO, sysroot, embedded_*.h, диск
	@rm -f $(ISO) $(KERNEL_DIR)/proc/embedded_*.h
	@rm -rf sysroot $(VNU)/.tmp_grub
	@echo "  удалены ISO, sysroot/, embedded_*.h, vnu.vhd"
	@echo "  grub-seed оставлен: на хосте без grub-mkrescue он берётся из уже собранного ISO"
