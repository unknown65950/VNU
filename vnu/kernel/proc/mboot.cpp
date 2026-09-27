#include <vnu/mboot.h>

namespace {
uint32_t g_info = 0;
}

namespace vnu::mboot {

void set_info(uint32_t addr)
{
    g_info = addr;
}

const void* first_module(uint32_t& size)
{
    size = 0;
    if (!g_info)
        return nullptr;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_info);
    uint32_t total = *reinterpret_cast<const uint32_t*>(p);
    const uint8_t* end = p + total;

    p += 8; // skip total_size + reserved
    while (p + 8 <= end) {
        uint32_t type = *reinterpret_cast<const uint32_t*>(p);
        uint32_t tsz = *reinterpret_cast<const uint32_t*>(p + 4);
        if (type == 0)
            break;
        if (tsz < 8)
            break;
        if (type == 3 && tsz >= 16) { // Multiboot2 module tag
            uint32_t start = *reinterpret_cast<const uint32_t*>(p + 8);
            uint32_t stop = *reinterpret_cast<const uint32_t*>(p + 12);
            size = stop - start;
            return reinterpret_cast<const void*>(start);
        }
        p += (tsz + 7) & ~7u; // tags are 8-byte aligned
    }
    return nullptr;
}

uint32_t ram_bytes()
{
    /* Sum the "available" regions of the Multiboot2 memory map, so
     * userspace can be told how much memory the machine really has —
     * the PMM pool is only a slice of it. 0 when the boot loader gave
     * us no map. */
    if (!g_info)
        return 0;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_info);
    uint32_t total = *reinterpret_cast<const uint32_t*>(p);
    const uint8_t* end = p + total;

    p += 8;
    while (p + 8 <= end) {
        uint32_t type = *reinterpret_cast<const uint32_t*>(p);
        uint32_t tsz = *reinterpret_cast<const uint32_t*>(p + 4);
        if (type == 0 || tsz < 8)
            break;
        if (type == 6) { // memory map: 24-byte entries after the 16-byte header
            const uint8_t* e = p + 16;
            const uint8_t* eend = p + tsz;
            uint64_t sum = 0;
            /* Entry layout: base (8) | length (8) | type (4) | zero (4). */
            while (e + 24 <= eend) {
                uint32_t len_lo = *reinterpret_cast<const uint32_t*>(e + 8);
                uint32_t len_hi = *reinterpret_cast<const uint32_t*>(e + 12);
                uint32_t kind = *reinterpret_cast<const uint32_t*>(e + 16);
                if (kind == 1)
                    sum += (static_cast<uint64_t>(len_hi) << 32) | len_lo;
                e += 24;
            }
            /* 32-bit address space: saturate rather than lie. */
            return sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(sum);
        }
        p += (tsz + 7) & ~7u;
    }
    return 0;
}

} // namespace vnu::mboot
