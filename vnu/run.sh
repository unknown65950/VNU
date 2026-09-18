#!/bin/sh
# Bash trampoline: /usr/bin/env is missing on some hosts (e.g. Termux
# without termux-exec) -- if not under bash already, re-exec via PATH.
if [ -z "${BASH_VERSION:-}" ]; then exec env bash "$0" "$@"; fi
# run.sh — запускает vnu.iso в QEMU.
#
# По умолчанию поднимает окно QEMU (нужен доступ к дисплею).
# С флагом --headless выводит ядро в текущий терминал через serial
# (полезно на сервере / в CI, где нет графики).
set -euo pipefail

cd "$(dirname "$0")"

if [[ ! -f vnu.iso ]]; then
    echo "vnu.iso не найден, запускаю build_iso.sh..."
    ./build_iso.sh
fi

if [[ "${1:-}" == "--headless" ]]; then
    qemu-system-i386 -cdrom vnu.iso -m 32 -display none -serial stdio -no-reboot
else
    qemu-system-i386 -cdrom vnu.iso -m 32 -serial stdio -no-reboot
fi
