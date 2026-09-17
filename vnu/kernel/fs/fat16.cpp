#include <vnu/fat16.h>
#include <vnu/ata.h>

namespace {

constexpr uint32_t BPS = 512;
constexpr uint32_t NUM_FATS = 2;
constexpr uint32_t RESERVED = 1;
constexpr uint32_t ROOT_ENTRIES = 512;
constexpr uint32_t FAT16_EOC = 0xFFFF;
constexpr uint32_t FAT16_MAX_CLUSTER = 0xFFF4; // 0xFFF5..0xFFF7 reserved, 0xFFF8+ EOC

struct Vol {
    int drive;
    uint32_t part_lba;
    uint32_t total_sectors;
    uint32_t spc;              // sectors per cluster
    uint32_t fatsz;            // sectors per FAT
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector; // relative to partition start
    uint32_t first_root_sector;
    uint32_t first_data_sector;
    uint32_t next_cluster;
    uint32_t max_cluster;
};

void wr16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void wr32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

bool read_sector(Vol& v, uint32_t rel_sector, uint8_t* buf)
{
    return vnu::ata::read_sectors(v.drive, v.part_lba + rel_sector, 1, buf);
}
bool write_sector(Vol& v, uint32_t rel_sector, const uint8_t* buf)
{
    return vnu::ata::write_sectors(v.drive, v.part_lba + rel_sector, 1, buf);
}

bool fat_set(Vol& v, uint32_t cluster, uint16_t value)
{
    uint32_t off = cluster * 2;
    uint32_t sec = v.first_fat_sector + off / BPS;
    uint32_t byte = off % BPS;
    for (uint32_t copy = 0; copy < NUM_FATS; ++copy) {
        uint8_t buf[BPS];
        if (!read_sector(v, sec + copy * v.fatsz, buf))
            return false;
        wr16(buf + byte, value);
        if (!write_sector(v, sec + copy * v.fatsz, buf))
            return false;
    }
    return true;
}

bool zero_sectors(Vol& v, uint32_t rel_sector, uint32_t count)
{
    uint8_t buf[BPS];
    for (uint32_t i = 0; i < BPS; ++i)
        buf[i] = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!write_sector(v, rel_sector + i, buf))
            return false;
    return true;
}

uint32_t cluster_to_sector(Vol& v, uint32_t cluster)
{
    return v.first_data_sector + (cluster - 2) * v.spc;
}

// Allocate a run of `count` consecutive clusters starting at the bump
// pointer. Returns the first cluster, or 0 when the volume is full.
uint32_t alloc_run(Vol& v, uint32_t count)
{
    if (count == 0)
        return 0;
    uint32_t start = v.next_cluster;
    if (start + count - 1 > v.max_cluster)
        return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t val = (i + 1 < count) ? static_cast<uint16_t>(start + i + 1)
                                       : static_cast<uint16_t>(FAT16_EOC);
        if (!fat_set(v, start + i, val))
            return 0;
    }
    v.next_cluster = start + count;
    return start;
}

void make_entry(uint8_t* e, const char* name, const char* ext, uint8_t attr,
                uint32_t first_cluster, uint32_t size)
{
    for (int i = 0; i < 32; ++i)
        e[i] = 0;
    for (int i = 0; i < 8; ++i)
        e[i] = ' ';
    for (int i = 0; i < 3; ++i)
        e[8 + i] = ' ';
    int n = 0;
    for (; name[n] && n < 8; ++n)
        e[n] = static_cast<uint8_t>(name[n]);
    for (int i = 0; ext && ext[i] && i < 3; ++i)
        e[8 + i] = static_cast<uint8_t>(ext[i]);
    e[11] = attr;
    e[26] = static_cast<uint8_t>(first_cluster & 0xFF);
    e[27] = static_cast<uint8_t>((first_cluster >> 8) & 0xFF);
    wr32(e + 28, size);
}

// Directory entries live either in the fixed root region (cluster 0) or
// in a single allocated cluster. Both cases are a linear array of
// 32-byte slots; this returns the sector/offset of the first free slot,
// writing the entry there.
bool add_dir_entry(Vol& v, uint32_t dir_cluster, const uint8_t* entry)
{
    uint32_t total_bytes = (dir_cluster == 0) ? v.root_dir_sectors * BPS
                                              : v.spc * BPS;
    uint32_t base_sector = (dir_cluster == 0) ? v.first_root_sector
                                              : cluster_to_sector(v, dir_cluster);
    uint8_t buf[BPS];
    for (uint32_t sec = 0; sec < total_bytes / BPS; ++sec) {
        if (!read_sector(v, base_sector + sec, buf))
            return false;
        for (uint32_t off = 0; off + 32 <= BPS; off += 32) {
            if (buf[off] == 0x00 || buf[off] == 0xE5) {
                for (int i = 0; i < 32; ++i)
                    buf[off + i] = entry[i];
                return write_sector(v, base_sector + sec, buf);
            }
        }
    }
    return false; // directory full (never happens for our tiny tree)
}

// Fill a freshly allocated cluster with "." and ".." entries.
bool init_subdir(Vol& v, uint32_t cluster, uint32_t parent_cluster)
{
    uint8_t dot[32], dotdot[32];
    make_entry(dot, ".", nullptr, 0x10, cluster, 0);
    make_entry(dotdot, "..", nullptr, 0x10, parent_cluster, 0);
    uint8_t buf[BPS];
    for (uint32_t s = 0; s < v.spc; ++s) {
        for (int i = 0; i < 32; ++i)
            buf[i] = 0;
        if (s == 0) {
            for (int i = 0; i < 32; ++i) {
                buf[i] = dot[i];
                buf[32 + i] = dotdot[i];
            }
        }
        if (!write_sector(v, cluster_to_sector(v, cluster) + s, buf))
            return false;
    }
    return true;
}

bool write_file(Vol& v, const void* data, uint32_t size, uint32_t& first_out)
{
    uint32_t cluster_bytes = v.spc * BPS;
    uint32_t need = (size + cluster_bytes - 1) / cluster_bytes;
    if (need == 0)
        need = 1;
    uint32_t first = alloc_run(v, need);
    if (!first)
        return false;
    first_out = first;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint8_t buf[BPS];
    for (uint32_t c = 0; c < need; ++c) {
        uint32_t base = cluster_to_sector(v, first + c);
        for (uint32_t s = 0; s < v.spc; ++s) {
            uint32_t pos = c * cluster_bytes + s * BPS;
            uint32_t remain = (pos < size) ? size - pos : 0;
            uint32_t chunk = remain > BPS ? BPS : remain;
            for (uint32_t i = 0; i < chunk; ++i)
                buf[i] = p[pos + i];
            for (uint32_t i = chunk; i < BPS; ++i)
                buf[i] = 0;
            if (!write_sector(v, base + s, buf))
                return false;
        }
    }
    return true;
}

} // namespace

namespace vnu::fat16 {

bool create_boot_volume(int drive, uint32_t part_lba, uint32_t part_sectors,
                        const void* kernel, uint32_t kernel_size,
                        const char* grub_cfg)
{
    Vol v{};
    v.drive = drive;
    v.part_lba = part_lba;
    v.total_sectors = part_sectors;

    // Pick the smallest power-of-two cluster size that keeps the cluster
    // count inside FAT16's limit (it also has to stay above the FAT12/16
    // boundary, which is satisfied for any realistic target here).
    v.spc = 1;
    while (part_sectors / v.spc > 60000 && v.spc < 128)
        v.spc <<= 1;

    v.root_dir_sectors = (ROOT_ENTRIES * 32 + BPS - 1) / BPS;
    uint32_t fatsz = 1;
    for (;;) {
        uint32_t data = part_sectors - RESERVED - NUM_FATS * fatsz - v.root_dir_sectors;
        uint32_t clusters = data / v.spc;
        uint32_t need = ((clusters + 2) * 2 + BPS - 1) / BPS;
        if (need <= fatsz)
            break;
        fatsz = need;
        if (fatsz > 65535)
            return false;
    }
    v.fatsz = fatsz;
    uint32_t data_sectors = part_sectors - RESERVED - NUM_FATS * fatsz - v.root_dir_sectors;
    if (data_sectors < v.spc * 8)
        return false;
    uint32_t clusters = data_sectors / v.spc;
    // Below 4085 clusters the MS spec says the volume is FAT12, which
    // would make a spec-compliant reader misinterpret our 16-bit FAT.
    // Real install targets are far larger; reject anything that small.
    if (clusters < 4085 || clusters + 2 > FAT16_MAX_CLUSTER)
        return false;
    v.first_fat_sector = RESERVED;
    v.first_root_sector = RESERVED + NUM_FATS * fatsz;
    v.first_data_sector = v.first_root_sector + v.root_dir_sectors;
    v.next_cluster = 2;
    v.max_cluster = clusters + 1;

    // Boot sector.
    uint8_t bs[BPS];
    for (uint32_t i = 0; i < BPS; ++i)
        bs[i] = 0;
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    const char* oem = "VNUFS   ";
    for (int i = 0; i < 8; ++i)
        bs[3 + i] = static_cast<uint8_t>(oem[i]);
    wr16(bs + 11, BPS);
    bs[13] = static_cast<uint8_t>(v.spc);
    wr16(bs + 14, RESERVED);
    bs[16] = NUM_FATS;
    wr16(bs + 17, ROOT_ENTRIES);
    if (part_sectors < 65536) {
        wr16(bs + 19, static_cast<uint16_t>(part_sectors));
        wr32(bs + 32, 0);
    } else {
        wr16(bs + 19, 0);
        wr32(bs + 32, part_sectors);
    }
    bs[21] = 0xF8;
    wr16(bs + 22, static_cast<uint16_t>(v.fatsz));
    wr16(bs + 24, 63);
    wr16(bs + 26, 255);
    wr32(bs + 28, part_lba);
    bs[36] = 0x80;
    bs[38] = 0x29;
    wr32(bs + 39, 0x56554E55); // "VNUU"-ish volume id
    const char* label = "VNU        ";
    for (int i = 0; i < 11; ++i)
        bs[43 + i] = static_cast<uint8_t>(label[i]);
    const char* fstype = "FAT16   ";
    for (int i = 0; i < 8; ++i)
        bs[54 + i] = static_cast<uint8_t>(fstype[i]);
    bs[510] = 0x55;
    bs[511] = 0xAA;
    if (!write_sector(v, 0, bs))
        return false;

    // FAT[0]/FAT[1] sentinels, the rest zeroed.
    if (!zero_sectors(v, v.first_fat_sector, v.fatsz))
        return false;
    if (!zero_sectors(v, v.first_fat_sector + v.fatsz, v.fatsz))
        return false;
    {
        uint8_t buf[BPS];
        if (!read_sector(v, v.first_fat_sector, buf))
            return false;
        wr16(buf + 0, 0xFFF8);
        wr16(buf + 2, 0xFFFF);
        if (!write_sector(v, v.first_fat_sector, buf))
            return false;
        if (!read_sector(v, v.first_fat_sector + v.fatsz, buf))
            return false;
        wr16(buf + 0, 0xFFF8);
        wr16(buf + 2, 0xFFFF);
        if (!write_sector(v, v.first_fat_sector + v.fatsz, buf))
            return false;
    }

    // Root directory, with a volume-label entry so `search --label VNU`
    // works regardless of whether the reader looks at the BPB label or
    // the root directory entry.
    if (!zero_sectors(v, v.first_root_sector, v.root_dir_sectors))
        return false;
    {
        uint8_t e[32];
        make_entry(e, "VNU", nullptr, 0x08, 0, 0);
        if (!add_dir_entry(v, 0, e))
            return false;
    }

    // /boot
    uint32_t boot_dir = alloc_run(v, 1);
    if (!boot_dir || !init_subdir(v, boot_dir, 0))
        return false;
    {
        uint8_t e[32];
        make_entry(e, "BOOT", nullptr, 0x10, boot_dir, 0);
        if (!add_dir_entry(v, 0, e))
            return false;
    }

    // /boot/grub
    uint32_t grub_dir = alloc_run(v, 1);
    if (!grub_dir || !init_subdir(v, grub_dir, boot_dir))
        return false;
    {
        uint8_t e[32];
        make_entry(e, "GRUB", nullptr, 0x10, grub_dir, 0);
        if (!add_dir_entry(v, boot_dir, e))
            return false;
    }

    // /boot/kernel.elf
    {
        uint32_t first = 0;
        if (!write_file(v, kernel, kernel_size, first))
            return false;
        uint8_t e[32];
        make_entry(e, "KERNEL", "ELF", 0x20, first, kernel_size);
        if (!add_dir_entry(v, boot_dir, e))
            return false;
    }

    // /boot/grub/grub.cfg
    {
        uint32_t len = 0;
        while (grub_cfg[len])
            ++len;
        uint32_t first = 0;
        if (!write_file(v, grub_cfg, len, first))
            return false;
        uint8_t e[32];
        make_entry(e, "GRUB", "CFG", 0x20, first, len);
        if (!add_dir_entry(v, grub_dir, e))
            return false;
    }

    return true;
}

} // namespace vnu::fat16
