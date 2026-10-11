#!/bin/sh
# Build the initial ramdisk: pack vnu/initrd/ into vnu/initrd.tar.
#
# The archive is a plain POSIX ustar tar (the format the kernel reads
# with the freestanding reader in vnu/kernel/include/vnu/ustar.h). It is
# loaded by GRUB as a Multiboot2 module and unpacked into /initrd at
# boot, so the image carries these files without a line of the kernel
# changing. vnu/initrd.tar is a build artifact and is gitignored.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT_DIR/vnu/initrd"
OUT="$ROOT_DIR/vnu/initrd.tar"

if [ ! -d "$SRC_DIR" ]; then
    echo "build_initrd: no source tree at $SRC_DIR" >&2
    exit 1
fi

# --format=ustar keeps GNU's own header extensions out of the archive;
# the kernel reads the POSIX fields only. (GNU and BSD tar both accept
# --format=ustar; no ownership flags, which differ between the two.)
tar --format=ustar -cf "$OUT" -C "$SRC_DIR" .

echo "initrd: $OUT ($(wc -c < "$OUT") bytes)"
