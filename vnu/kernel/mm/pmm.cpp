#include <vnu/pmm.h>

extern "C" void* memset(void* dst, int val, unsigned long count);

/* The linker's __bss_end: the first byte past the kernel's zeroed
 * globals. Its address is the one thing the pool base cannot be a
 * hand-written constant, because a constant that lags behind a growing
 * .bss does not fail cleanly - it hands out the kernel's own memory. */
extern "C" uint8_t __bss_end[];

namespace {
/* The lowest address the pool can ever start at: the .bss itself, which
 * linker.ld places at 0xA00000. The bitmap is sized for a pool that
 * starts here, so whatever init() works out fits in it. */
constexpr uint32_t POOL_MIN_BASE = 0x00A00000u;
constexpr uint32_t MAX_POOL_FRAMES =
    (vnu::pmm::POOL_END - POOL_MIN_BASE) / vnu::pmm::FRAME_SIZE;
constexpr uint32_t BITMAP_WORDS = (MAX_POOL_FRAMES + 31) / 32;
uint32_t g_bitmap[BITMAP_WORDS];

/* The pool as init() found it: everything between the kernel's .bss and
 * POOL_END. Both are set once, before the first allocation. */
uint32_t g_pool_base = 0;
uint32_t g_pool_frames = 0;
} // namespace

namespace vnu::pmm {

void init()
{
    for (uint32_t i = 0; i < BITMAP_WORDS; ++i)
        g_bitmap[i] = 0;
    /* Everything above the kernel's own globals, up to the last usable
     * byte of RAM. A .bss that grew by 4.7 MiB (the desktop's buffers
     * left it for the pool) hands those frames straight back, with no
     * constant here to keep in step. */
    const uint32_t bss_end = reinterpret_cast<uint32_t>(__bss_end);
    uint32_t base = (bss_end + FRAME_SIZE - 1u) & ~(FRAME_SIZE - 1u);
    if (base < POOL_MIN_BASE)
        base = POOL_MIN_BASE;
    g_pool_base = base;
    g_pool_frames = base < POOL_END ? (POOL_END - base) / FRAME_SIZE : 0;
}

uint32_t alloc_frame()
{
    for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
        if (g_bitmap[w] == 0xFFFFFFFFu)
            continue;
        for (uint32_t b = 0; b < 32; ++b) {
            uint32_t idx = w * 32 + b;
            if (idx >= g_pool_frames)
                break;
            if (!(g_bitmap[w] & (1u << b))) {
                g_bitmap[w] |= (1u << b);
                uint32_t phys = g_pool_base + idx * FRAME_SIZE;
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
    if (n == 0 || n > g_pool_frames)
        return 0;
    for (uint32_t i = 0; i + n <= g_pool_frames; ++i) {
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
        uint32_t phys = g_pool_base + i * FRAME_SIZE;
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
    if (phys_addr < g_pool_base || phys_addr >= POOL_END)
        return;
    uint32_t idx = (phys_addr - g_pool_base) / FRAME_SIZE;
    g_bitmap[idx / 32] &= ~(1u << (idx % 32));
}

uint32_t total_frames()
{
    return g_pool_frames;
}

uint32_t free_frames()
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < g_pool_frames; ++i)
        if (g_bitmap[i / 32] & (1u << (i % 32)))
            ++used;
    return g_pool_frames - used;
}

} // namespace vnu::pmm
