#include <vnu/ata.h>

namespace {

inline void outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    asm volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

inline void outw(uint16_t port, uint16_t val)
{
    asm volatile("outw %0,%1" : : "a"(val), "Nd"(port));
}

constexpr uint16_t PRIMARY_IO = 0x1F0;
constexpr uint16_t PRIMARY_CTRL = 0x3F6;
constexpr uint16_t SECONDARY_IO = 0x170;
constexpr uint16_t SECONDARY_CTRL = 0x376;

// Register offsets from the channel's I/O base.
constexpr uint16_t R_DATA = 0;
constexpr uint16_t R_ERROR = 1;
constexpr uint16_t R_FEATURES = 1;
constexpr uint16_t R_SECCOUNT = 2;
constexpr uint16_t R_LBA0 = 3;
constexpr uint16_t R_LBA1 = 4;
constexpr uint16_t R_LBA2 = 5;
constexpr uint16_t R_DRIVE = 6;
constexpr uint16_t R_STATUS = 7;
constexpr uint16_t R_COMMAND = 7;
// Control block (alt status has the same layout as status but no side
// effects — reading the regular status clears the pending IRQ).
constexpr uint16_t R_ALTSTATUS = 0;
constexpr uint16_t R_CONTROL = 2;

constexpr uint8_t ST_BSY = 0x80;
constexpr uint8_t ST_DRDY = 0x40;
constexpr uint8_t ST_DF = 0x20;
constexpr uint8_t ST_DRQ = 0x08;
constexpr uint8_t ST_ERR = 0x01;

constexpr uint8_t CMD_READ_PIO = 0x20;
constexpr uint8_t CMD_WRITE_PIO = 0x30;
constexpr uint8_t CMD_FLUSH = 0xE7;
constexpr uint8_t CMD_IDENTIFY = 0xEC;

/* A drive needs ~400ns between selecting it and the status register
 * being valid. Reading the alternate status four times is the classic
 * portable way to burn that delay on a machine without a usable timer. */
void io_delay(uint16_t ctrl)
{
    inb(ctrl + R_ALTSTATUS);
    inb(ctrl + R_ALTSTATUS);
    inb(ctrl + R_ALTSTATUS);
    inb(ctrl + R_ALTSTATUS);
}

enum class Poll { Ready, Error, Timeout };

/* Wait until BSY clears. `want_drq` then waits for DRQ to assert. */
Poll wait_ready(uint16_t ctrl, bool want_drq)
{
    for (int i = 0; i < 1000000; ++i) {
        uint8_t st = inb(ctrl + R_ALTSTATUS);
        if (st & ST_BSY)
            continue;
        if (st & (ST_DF | ST_ERR))
            return Poll::Error;
        if (!want_drq || (st & ST_DRQ))
            return Poll::Ready;
    }
    return Poll::Timeout;
}

struct HwDrive {
    bool present;
    bool lba28;
    bool slave;
    uint16_t io;
    uint16_t ctrl;
    uint32_t sectors;
    char model[41];
};

HwDrive g_drives[4];
int g_count = 0;

/* Identify one (channel, slave) pair. Returns true if an ATA disk (not
 * ATAPI, not empty) answered. */
bool probe(uint16_t io, uint16_t ctrl, bool slave, HwDrive& out)
{
    out = HwDrive{};
    out.io = io;
    out.ctrl = ctrl;
    out.slave = slave;

    outb(io + R_DRIVE, static_cast<uint8_t>(0xA0 | (slave ? 0x10 : 0x00)));
    io_delay(ctrl);

    /* Some controllers report "no device" only here: a zero status and
     * zero LBA registers after a select mean there is nothing attached. */
    uint8_t st = inb(io + R_STATUS);
    if (st == 0)
        return false;

    outb(io + R_SECCOUNT, 0);
    outb(io + R_LBA0, 0);
    outb(io + R_LBA1, 0);
    outb(io + R_LBA2, 0);
    outb(io + R_COMMAND, CMD_IDENTIFY);
    io_delay(ctrl);

    st = inb(io + R_STATUS);
    if (st == 0)
        return false;

    if (wait_ready(ctrl, true) != Poll::Ready) {
        /* No DRQ: either not an ATA device (e.g. ATAPI packet device)
         * or an error. Either way we don't support it here. */
        return false;
    }

    /* If these are non-zero, this is not a plain ATA disk. */
    if (inb(io + R_LBA1) != 0 || inb(io + R_LBA2) != 0)
        return false;

    uint16_t ident[256];
    for (int i = 0; i < 256; ++i)
        ident[i] = inw(io + R_DATA);

    bool lba28 = (ident[49] & (1u << 9)) != 0;
    uint32_t sectors = static_cast<uint32_t>(ident[60]) |
                       (static_cast<uint32_t>(ident[61]) << 16);
    if (!lba28 || sectors == 0)
        return false;

    /* Model is words 27..46, big-endian per word (byte-swapped). */
    for (int i = 0; i < 20; ++i) {
        uint16_t w = ident[27 + i];
        out.model[i * 2] = static_cast<char>(w >> 8);
        out.model[i * 2 + 1] = static_cast<char>(w & 0xFF);
    }
    out.model[40] = '\0';
    for (int i = 39; i >= 0 && out.model[i] == ' '; --i)
        out.model[i] = '\0';

    out.lba28 = true;
    out.sectors = sectors;
    out.present = true;
    return true;
}

bool issue(int idx, uint32_t lba, uint32_t count, uint8_t cmd)
{
    if (idx < 0 || idx >= g_count)
        return false;
    HwDrive& d = g_drives[idx];
    if (!d.present)
        return false;
    if (count == 0 || count > 256)
        return false;
    /* LBA28 range: last addressed sector must stay below 2^28. */
    if (lba + count > 0x10000000u)
        return false;

    outb(d.io + R_DRIVE, static_cast<uint8_t>(
        0xE0 | (d.slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F)));
    io_delay(d.ctrl);
    /* A zero sector count here means 256 sectors; keep it literal. */
    outb(d.io + R_SECCOUNT, static_cast<uint8_t>(count == 256 ? 0 : count));
    outb(d.io + R_LBA0, static_cast<uint8_t>(lba & 0xFF));
    outb(d.io + R_LBA1, static_cast<uint8_t>((lba >> 8) & 0xFF));
    outb(d.io + R_LBA2, static_cast<uint8_t>((lba >> 16) & 0xFF));
    outb(d.io + R_COMMAND, cmd);
    return true;
}

} // namespace

namespace vnu::ata {

void init()
{
    g_count = 0;
    HwDrive tmp;
    const struct { uint16_t io, ctrl; bool slave; } slots[4] = {
        { PRIMARY_IO, PRIMARY_CTRL, false },
        { PRIMARY_IO, PRIMARY_CTRL, true },
        { SECONDARY_IO, SECONDARY_CTRL, false },
        { SECONDARY_IO, SECONDARY_CTRL, true },
    };
    for (const auto& s : slots) {
        if (probe(s.io, s.ctrl, s.slave, tmp)) {
            g_drives[g_count] = tmp;
            ++g_count;
            if (g_count >= MAX_DRIVES)
                break;
        }
    }
}

int drive_count()
{
    return g_count;
}

const DriveInfo& drive(int idx)
{
    /* A single static result lets us hand back a reference without
     * exposing the internal HwDrive layout; the caller only reads it. */
    static DriveInfo info;
    if (idx < 0 || idx >= g_count) {
        info = DriveInfo{};
        return info;
    }
    const HwDrive& d = g_drives[idx];
    info.present = d.present;
    info.lba28 = d.lba28;
    info.sectors = d.sectors;
    info.size_mib = d.sectors / 2048;
    for (int i = 0; i < 41; ++i)
        info.model[i] = d.model[i];
    return info;
}

bool read_sectors(int idx, uint32_t lba, uint32_t count, void* buf)
{
    if (!buf)
        return false;
    uint8_t* p = static_cast<uint8_t*>(buf);

    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;
        if (!issue(idx, lba, chunk, CMD_READ_PIO))
            return false;

        for (uint32_t s = 0; s < chunk; ++s) {
            if (wait_ready(g_drives[idx].ctrl, true) != Poll::Ready)
                return false;
            for (int i = 0; i < 256; ++i) {
                uint16_t w = inw(g_drives[idx].io + R_DATA);
                p[0] = static_cast<uint8_t>(w & 0xFF);
                p[1] = static_cast<uint8_t>(w >> 8);
                p += 2;
            }
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool write_sectors(int idx, uint32_t lba, uint32_t count, const void* buf)
{
    if (!buf)
        return false;
    const uint8_t* p = static_cast<const uint8_t*>(buf);

    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;
        if (!issue(idx, lba, chunk, CMD_WRITE_PIO))
            return false;

        for (uint32_t s = 0; s < chunk; ++s) {
            if (wait_ready(g_drives[idx].ctrl, true) != Poll::Ready)
                return false;
            for (int i = 0; i < 256; ++i) {
                uint16_t w = static_cast<uint16_t>(p[0]) |
                             (static_cast<uint16_t>(p[1]) << 8);
                outw(g_drives[idx].io + R_DATA, w);
                p += 2;
            }
            /* Let the drive absorb the sector before we touch it again:
             * DRQ stays asserted right after the last data word, so wait
             * for BSY to clear (and the transfer to be consumed) before
             * handing over the next sector. */
            if (wait_ready(g_drives[idx].ctrl, false) != Poll::Ready)
                return false;
        }
        lba += chunk;
        count -= chunk;
    }
    return flush(idx);
}

bool flush(int idx)
{
    if (idx < 0 || idx >= g_count)
        return false;
    HwDrive& d = g_drives[idx];
    outb(d.io + R_COMMAND, CMD_FLUSH);
    io_delay(d.ctrl);
    return wait_ready(d.ctrl, false) == Poll::Ready;
}

} // namespace vnu::ata
