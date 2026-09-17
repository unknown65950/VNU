#include <vnu/pmm.h>

extern "C" void* memset(void* dst, int val, unsigned long count);

namespace {
constexpr uint32_t POOL_FRAMES = (vnu::pmm::POOL_END - vnu::pmm::POOL_BASE) / vnu::pmm::FRAME_SIZE;
constexpr uint32_t BITMAP_WORDS = (POOL_FRAMES + 31) / 32;
uint32_t g_bitmap[BITMAP_WORDS];
} // namespace

namespace vnu::pmm {

void init()
{
    for (uint32_t i = 0; i < BITMAP_WORDS; ++i)
        g_bitmap[i] = 0;
}

uint32_t alloc_frame()
{
    for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
        if (g_bitmap[w] == 0xFFFFFFFFu)
            continue;
        for (uint32_t b = 0; b < 32; ++b) {
            uint32_t idx = w * 32 + b;
            if (idx >= POOL_FRAMES)
                break;
            if (!(g_bitmap[w] & (1u << b))) {
                g_bitmap[w] |= (1u << b);
                uint32_t phys = POOL_BASE + idx * FRAME_SIZE;
                /* Identity-mapped by paging::init(), so this cast is
                 * valid as a virtual address too. */
                memset(reinterpret_cast<void*>(phys), 0, FRAME_SIZE);
                return phys;
            }
        }
    }
    return 0;
}

uint32_t alloc_contig(uint32_t n)
{
    if (n == 0 || n > POOL_FRAMES)
        return 0;
    for (uint32_t i = 0; i + n <= POOL_FRAMES; ++i) {
        bool free_run = true;
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t idx = i + k;
            if (g_bitmap[idx / 32] & (1u << (idx % 32))) {
                free_run = false;
                break;
            }
        }
        if (!free_run)
            continue;
        uint32_t phys = POOL_BASE + i * FRAME_SIZE;
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t idx = i + k;
            g_bitmap[idx / 32] |= (1u << (idx % 32));
            memset(reinterpret_cast<void*>(phys + k * FRAME_SIZE), 0, FRAME_SIZE);
        }
        return phys;
    }
    return 0;
}

void free_contig(uint32_t phys_addr, uint32_t n)
{
    for (uint32_t k = 0; k < n; ++k)
        free_frame(phys_addr + k * FRAME_SIZE);
}

void free_frame(uint32_t phys_addr)
{
    if (phys_addr < POOL_BASE || phys_addr >= POOL_END)
        return;
    uint32_t idx = (phys_addr - POOL_BASE) / FRAME_SIZE;
    g_bitmap[idx / 32] &= ~(1u << (idx % 32));
}

uint32_t total_frames()
{
    return POOL_FRAMES;
}

uint32_t free_frames()
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < POOL_FRAMES; ++i)
        if (g_bitmap[i / 32] & (1u << (i % 32)))
            ++used;
    return POOL_FRAMES - used;
}

} // namespace vnu::pmm
