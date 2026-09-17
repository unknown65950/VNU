#pragma once
#include <stdint.h>

// Polling ATA (IDE) PIO driver for the primary and secondary channels.
//
// Everything here is synchronous and assumes the caller runs with the
// scheduler parked (kernel context or a coroutine that is happy to spin
// for the duration of a transfer). No IRQ14/15 handler is installed:
// the status/DRQ bits are polled instead, mirroring how the rest of this
// kernel does I/O (keyboard, mouse, tty all poll).
//
// Only LBA28 is issued. That caps a single drive at 2^28 * 512 bytes =
// 128 GiB, which covers the HDD/SSD targets this install path cares
// about; LBA48 can be layered on later without changing the public API.

namespace vnu::ata {

constexpr int MAX_DRIVES = 4;      // primary master/slave + secondary
constexpr uint32_t SECTOR_SIZE = 512;

struct DriveInfo {
    bool present;
    bool lba28;
    uint32_t sectors;  // total addressable 512-byte sectors
    uint32_t size_mib; // convenience, rounded down
    char model[41];    // NUL-terminated, ASCII
};

// Probe both channels and populate the drive table. Safe to call once,
// from kernel boot, before any userspace starts.
void init();

int drive_count();               // number of present drives
const DriveInfo& drive(int idx); // idx in [0, drive_count())

// Sector I/O. `drive` is the zero-based index from drive_count(), not a
// raw (channel, master/slave) pair, so callers never poke the hardware.
// `count` sectors are transferred to/from `buf` (must hold count*512
// bytes). Returns false on any error or out-of-range request.
bool read_sectors(int drive, uint32_t lba, uint32_t count, void* buf);
bool write_sectors(int drive, uint32_t lba, uint32_t count, const void* buf);

// Best-effort cache flush (no-op on PIO, but kept for API completeness).
bool flush(int drive);

} // namespace vnu::ata
