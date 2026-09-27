#include <vnu/install.h>
#include <vnu/ata.h>
#include <vnu/fat16.h>
#include <vnu/host.h>
#include <vnu/mboot.h>
#include <vnu/tty.h>

// GRUB stage blobs, generated at build time by
// tools/gen_install_blobs.sh and pulled in through install/blobs.s.
extern "C" const uint8_t vnu_install_boot_img[];
extern "C" const uint8_t vnu_install_boot_img_end[];
extern "C" const uint8_t vnu_install_core_img[];
extern "C" const uint8_t vnu_install_core_img_end[];

namespace {

constexpr uint32_t PART_START = 2048;         // 1 MiB alignment
constexpr uint32_t MAX_PART_SECTORS = 0x3F0000u; // keep FAT16 happy (~2 GiB)

// The one sector of our own in the gap between the GRUB embedding area
// and the partition: magic + the node name the installer was given. It
// is never part of the FAT16 volume, so a filesystem tool cannot see or
// clobber it, and the kernel reads it back with a single sector read
// when it boots from an installed disk.
constexpr uint32_t CFG_SECTOR = PART_START - 1;
constexpr const char* CFG_MAGIC = "VNUCFG1"; // 8 bytes, then the name
constexpr uint32_t CFG_NAME_OFF = 8;
constexpr uint32_t CFG_NAME_MAX = vnu::host::MAX;

// The config the installed GRUB loads. `module2 /boot/kernel.elf` is
// what makes the running kernel available as a Multiboot2 module, so a
// second install copies the exact same kernel.
const char* GRUB_CFG =
    "set timeout=0\n"
    "set default=0\n"
    "menuentry \"VNU\" {\n"
    "    multiboot2 /boot/kernel.elf\n"
    "    module2 /boot/kernel.elf\n"
    "    boot\n"
    "}\n";

void put32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

/* Is drive `d` one of ours? A single boot sector read is enough: the
 * installer stamps the FAT16 OEM name "VNUFS   " into it. The
 * freestanding kernel has no memcmp, hence the byte compare by hand. */
bool is_vnu_volume(int d, uint8_t* scratch)
{
    if (!vnu::ata::read_sectors(d, PART_START, 1, scratch))
        return false;
    for (int i = 0; i < 8; ++i) {
        if (scratch[3 + i] != static_cast<uint8_t>("VNUFS   "[i]))
            return false;
    }
    return true;
}

} // namespace

namespace vnu::install {

int disk_count()
{
    return vnu::ata::drive_count();
}

int installed_drive()
{
    /* We only look for the OEM name the installer stamped, we never walk
     * the volume. */
    uint8_t bs[512];
    for (int d = 0; d < vnu::ata::drive_count(); ++d)
        if (is_vnu_volume(d, bs))
            return d;
    return -1;
}

int installed_hostname(char* out, uint32_t max)
{
    if (!out || max == 0)
        return -1;
    out[0] = 0;
    /* The same disk /proc/boot reports, so the name on screen and the
     * name on the disk can never come from two different volumes. */
    int d = installed_drive();
    if (d < 0)
        return -1;
    uint8_t cfg[512];
    if (!vnu::ata::read_sectors(d, CFG_SECTOR, 1, cfg))
        return -1;
    for (int i = 0; i < 8; ++i) {
        if (cfg[i] != static_cast<uint8_t>(CFG_MAGIC[i]))
            return -1; /* installed, but by a build without names */
    }
    uint32_t n = 0;
    while (n < CFG_NAME_MAX - 1 && CFG_NAME_OFF + n < 512 &&
           cfg[CFG_NAME_OFF + n] != 0) {
        out[n] = static_cast<char>(cfg[CFG_NAME_OFF + n]);
        ++n;
    }
    out[n] = 0;
    if (n == 0 || n >= max)
        return -1;
    return 0;
}

int install(int drive, uint32_t size_mib, const char* hostname)
{
    if (drive < 0 || drive >= vnu::ata::drive_count())
        return -22; // EINVAL
    /* The name is part of the install, not a nicety: the disk has to
     * boot as a machine with a name. Same rule as vnu::host::valid(),
     * which also documents the accepted characters. */
    if (!vnu::host::valid(hostname))
        return -22; // EINVAL

    uint32_t total = vnu::ata::drive(drive).sectors;
    if (total <= PART_START + 8192)
        return -28; // ENOSPC: disk far too small

    // Partition size: explicit MiB (1 MiB = 2048 sectors), or the whole
    // usable disk when size_mib is 0. An explicit size that cannot fit
    // the disk is an error; an explicit size above the FAT16 ceiling is
    // clamped down to it (same ceiling the "whole disk" path uses).
    uint32_t part_sectors;
    if (size_mib == 0) {
        part_sectors = total - PART_START;
        if (part_sectors > MAX_PART_SECTORS)
            part_sectors = MAX_PART_SECTORS;
    } else {
        const uint64_t wanted = static_cast<uint64_t>(size_mib) * 2048;
        if (wanted < 2048 || wanted > static_cast<uint64_t>(total - PART_START))
            return -22; // EINVAL
        part_sectors = static_cast<uint32_t>(wanted > MAX_PART_SECTORS
                                                 ? MAX_PART_SECTORS
                                                 : wanted);
    }

    // 1) MBR: the GRUB boot image plus our partition table.
    vnu::tty::write_cstr("install: writing boot loader\n");
    uint8_t mbr[512];
    const uint8_t* boot = vnu_install_boot_img;
    uint32_t boot_len = static_cast<uint32_t>(vnu_install_boot_img_end - boot);
    for (int i = 0; i < 512; ++i)
        mbr[i] = (i < static_cast<int>(boot_len)) ? boot[i] : 0;
    mbr[0x64] = 0x80; // BIOS boot drive: first hard disk

    for (int i = 0; i < 16 * 4; ++i)
        mbr[0x1be + i] = 0;
    mbr[0x1be + 0] = 0x80;                  // bootable
    mbr[0x1be + 1] = 0xFE;                  // CHS start (dummy, LBA used)
    mbr[0x1be + 2] = 0xFF;
    mbr[0x1be + 3] = 0xFF;
    mbr[0x1be + 4] = 0x0C;                  // FAT32 LBA (FAT16 also accepted)
    mbr[0x1be + 5] = 0xFE;                  // CHS end (dummy)
    mbr[0x1be + 6] = 0xFF;
    mbr[0x1be + 7] = 0xFF;
    put32(mbr + 0x1be + 8, PART_START);
    put32(mbr + 0x1be + 12, part_sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (!vnu::ata::write_sectors(drive, 0, 1, mbr))
        return -5; // EIO

    // 2) GRUB core image in the embedding area right after the MBR.
    const uint8_t* core = vnu_install_core_img;
    uint32_t core_len = static_cast<uint32_t>(vnu_install_core_img_end - core);
    uint32_t core_sec = (core_len + 511) / 512;
    if (1 + core_sec >= PART_START) {
        vnu::tty::write_cstr("install: boot loader too large for embedding area\n");
        return -28;
    }
    vnu::tty::write_cstr("install: writing GRUB core image\n");
    if (!vnu::ata::write_sectors(drive, 1, core_sec, core))
        return -5;

    // 3) Our config sector: the node name for the next boot.
    {
        uint8_t cfg[512];
        for (int i = 0; i < 512; ++i)
            cfg[i] = 0;
        for (int i = 0; i < 8; ++i)
            cfg[i] = static_cast<uint8_t>(CFG_MAGIC[i]);
        uint32_t n = 0;
        for (; hostname[n] && n < CFG_NAME_MAX - 1; ++n)
            cfg[CFG_NAME_OFF + n] = static_cast<uint8_t>(hostname[n]);
        vnu::tty::write_cstr("install: recording host name '");
        vnu::tty::write_cstr(hostname);
        vnu::tty::write_cstr("'\n");
        if (!vnu::ata::write_sectors(drive, CFG_SECTOR, 1, cfg))
            return -5; // EIO
    }

    // 4) Filesystem with the running kernel.
    uint32_t ksize = 0;
    const void* kernel = vnu::mboot::first_module(ksize);
    if (!kernel || ksize == 0) {
        vnu::tty::write_cstr("install: no kernel module loaded; "
                             "boot with `module2 /boot/kernel.elf`\n");
        return -2; // ENOENT
    }
    vnu::tty::write_cstr("install: creating filesystem and copying kernel\n");
    if (!vnu::fat16::create_boot_volume(drive, PART_START, part_sectors,
                                        kernel, ksize, GRUB_CFG))
        return -5;

    vnu::tty::write_cstr("install: done\n");
    return 0;
}

} // namespace vnu::install
