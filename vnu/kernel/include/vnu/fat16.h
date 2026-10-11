#pragma once
#include <stdint.h>
#include <stddef.h>

// FAT16: a self-contained read/write filesystem.
//
// This is the general FAT16 implementation the system mounts on disk
// volumes; it is also what the installer lays the boot volume down with.
// It is deliberately freestanding and POSIX-friendly: it talks to the
// disk only through a small sector-I/O callback (Device below), never
// through the ATA driver or any syscall, so the same code runs in the
// kernel, in a userspace program and on the host (see tools/fat16_test).
//
// On-disk layout (all little-endian, sector-addressed):
//   sector 0               boot sector / BPB
//   reserved ..            nothing here (reserved = 1)
//   first_fat ..           two copies of the 16-bit FAT
//   first_root ..          fixed root directory (root_entries * 32 bytes)
//   first_data ..          clusters, 2 = first
//
// Only 512-byte logical sectors and cluster sizes of one power-of-two
// number of sectors are supported, which is what every FAT16 volume in
// the wild uses. Long file names (VFAT LFN) are read and written, and
// 8.3 names carry the NT case bits, so `Readme.txt' stays mixed-case
// without an LFN entry.

namespace vnu::fat16 {

constexpr uint32_t SECTOR = 512;
/* Longest name a caller's buffer has to hold (255 UTF-16 units + NUL). */
constexpr uint32_t NAME_MAX = 256;

/* Directory-entry attribute bits, as on disk. */
enum Attr : uint8_t {
    ATTR_RO = 0x01,
    ATTR_HIDDEN = 0x02,
    ATTR_SYSTEM = 0x04,
    ATTR_VOLUME = 0x08,
    ATTR_DIR = 0x10,
    ATTR_ARCH = 0x20,
};

/* Sector I/O the volume is read and written through. `lba` is the
 * absolute sector number (the partition start is already folded in by
 * Fs::part_lba), `count` sectors are transferred to/from `buf`. The
 * callbacks return 0 on success and a negative value on error. */
struct Device {
    void* ctx;
    int (*read)(void* ctx, uint32_t lba, uint32_t count, void* buf);
    int (*write)(void* ctx, uint32_t lba, uint32_t count, const void* buf);
    int (*flush)(void* ctx);
};

/* Mounted-volume state. The caller owns the storage (a static/global or
 * a stack object) and the one-sector scratch buffer handed to mount();
 * the library never allocates. */
struct Fs {
    Device dev;
    uint32_t part_lba;        /* absolute LBA of sector 0 of the volume */
    uint32_t total_sectors;
    uint32_t spc;             /* sectors per cluster */
    uint32_t fatsz;           /* sectors per FAT copy */
    uint32_t num_fats;
    uint32_t root_entries;
    uint32_t root_dir_sectors;
    uint32_t first_fat;       /* relative to part_lba */
    uint32_t first_root;
    uint32_t first_data;
    uint32_t max_cluster;     /* highest valid cluster number */
    bool mounted;
    uint8_t* sec;             /* caller scratch, SECTOR bytes */
};

/* Lay a fresh FAT16 volume down in [part_lba, part_lba+part_sectors).
 * `oem` is up to 8 bytes, `label` up to 11 (both padded with spaces),
 * `volume_id` the 32-bit serial number. The volume gets a root-directory
 * volume-label entry so `search --label' finds it. Returns false when
 * the region is too small (FAT16 needs at least 4085 clusters) or a
 * write fails. */
bool format(const Device& dev, uint32_t part_lba, uint32_t part_sectors,
            const char* oem, const char* label, uint32_t volume_id);

/* Read the BPB at part_lba and prepare the volume for use. `sector_buf`
 * must point at SECTOR bytes and is used for every transfer. */
bool mount(Fs& fs, const Device& dev, uint32_t part_lba, uint8_t* sector_buf);
bool umount(Fs& fs);
bool sync(Fs& fs);

/* Look `name` up in directory `dir_cluster` (0 = the fixed root). On
 * success fills the entry's first cluster, byte size and attributes. */
bool lookup(Fs& fs, uint32_t dir_cluster, const char* name,
            uint32_t* out_cluster, uint32_t* out_size, uint8_t* out_attr);

/* Resolve a slash-separated path relative to the volume root ("" and
 * "/" both mean the root) the way lookup() resolves one component. */
bool resolve(Fs& fs, const char* path, uint32_t* out_cluster,
             uint32_t* out_size, uint8_t* out_attr);

/* File I/O by (directory, name). read fails on a directory; write
 * creates the file when it is missing, grows it as needed and reports
 * the number of bytes stored. */
bool read(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t off,
          void* buf, uint32_t len, uint32_t* out);
bool write(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t off,
           const void* buf, uint32_t len, uint32_t* out);
bool create(Fs& fs, uint32_t dir_cluster, const char* name);
bool truncate(Fs& fs, uint32_t dir_cluster, const char* name, uint32_t size);
bool unlink(Fs& fs, uint32_t dir_cluster, const char* name);
bool mkdir(Fs& fs, uint32_t dir_cluster, const char* name);
bool rmdir(Fs& fs, uint32_t dir_cluster, const char* name);

/* Directory iteration. Start a cursor on a directory, then pull names
 * out one at a time; "." and ".." and the volume label are skipped, the
 * long name is returned when the entry has one. */
struct Iter {
    uint32_t dir_cluster;
    uint32_t pos;         /* byte offset of the next directory entry */
    uint32_t total;       /* size of the directory in bytes */
    uint32_t seen;        /* real entries returned so far */
};
void opendir(Fs& fs, uint32_t dir_cluster, Iter* it);
bool readdir(Fs& fs, Iter* it, char* name, uint32_t name_cap,
             uint32_t* out_cluster, uint32_t* out_size, uint8_t* out_attr);

/* Kernel-side convenience (defined in the installer): format the
 * bootable volume on an ATA drive and copy the kernel, initrd and
 * grub.cfg into it. Declared here so the FAT16 header owns the volume
 * layout documentation. */
bool create_boot_volume(int drive, uint32_t part_lba, uint32_t part_sectors,
                        const void* kernel, uint32_t kernel_size,
                        const void* initrd, uint32_t initrd_size,
                        const char* grub_cfg);

} // namespace vnu::fat16
