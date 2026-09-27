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
# ...and boot the live ISO, so the installer can write onto that disk.
#
# The word installed boots from the disk alone — no ISO at all, which is
# what a machine looks like once `vnu install` has written it:
#   ./vnu/run.sh installed           — boot vnu/vnu.vhd as the only medium
#   ./vnu/run.sh installed --headless — the same, serial in this terminal
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

# Absolute path to this script: it changes directory below, and --help
# still has to read the comment block at the top.
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")"

HEADLESS=0
USE_VHD=0
USE_INSTALLED=0
VIRTIO_GPU=0
VHD_PATH="vnu.vhd"
SIZE_MIB=64

for arg in "$@"; do
    case "$arg" in
        --headless)   HEADLESS=1 ;;
        vhd)          USE_VHD=1 ;;
        installed)    USE_INSTALLED=1; USE_VHD=1 ;;
        virtio-gpu)   VIRTIO_GPU=1 ;;
        --help|-h)    sed -n '5,30p' "$SELF" | sed 's/^# \{0,1\}//'; exit 0 ;;
        VHD=*)        VHD_PATH="${arg#VHD=}" ; USE_VHD=1 ;;
        SIZE=*)       SIZE_MIB="${arg#SIZE=}" ;;
        *) echo "run.sh: unknown argument '$arg' (see --help)" >&2; exit 2 ;;
    esac
done

# Booting the installed disk needs no ISO, so skip the build in that mode.
if [[ "$USE_INSTALLED" == 0 && ! -f vnu.iso ]]; then
    echo "no vnu.iso yet, starting build_iso.sh..."
    ./build_iso.sh
fi

QEMU_DISK_ARGS=()
if [[ "$USE_VHD" == 1 ]]; then
    if [[ ! -f "$VHD_PATH" ]]; then
        if [[ "$USE_INSTALLED" == 1 ]]; then
            echo "run.sh: no disk at $VHD_PATH to boot from." >&2
            echo "  Install one first: 'make run-vhd', then 'vnu install 0 NAME'" >&2
            echo "  inside the guest (or point at another image with VHD=<path>)." >&2
            exit 1
        fi
        echo "creating the VHD disk: $VHD_PATH (${SIZE_MIB} MiB)"
        qemu-img create -f vpc "$VHD_PATH" "${SIZE_MIB}M" >/dev/null
    else
        echo "attaching the existing VHD disk: $VHD_PATH"
    fi
    QEMU_DISK_ARGS=(-hda "$VHD_PATH")
fi

# Advisory check for `installed`: the installer stamps the FAT16 OEM name
# "VNUFS   " into the boot sector of the partition it creates (LBA 2048),
# so a disk without it was never written by `vnu install` and has no
# bootloader. The image is a VHD container, so a guest LBA is not a file
# offset: read the disk's contents through `qemu-img convert` when both
# it and od(1) are available, and skip the check otherwise.
if [[ "$USE_INSTALLED" == 1 ]] &&
   command -v qemu-img >/dev/null 2>&1 && command -v od >/dev/null 2>&1; then
    raw=$(mktemp "${TMPDIR:-/tmp}/vnu-raw.XXXXXX")
    if qemu-img convert -O raw "$VHD_PATH" "$raw" 2>/dev/null; then
        oem=$(od -An -c -j $((2048 * 512 + 3)) -N 8 "$raw" 2>/dev/null |
              tr -d ' \n')
        if [[ "$oem" != "VNUFS" ]]; then
            echo "run.sh: warning: $VHD_PATH carries no VNU install" >&2
            echo "  (no VNUFS volume on it) — boot it once from the ISO with" >&2
            echo "  'make run-vhd' and run 'vnu install 0 NAME' in the guest." >&2
        fi
    fi
    rm -f "$raw"
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

# Normally boot from the ISO (device d) so the installer/the tests can
# write to the VHD; the disk itself (c) stays attached and ready to be
# installed onto. In `installed` mode there is no ISO at all: the disk
# is the only medium and QEMU boots it as the hard disk (c).
QEMU_BOOT_ARGS=(-cdrom vnu.iso -boot d)
if [[ "$USE_INSTALLED" == 1 ]]; then
    QEMU_BOOT_ARGS=(-boot c)
fi

if [[ "$HEADLESS" == 1 ]]; then
    qemu-system-i386 -m 32 -display none -serial stdio -no-reboot "${QEMU_BOOT_ARGS[@]}" "${QEMU_AUDIO_ARGS[@]}" "${QEMU_GPU_ARGS[@]}" "${QEMU_NET_ARGS[@]}" "${QEMU_DISK_ARGS[@]}"
else
    qemu-system-i386 -serial stdio -no-reboot "${QEMU_BOOT_ARGS[@]}" "${QEMU_AUDIO_ARGS[@]}" "${QEMU_GPU_ARGS[@]}" "${QEMU_NET_ARGS[@]}" "${QEMU_DISK_ARGS[@]}"
fi
