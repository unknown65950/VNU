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

} // namespace vnu::mboot
