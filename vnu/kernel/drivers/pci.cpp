#include <vnu/pci.h>

namespace {

void outl(uint16_t port, uint32_t val)
{
    asm volatile("outl %0,%1" : : "a"(val), "Nd"(port));
}

uint32_t inl(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

uint32_t config_addr(uint8_t bus, uint8_t dev, uint8_t func,
                          uint16_t reg)
{
    return 0x80000000u | (static_cast<uint32_t>(bus) << 16) |
           (static_cast<uint32_t>(dev) << 11) | (static_cast<uint32_t>(func) << 8) |
           (static_cast<uint32_t>(reg) & 0xFCu);
}

} // namespace

namespace vnu::pci {

uint32_t read_dword(Address a, uint16_t reg)
{
    outl(0xCF8, config_addr(a.bus, a.dev, a.func, reg));
    return inl(0xCFC);
}

void write_dword(Address a, uint16_t reg, uint32_t val)
{
    outl(0xCF8, config_addr(a.bus, a.dev, a.func, reg));
    outl(0xCFC, val);
}

uint16_t read_word(Address a, uint16_t reg)
{
    /* Align to the containing dword; the byte offset's bit 1 picks which
     * half of that dword carries the 16-bit field (PCI config space is
     * byte-addressed, but 0xCFC always returns full dwords). */
    uint32_t d = read_dword(a, static_cast<uint16_t>(reg & ~3u));
    return static_cast<uint16_t>((reg & 2u) ? d >> 16 : d & 0xFFFFu);
}

uint16_t vendor(Address a)
{
    return read_word(a, 0);
}

uint16_t device(Address a)
{
    return read_word(a, 2);
}

bool find(uint16_t vendor_id, uint16_t device_id, Address& out)
{
    for (uint16_t bus = 0; bus <= 7; ++bus) {
        for (uint16_t dev = 0; dev <= 31; ++dev) {
            for (uint16_t func = 0; func <= 7; ++func) {
                Address a{static_cast<uint8_t>(bus), static_cast<uint8_t>(dev),
                          static_cast<uint8_t>(func)};
                if (vendor(a) != 0xFFFF) {
                    if (vendor(a) == vendor_id && device(a) == device_id) {
                        out = a;
                        return true;
                    }
                    /* Skip the ""sub-devices"" of multi-function... nothing
                     * more to scan — a bridge's secondary bus is handled by
                     * the outer bus loop, which is plenty for QEMU. */
                    if ((read_word(a, 14) & 0x80) == 0)
                        break; /* func 0 non-multifunction: no func 1-7 */
                }
            }
        }
    }
    return false;
}

uint32_t bar_size(Address a, int index)
{
    uint16_t reg = static_cast<uint16_t>(0x10 + index * 4);
    uint32_t orig = read_dword(a, reg);
    write_dword(a, reg, 0xFFFFFFFF);
    uint32_t mask = read_dword(a, reg);
    write_dword(a, reg, orig);
    uint32_t aligned = mask & 0xFFFFFFF0u; /* strip decoded type bits */
    if (aligned == 0)
        return 0;
    return (~aligned) + 1u; /* two's complement = BAR size */
}

} // namespace vnu::pci