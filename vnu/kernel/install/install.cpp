#include <vnu/install.h>
#include <vnu/ata.h>
#include <vnu/fat16.h>
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

} // namespace

namespace vnu::install {

int disk_count()
{
    return vnu::ata::drive_count();
}

int install(int drive)
{
    if (drive < 0 || drive >= vnu::ata::drive_count())
        return -22; // EINVAL

    uint32_t total = vnu::ata::drive(drive).sectors;
    if (total <= PART_START + 8192)
        return -28; // ENOSPC: disk far too small
    uint32_t part_sectors = total - PART_START;
    if (part_sectors > MAX_PART_SECTORS)
        part_sectors = MAX_PART_SECTORS;

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

    // 3) Filesystem with the running kernel.
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
