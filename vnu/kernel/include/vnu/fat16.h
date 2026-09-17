#pragma once
#include <stdint.h>

// Write-only FAT16 formatter/populator.
//
// The disk installer needs to create a bootable FAT volume that GRUB can
// read back (grub's `fat` module understands FAT12/16/32). We only ever
// create a fresh volume and write a handful of files into it, so there is
// no need for a general filesystem: a bump allocator over the data area,
// RMW writes to the FAT copies, and fixed 8.3 directory entries cover the
// whole contract. FAT16 (not FAT32) is used because the fixed root
// directory region makes the initial layout trivial and the volume sizes
// involved are well under the 2 GiB FAT16 ceiling.

namespace vnu::fat16 {

// Format the region [part_lba, part_lba + part_sectors) on `drive` as
// FAT16 and lay down:
//   /boot/kernel.elf      <- kernel[0..kernel_size)
//   /boot/grub/grub.cfg   <- grub_cfg (NUL-terminated text)
// Returns false if any sector write fails or the volume is too small.
bool create_boot_volume(int drive, uint32_t part_lba, uint32_t part_sectors,
                        const void* kernel, uint32_t kernel_size,
                        const char* grub_cfg);

} // namespace vnu::fat16
