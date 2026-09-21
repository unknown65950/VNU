#!/bin/sh
# Bash trampoline: /usr/bin/env is missing on some hosts (e.g. Termux
# without termux-exec) -- if not under bash already, re-exec via PATH.
if [ -z "${BASH_VERSION:-}" ]; then exec env bash "$0" "$@"; fi
# run.sh — запускает vnu.iso в QEMU.
#
# По умолчанию поднимает окно QEMU (нужен доступ к дисплею).
# С флагом --headless выводит ядро в текущий терминал через serial.
#
# Для теста установки на диск / флеш-накопитель передайте слово vhd:
#   ./vnu/run.sh vhd                 — создать (если нет) vnu/vnu.vhd
#                                      и подключить его как жёсткий диск
#   ./vnu/run.sh vhd --headless      — то же, но serial в текущий терминал
#
# Дополнительно (необязательно):
#   VHD=<путь>      — свой путь к образу диска (например VHD=/sdcard/sda.vhd)
#   SIZE=<MiB>      — размер создаваемого диска (по умолчанию 64)
#   --help          — эта справка
set -euo pipefail

cd "$(dirname "$0")"

if [[ ! -f vnu.iso ]]; then
    echo "vnu.iso не найден, запускаю build_iso.sh..."
    ./build_iso.sh
fi

HEADLESS=0
USE_VHD=0
VHD_PATH="vnu.vhd"
SIZE_MIB=64

for arg in "$@"; do
    case "$arg" in
        --headless)   HEADLESS=1 ;;
        vhd)          USE_VHD=1 ;;
        --help|-h)    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        VHD=*)        VHD_PATH="${arg#VHD=}" ; USE_VHD=1 ;;
        SIZE=*)       SIZE_MIB="${arg#SIZE=}" ;;
        *) echo "run.sh: неизвестный аргумент '$arg' (см. --help)" >&2; exit 2 ;;
    esac
done

QEMU_DISK_ARGS=()
if [[ "$USE_VHD" == 1 ]]; then
    if [[ ! -f "$VHD_PATH" ]]; then
        echo "Создаю диск VHD: $VHD_PATH (${SIZE_MIB} MiB)"
        qemu-img create -f vpc "$VHD_PATH" "${SIZE_MIB}M" >/dev/null
    else
        echo "Подключаю существующий диск VHD: $VHD_PATH"
    fi
    QEMU_DISK_ARGS=(-hda "$VHD_PATH")
fi

# Грузимся с ISO (устройство d), чтобы установщик/тест мог писать на VHD;
# сам диск (c) остаётся рядом и готов к установке на него.
if [[ "$HEADLESS" == 1 ]]; then
    qemu-system-i386 -cdrom vnu.iso -m 32 -boot d -display none -serial stdio -no-reboot "${QEMU_DISK_ARGS[@]}"
else
    qemu-system-i386 -cdrom vnu.iso -m 32 -boot d -serial stdio -no-reboot "${QEMU_DISK_ARGS[@]}"
fi
