#!/usr/bin/env bash
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
