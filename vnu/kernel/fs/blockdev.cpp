#include <vnu/blockdev.h>
#include <vnu/ata.h>

namespace {

using vnu::blockdev::Device;
using vnu::blockdev::MAX_DEVICES;
using vnu::blockdev::NAME_CAP;

Device g_devs[MAX_DEVICES];
int g_count = 0;

void copy_name(char* d, const char* s)
{
    int i = 0;
    for (; s[i] && i < NAME_CAP - 1; ++i)
        d[i] = s[i];
    d[i] = 0;
}

uint32_t le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void add(bool whole, uint8_t drive, uint32_t lba, uint32_t sectors, const char* name)
{
    if (g_count >= MAX_DEVICES)
        return;
    Device& d = g_devs[g_count++];
    d.whole = whole;
    d.drive = drive;
    d.lba = lba;
    d.sectors = sectors;
    copy_name(d.name, name);
}

} // namespace

namespace vnu::blockdev {

void init()
{
    g_count = 0;
    const char letters[] = "abcd";
    for (int dr = 0; dr < vnu::ata::drive_count(); ++dr) {
        const uint32_t total = vnu::ata::drive(dr).sectors;
        if (total == 0)
            continue;
        char name[NAME_CAP];
        name[0] = 'h';
        name[1] = 'd';
        name[2] = letters[dr < 4 ? dr : 0];
        name[3] = 0;
        add(true, static_cast<uint8_t>(dr), 0, total, name);

        /* A disk whose first sector does not carry the MBR signature is
         * not partitioned: the whole-drive entry above is all it gets. */
        uint8_t mbr[512];
        if (!vnu::ata::read_sectors(dr, 0, 1, mbr))
            continue;
        if (mbr[510] != 0x55 || mbr[511] != 0xAA)
            continue;

        for (int slot = 0; slot < 4; ++slot) {
            const uint8_t* e = mbr + 0x1BE + slot * 16;
            if (e[4] == 0)
                continue; /* empty partition slot */
            uint32_t lba = le32(e + 8);
            uint32_t sectors = le32(e + 12);
            if (sectors == 0 || lba >= total)
                continue;
            if (static_cast<uint64_t>(lba) + sectors > total)
                sectors = total - lba;

            /* "hda" + decimal(1-based slot), e.g. hda2. */
            char pname[NAME_CAP];
            int k = 0;
            for (; name[k] && k < NAME_CAP - 4; ++k)
                pname[k] = name[k];
            char digs[8];
            int dl = 0;
            uint32_t v = static_cast<uint32_t>(slot + 1);
            do {
                digs[dl++] = static_cast<char>('0' + v % 10);
                v /= 10;
            } while (v);
            for (int i = dl - 1; i >= 0; --i)
                pname[k++] = digs[i];
            pname[k] = 0;
            add(false, static_cast<uint8_t>(dr), lba, sectors, pname);
        }
    }
}

int count() { return g_count; }

const Device& at(int idx)
{
    static Device empty{};
    if (idx < 0 || idx >= g_count)
        return empty;
    return g_devs[idx];
}

int find(const char* name)
{
    for (int i = 0; i < g_count; ++i) {
        const char* a = g_devs[i].name;
        int k = 0;
        while (a[k] && name[k] && a[k] == name[k])
            ++k;
        if (!a[k] && !name[k])
            return i;
    }
    return -1;
}

uint32_t sector_count(int idx)
{
    if (idx < 0 || idx >= g_count)
        return 0;
    return g_devs[idx].sectors;
}

uint32_t sector_size(int idx)
{
    return sector_count(idx) ? sector_count(idx) * 512u : 0;
}

int read(int idx, uint64_t off, void* buf, uint32_t len)
{
    if (idx < 0 || idx >= g_count || !buf)
        return -1;
    const Device& d = g_devs[idx];
    const uint64_t total = static_cast<uint64_t>(d.sectors) * 512;
    if (off >= total)
        return 0;
    if (static_cast<uint64_t>(len) > total - off)
        len = static_cast<uint32_t>(total - off);

    auto* out = static_cast<uint8_t*>(buf);
    uint32_t done = 0;
    uint32_t lba = d.lba + static_cast<uint32_t>(off / 512);
    uint32_t bo = static_cast<uint32_t>(off % 512);

    if (bo != 0) {
        uint8_t s[512];
        if (!vnu::ata::read_sectors(d.drive, lba, 1, s))
            return -1;
        uint32_t n = 512 - bo;
        if (n > len)
            n = len;
        for (uint32_t i = 0; i < n; ++i)
            out[i] = s[bo + i];
        out += n;
        len -= n;
        done += n;
        ++lba;
    }
    if (len >= 512) {
        uint32_t ns = len / 512;
        if (!vnu::ata::read_sectors(d.drive, lba, ns, out))
            return -1;
        uint32_t nb = ns * 512;
        out += nb;
        len -= nb;
        done += nb;
        lba += ns;
    }
    if (len != 0) {
        uint8_t s[512];
        if (!vnu::ata::read_sectors(d.drive, lba, 1, s))
            return -1;
        for (uint32_t i = 0; i < len; ++i)
            out[i] = s[i];
        done += len;
    }
    return static_cast<int>(done);
}

int write(int idx, uint64_t off, const void* buf, uint32_t len)
{
    if (idx < 0 || idx >= g_count || !buf)
        return -1;
    const Device& d = g_devs[idx];
    const uint64_t total = static_cast<uint64_t>(d.sectors) * 512;
    if (off >= total)
        return 0;
    if (static_cast<uint64_t>(len) > total - off)
        len = static_cast<uint32_t>(total - off);

    const auto* in = static_cast<const uint8_t*>(buf);
    uint32_t done = 0;
    uint32_t lba = d.lba + static_cast<uint32_t>(off / 512);
    uint32_t bo = static_cast<uint32_t>(off % 512);

    if (bo != 0) {
        uint8_t s[512];
        if (!vnu::ata::read_sectors(d.drive, lba, 1, s))
            return -1;
        uint32_t n = 512 - bo;
        if (n > len)
            n = len;
        for (uint32_t i = 0; i < n; ++i)
            s[bo + i] = in[i];
        if (!vnu::ata::write_sectors(d.drive, lba, 1, s))
            return -1;
        in += n;
        len -= n;
        done += n;
        ++lba;
    }
    if (len >= 512) {
        uint32_t ns = len / 512;
        if (!vnu::ata::write_sectors(d.drive, lba, ns, in))
            return -1;
        uint32_t nb = ns * 512;
        in += nb;
        len -= nb;
        done += nb;
        lba += ns;
    }
    if (len != 0) {
        uint8_t s[512];
        if (!vnu::ata::read_sectors(d.drive, lba, 1, s))
            return -1;
        for (uint32_t i = 0; i < len; ++i)
            s[i] = in[i];
        if (!vnu::ata::write_sectors(d.drive, lba, 1, s))
            return -1;
        done += len;
    }
    return static_cast<int>(done);
}

namespace {

/* The FAT16 library's sector callbacks want the volume's own sector
 * numbering; the device already folds in the partition start, so this
 * only converts sectors to bytes. One context per table slot keeps the
 * `ctx` pointer valid without per-call allocation. */
struct Ctx {
    int idx;
};

Ctx g_ctx[MAX_DEVICES];

int dev_read(void* ctx, uint32_t lba, uint32_t count, void* buf)
{
    const int idx = static_cast<Ctx*>(ctx)->idx;
    return read(idx, static_cast<uint64_t>(lba) * 512, buf, count * 512) < 0 ? -1 : 0;
}

int dev_write(void* ctx, uint32_t lba, uint32_t count, const void* buf)
{
    const int idx = static_cast<Ctx*>(ctx)->idx;
    return write(idx, static_cast<uint64_t>(lba) * 512, buf, count * 512) < 0 ? -1 : 0;
}

int dev_flush(void*)
{
    return 0;
}

} // namespace

vnu::fat16::Device fat16_device(int idx)
{
    vnu::fat16::Device d{};
    if (idx < 0 || idx >= g_count)
        return d;
    g_ctx[idx].idx = idx;
    d.ctx = &g_ctx[idx];
    d.read = dev_read;
    d.write = dev_write;
    d.flush = dev_flush;
    return d;
}

} // namespace vnu::blockdev
