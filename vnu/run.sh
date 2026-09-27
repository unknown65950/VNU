#!/bin/sh
# Bash trampoline: /usr/bin/env is missing on some hosts (e.g. Termux
# without termux-exec) -- if not under bash already, re-exec via PATH.
if [ -z "${BASH_VERSION:-}" ]; then exec env bash "$0" "$@"; fi
# run.sh — boots vnu.iso in QEMU.
#
# By default it opens a QEMU window (a display has to be available).
# With --headless the kernel writes to this terminal over serial instead.
#
# To test an install onto a disk / USB stick, pass the word vhd:
#   ./vnu/run.sh vhd                 — create vnu/vnu.vhd (if missing) and
#                                      attach it as a hard disk
#   ./vnu/run.sh vhd --headless      — the same, with serial in this terminal
#
# The word virtio-gpu attaches a virtio-gpu display (QEMU `-device
# virtio-vga` instead of the usual std VGA); the kernel draws the desktop
# on it while the console and the font capture stay on the VGA-compatible
# part of the device:
#   ./vnu/run.sh virtio-gpu           — desktop through virtio-gpu
#   ./vnu/run.sh virtio-gpu --headless — the same, serial in this terminal
#
# Optionally:
#   VHD=<path>      — your own disk image path (e.g. VHD=/sdcard/sda.vhd)
#   SIZE=<MiB>      — size of the disk image created (64 by default)
#   --help          — this help
set -euo pipefail

cd "$(dirname "$0")"

if [[ ! -f vnu.iso ]]; then
    echo "no vnu.iso yet, starting build_iso.sh..."
    ./build_iso.sh
fi

HEADLESS=0
USE_VHD=0
VIRTIO_GPU=0
VHD_PATH="vnu.vhd"
SIZE_MIB=64

for arg in "$@"; do
    case "$arg" in
        --headless)   HEADLESS=1 ;;
        vhd)          USE_VHD=1 ;;
        virtio-gpu)   VIRTIO_GPU=1 ;;
        --help|-h)    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        VHD=*)        VHD_PATH="${arg#VHD=}" ; USE_VHD=1 ;;
        SIZE=*)       SIZE_MIB="${arg#SIZE=}" ;;
        *) echo "run.sh: unknown argument '$arg' (see --help)" >&2; exit 2 ;;
    esac
done

QEMU_DISK_ARGS=()
if [[ "$USE_VHD" == 1 ]]; then
    if [[ ! -f "$VHD_PATH" ]]; then
        echo "creating the VHD disk: $VHD_PATH (${SIZE_MIB} MiB)"
        qemu-img create -f vpc "$VHD_PATH" "${SIZE_MIB}M" >/dev/null
    else
        echo "attaching the existing VHD disk: $VHD_PATH"
    fi
    QEMU_DISK_ARGS=(-hda "$VHD_PATH")
fi

QEMU_GPU_ARGS=()
if [[ "$VIRTIO_GPU" == 1 ]]; then
    # virtio-vga is a VGA-compatible virtio-gpu: the kernel keeps the
    # console and the font capture on the VGA part and drives the desktop
    # through virtio.
    QEMU_GPU_ARGS=(-vga none -device virtio-vga)
fi

# slirp user networking (an explicit -netdev with the same e1000 the
# default brings up) plus the host loopback forwarded to guest ports:
#   17778 -> 7778  echoserver (nc 127.0.0.1 17778)
#   17779 -> 7779  tlsserver (openssl s_client -connect 127.0.0.1:17779)
#   14433 -> 14433 tlsdemo against openssl s_server on the host
QEMU_NET_ARGS=(-netdev user,id=n0,hostfwd=tcp::17778-:7778,hostfwd=tcp::17779-:7779,hostfwd=tcp::14433-:14433 -device e1000,netdev=n0)

# QEMU's AC'97 sound card (PCI 8086:2415) backs the audio_* syscalls;
# the kernel's ac97 driver does a short playback self-test at boot.
# Modern QEMU (>= 8, where -soundhw was removed) maps `AC97` directly;
# with no -audiodev it silently falls back to the "none" driver, which
# still runs the audio timer at nominal rate (samples are discarded).
QEMU_AUDIO_ARGS=(-device AC97)

# Boot from the ISO (device d) so the installer/the tests can write to the
# VHD; the disk itself (c) stays attached and ready to be installed onto.
if [[ "$HEADLESS" == 1 ]]; then
    qemu-system-i386 -cdrom vnu.iso -m 32 -boot d -display none -serial stdio -no-reboot "${QEMU_AUDIO_ARGS[@]}" "${QEMU_GPU_ARGS[@]}" "${QEMU_NET_ARGS[@]}" "${QEMU_DISK_ARGS[@]}"
else
    qemu-system-i386 -cdrom vnu.iso -m 32 -boot d -serial stdio -no-reboot "${QEMU_AUDIO_ARGS[@]}" "${QEMU_GPU_ARGS[@]}" "${QEMU_NET_ARGS[@]}" "${QEMU_DISK_ARGS[@]}"
fi
