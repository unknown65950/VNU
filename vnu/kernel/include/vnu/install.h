#pragma once

#include <stdint.h>

// In-system disk installer.
//
// `install(drive, size_mib)` turns the ATA disk at index `drive` (see
// ata.h / disk_count()) into a bootable VNU disk: an MBR + GRUB core
// image are written to the first sectors, and a fresh FAT16 partition
// is created at LBA 2048 holding /boot/kernel.elf (the running kernel,
// obtained from the Multiboot2 module GRUB loaded alongside it) and
// /boot/grub/grub.cfg. `size_mib` is the partition size in MiB; 0 means
// "as much of the disk / the FAT16 limit allows". After it returns 0
// the disk can be booted on its own, with no CD.
//
// Returns 0 on success, or a negative errno on failure.

namespace vnu::install {

int disk_count();
int install(int drive, uint32_t size_mib);

// Index of the first ATA disk carrying a VNU install, or -1 when the
// machine has none (a live CD session, or disks that are not ours).
// Detection is by the FAT16 boot sector's OEM name, so it is a fact
// about the disks rather than a guess; /proc/boot reports it.
int installed_drive();

} // namespace vnu::install
