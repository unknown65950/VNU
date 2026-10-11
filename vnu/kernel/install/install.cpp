#include <vnu/install.h>
#include <vnu/initrd.h>
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

// The installer is the only caller; the definition is with the ATA
// adapter at the bottom of this file.
namespace vnu::fat16 {
bool create_data_volume(int drive, uint32_t part_lba, uint32_t part_sectors);
}

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
// second install copies the exact same kernel. The initrd is only named
// when this boot actually carried one: a grub.cfg referring to a missing
// /boot/initrd.tar would leave the disk unbootable.
void make_grub_cfg(char* out, uint32_t cap)
{
    const char* head =
        "set timeout=0\n"
        "set default=0\n"
        "menuentry \"VNU\" {\n"
        "    multiboot2 /boot/kernel.elf\n"
        "    module2 /boot/kernel.elf\n";
    const char* initrd = "    module2 /boot/initrd.tar\n";
    const char* tail = "    boot\n}\n";
    uint32_t k = 0;
    auto put = [&](const char* s) {
        for (; *s && k + 1 < cap; ++s)
            out[k++] = *s;
    };
    put(head);
    if (vnu::initrd::present())
        put(initrd);
    put(tail);
    out[k] = 0;
}

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

    // Two partitions: the bootable FAT16 volume first, then the data
    // volume the kernel mounts at /mnt. With no explicit size the boot
    // volume takes half the usable disk, so the default install leaves
    // room for user files that survive a reboot; an explicit size names
    // the boot volume and the rest becomes the data volume. Either way
    // the boot volume is clamped to the FAT16 ceiling.
    uint32_t part_sectors;
    if (size_mib == 0) {
        part_sectors = (total - PART_START) / 2;
        part_sectors &= ~(2048u - 1); // 1 MiB alignment for the data start
        if (part_sectors > MAX_PART_SECTORS)
            part_sectors = MAX_PART_SECTORS;
        if (part_sectors < 8192)
            part_sectors = total - PART_START; // tiny disk: boot volume only
    } else {
        const uint64_t wanted = static_cast<uint64_t>(size_mib) * 2048;
        if (wanted < 2048 || wanted > static_cast<uint64_t>(total - PART_START))
            return -22; // EINVAL
        part_sectors = static_cast<uint32_t>(wanted > MAX_PART_SECTORS
                                                 ? MAX_PART_SECTORS
                                                 : wanted);
    }

    // The data volume, when the disk is big enough for a second FAT16
    // (a few thousand clusters at least). It ends the usable disk, and
    // is clamped like the boot volume so the FAT16 cluster count stays
    // inside the format's limit.
    const uint32_t data_start = PART_START + part_sectors;
    uint32_t data_sectors = total > data_start ? total - data_start : 0;
    if (data_sectors > MAX_PART_SECTORS)
        data_sectors = MAX_PART_SECTORS;
    if (data_sectors < 8192)
        data_sectors = 0; // too small: no data volume on this disk

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
    /* Second partition: the data volume. Slot left empty (type 0) when
     * the disk was too small for one. */
    if (data_sectors != 0) {
        mbr[0x1ce + 0] = 0x00;              // not bootable
        mbr[0x1ce + 1] = 0xFE;              // CHS start (dummy, LBA used)
        mbr[0x1ce + 2] = 0xFF;
        mbr[0x1ce + 3] = 0xFF;
        mbr[0x1ce + 4] = 0x06;              // FAT16
        mbr[0x1ce + 5] = 0xFE;              // CHS end (dummy)
        mbr[0x1ce + 6] = 0xFF;
        mbr[0x1ce + 7] = 0xFF;
        put32(mbr + 0x1ce + 8, data_start);
        put32(mbr + 0x1ce + 12, data_sectors);
    }
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
    char grub_cfg[512];
    make_grub_cfg(grub_cfg, sizeof(grub_cfg));
    const uint32_t initrd_size = vnu::initrd::image_size();
    vnu::tty::write_cstr("install: creating filesystem and copying kernel\n");
    if (!vnu::fat16::create_boot_volume(drive, PART_START, part_sectors,
                                        kernel, ksize,
                                        vnu::initrd::image(), initrd_size,
                                        grub_cfg))
        return -5;

    if (data_sectors != 0) {
        vnu::tty::write_cstr("install: creating data volume\n");
        if (!vnu::fat16::create_data_volume(drive, data_start, data_sectors))
            return -5;
    }

    vnu::tty::write_cstr("install: done\n");
    return 0;
}

} // namespace vnu::install

// The kernel-side half of the FAT16 header: format a bootable volume on an
// ATA drive and copy the kernel, the initrd and grub.cfg into it. The
// filesystem itself lives in fs/fat16.cpp and knows nothing about ATA, so
// this is where the two meet.
namespace vnu::fat16 {
namespace {

struct AtaDev {
    int drive;
};

int ata_read(void* ctx, uint32_t lba, uint32_t count, void* buf)
{
    return vnu::ata::read_sectors(static_cast<AtaDev*>(ctx)->drive, lba, count, buf) ? 0 : -1;
}

int ata_write(void* ctx, uint32_t lba, uint32_t count, const void* buf)
{
    return vnu::ata::write_sectors(static_cast<AtaDev*>(ctx)->drive, lba, count, buf) ? 0 : -1;
}

int ata_flush(void*)
{
    return 0;
}

bool write_file(Fs& fs, uint32_t dir, const char* name, const void* data, uint32_t size)
{
    if (size == 0)
        return create(fs, dir, name);
    uint32_t wrote = 0;
    return write(fs, dir, name, 0, data, size, &wrote) && wrote == size;
}

uint32_t cstr_len(const char* s)
{
    uint32_t n = 0;
    while (s && s[n])
        ++n;
    return n;
}

} // namespace

bool create_boot_volume(int drive, uint32_t part_lba, uint32_t part_sectors,
                        const void* kernel, uint32_t kernel_size,
                        const void* initrd, uint32_t initrd_size,
                        const char* grub_cfg)
{
    AtaDev ctx{drive};
    Device dev{&ctx, ata_read, ata_write, ata_flush};
    if (!format(dev, part_lba, part_sectors, "VNUFS   ", "VNU", 0x56554E55))
        return false;

    static uint8_t sector[SECTOR];
    Fs fs{};
    if (!mount(fs, dev, part_lba, sector))
        return false;

    if (!mkdir(fs, 0, "BOOT"))
        return false;
    uint32_t boot = 0, size = 0;
    uint8_t attr = 0;
    if (!resolve(fs, "BOOT", &boot, &size, &attr))
        return false;
    if (!mkdir(fs, boot, "GRUB"))
        return false;
    uint32_t grub = 0;
    if (!resolve(fs, "BOOT/GRUB", &grub, &size, &attr))
        return false;

    if (!write_file(fs, boot, "KERNEL.ELF", kernel, kernel_size))
        return false;
    if (initrd && initrd_size) {
        if (!write_file(fs, boot, "INITRD.TAR", initrd, initrd_size))
            return false;
    }
    if (!write_file(fs, grub, "GRUB.CFG", grub_cfg, cstr_len(grub_cfg)))
        return false;

    return sync(fs);
}

/* Empty FAT16 volume with the data label the kernel looks for at boot.
 * Unlike the boot volume there is nothing to copy in, so the volume is
 * mounted only to flush a consistent root directory and released. */
bool create_data_volume(int drive, uint32_t part_lba, uint32_t part_sectors)
{
    AtaDev ctx{drive};
    Device dev{&ctx, ata_read, ata_write, ata_flush};
    if (!format(dev, part_lba, part_sectors, "VNUFS   ", "VNUDATA", 0x44415441))
        return false;
    static uint8_t sector[SECTOR];
    Fs fs{};
    if (!mount(fs, dev, part_lba, sector))
        return false;
    return sync(fs);
}

} // namespace vnu::fat16
