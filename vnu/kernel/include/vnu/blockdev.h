#pragma once
#include <stdint.h>
#include <vnu/fat16.h>

// Block devices: the ATA disks and the partitions on them, as a byte
// addressable table the VFS can expose under /dev and mount a
// filesystem from.
//
// The table is filled by init() from the drives the ATA driver found:
// every present disk contributes a whole-drive entry (e.g. `hda`) and,
// when it carries a valid MBR, one entry per non-empty partition
// (`hda1`, `hda2`, ...). I/O is expressed in bytes from the start of the
// device, so a partition entry behaves exactly like a smaller disk; the
// sector offset folded in here is what lets the FAT16 library mount a
// partition with part_lba = 0.
namespace vnu::blockdev {

constexpr int MAX_DEVICES = 12;
constexpr int NAME_CAP = 16;

struct Device {
    bool whole;           // whole drive vs. a partition entry
    uint8_t drive;        // ATA drive index
    uint32_t lba;         // first sector on the drive
    uint32_t sectors;     // length in 512-byte sectors
    char name[NAME_CAP];  // "hda", "hda1", ...
};

// Probe the MBRs and build the table. Call once, after ata::init().
void init();
int count();
const Device& at(int idx);   // idx in [0, count())
int find(const char* name);  // -1 when absent
uint32_t sector_count(int idx);
uint32_t sector_size(int idx); // bytes, or 0 for a bad index

// Byte-granular read/write. `off` is from the start of the device. Both
// adjust the request to the device's end (a read past the end is 0, a
// partial read/write is clipped) and return the number of bytes
// transferred, or a negative value on I/O error.
int read(int idx, uint64_t off, void* buf, uint32_t len);
int write(int idx, uint64_t off, const void* buf, uint32_t len);

// A sector callback over the device, for the FAT16 library. `part_lba`
// on the Fs/format() calls is 0: the device already points at the
// partition's first sector.
vnu::fat16::Device fat16_device(int idx);

} // namespace vnu::blockdev
