#pragma once

#include <stdint.h>

// In-system disk installer.
//
// `install(drive, size_mib, hostname)` turns the ATA disk at index
// `drive` (see ata.h / disk_count()) into a bootable VNU disk: an MBR +
// GRUB core image are written to the first sectors, and a fresh FAT16
// partition is created at LBA 2048 holding /boot/kernel.elf (the running
// kernel, obtained from the Multiboot2 module GRUB loaded alongside it)
// and /boot/grub/grub.cfg. `size_mib` is the partition size in MiB; 0
// means "as much of the disk / the FAT16 limit allows". After it
// returns 0 the disk can be booted on its own, with no CD.
//
// `hostname` is the node name the machine should boot with (see
// vnu/host.h: 1..63 characters of letters, digits, '-', '_' and '.',
// which vnu::host::valid() checks). It is stamped into a small config
// record in the gap sector before the partition, so the next boot of
// that disk can put it back into /etc/hostname - this VFS is RAM-backed
// and has nowhere else to keep a name across a reboot.
//
// Returns 0 on success, or a negative errno on failure.

namespace vnu::install {

int disk_count();
int install(int drive, uint32_t size_mib, const char* hostname);

// Index of the first ATA disk carrying a VNU install, or -1 when the
// machine has none (a live CD session, or disks that are not ours).
// Detection is by the FAT16 boot sector's OEM name, so it is a fact
// about the disks rather than a guess; /proc/boot reports it.
int installed_drive();

// The node name recorded on an installed disk, copied into `out` and
// NUL-terminated. 0 when a disk carries a record with a valid name,
// -1 when no installed disk or no record (live session, or a disk
// installed before host names existed).
int installed_hostname(char* out, uint32_t max);

} // namespace vnu::install
