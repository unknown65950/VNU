#pragma once

// In-system disk installer.
//
// `install(drive)` turns the ATA disk at index `drive` (see ata.h /
// disk_count()) into a bootable VNU disk: an MBR + GRUB core image are
// written to the first sectors, and a fresh FAT16 partition is created
// at LBA 2048 holding /boot/kernel.elf (the running kernel, obtained
// from the Multiboot2 module GRUB loaded alongside it) and
// /boot/grub/grub.cfg. After it returns 0 the disk can be booted on its
// own, with no CD.
//
// Returns 0 on success, or a negative errno on failure.

namespace vnu::install {

int disk_count();
int install(int drive);

} // namespace vnu::install
