#include <vnu/paging.h>
#include <vnu/pmm.h>

extern "C" void* memset(void* dst, int val, unsigned long count);

namespace {

constexpr uint32_t PDE_PRESENT = 1u << 0;
constexpr uint32_t PDE_RW = 1u << 1;
constexpr uint32_t PTE_PRESENT = 1u << 0;
constexpr uint32_t PTE_RW = 1u << 1;

constexpr uint32_t IDENTITY_MB = 32; /* matches run.sh/build_iso.sh's -m 32 */
constexpr int NUM_IDENTITY_PDES = IDENTITY_MB / 4;

/* Stdvga's VBE linear framebuffer: a 16 MiB PCI BAR0 window at
 * 0xFD000000 (QEMU 11; found via `info pci`). Identity-mapped so
 * vga_gfx::present() can memcpy the desktop backbuffer straight into
 * VRAM. index 6 of dispi would give the address on real Bochs
 * hardware; older QEMUs used a fixed 0xE0000000 alias. */
constexpr uint32_t VBE_LFB_BASE = 0xFD000000u;
constexpr int VBE_LFB_PDE = VBE_LFB_BASE >> 22; /* = 1012 */

/* Page tables for the shared identity map — one 4 KiB table per 4 MiB
 * of physical memory, filled in by init(). These are never freed and
 * never duplicated; every page directory's non-private PDEs point
 * straight at these same physical frames. */
alignas(4096) uint32_t g_identity_pt[NUM_IDENTITY_PDES][1024];
alignas(4096) uint32_t g_io_pt[1024];
alignas(4096) uint32_t g_kernel_pgdir[1024];

/* Kernel-only device MMIO window: a single 4 MiB page table at
 * 0xFE800000 (PDE 1010), above the 32 MiB identity map and clear of
 * the VBE LFB window (PDE 1012). PCI BARs that live in the high part
 * of the 4 GiB space are remapped here by map_device_region() so
 * drivers can dereference them as plain pointers in every address
 * space (the PDE is copied from g_kernel_pgdir by create_address_space). */
constexpr uint32_t DEV_MMIO_BASE = 0xFE800000u;
constexpr int DEV_MMIO_PDE = DEV_MMIO_BASE >> 22; /* 1010 */
alignas(4096) uint32_t g_dev_pt[1024];

uint32_t g_current_pgdir = 0;

constexpr int MAX_ADDRESS_SPACES = 16;
/* Frames pinned per private address space: app region (64 pages) +
 * stack (16) + heap (BRK 1 MiB = 256) = 336, with a little headroom.
 * Each entry is a 4-byte frame number, so the meta table below stays
 * well under a page. */
/* Per-space frame budget. The app-image region is sized from the ELF
 * (up to 1 MiB -> 256 frames), the heap is 1 MiB (256) and the stack 16:
 * bigger guest binaries (a guest C compiler!) need the headroom. */
constexpr int MAX_FRAMES_PER_SPACE = 1024;
constexpr int MAX_PRIVATE_PDES_PER_SPACE = 4;

struct AddrSpaceMeta {
    bool used = false;
    uint32_t pgdir_phys = 0;
    uint32_t data_frames[MAX_FRAMES_PER_SPACE];
    int data_frame_count = 0;
    uint32_t pt_frames[MAX_PRIVATE_PDES_PER_SPACE];
    int pt_frame_count = 0;
};

AddrSpaceMeta g_spaces[MAX_ADDRESS_SPACES];

AddrSpaceMeta* find_meta(uint32_t pgdir_phys)
{
    for (auto& m : g_spaces)
        if (m.used && m.pgdir_phys == pgdir_phys)
            return &m;
    return nullptr;
}

AddrSpaceMeta* alloc_meta()
{
    for (auto& m : g_spaces)
        if (!m.used) {
            m = AddrSpaceMeta{};
            m.used = true;
            return &m;
        }
    return nullptr;
}

} // namespace

namespace vnu::paging {

void init()
{
    for (int pde = 0; pde < NUM_IDENTITY_PDES; ++pde) {
        for (int i = 0; i < 1024; ++i) {
            uint32_t phys = static_cast<uint32_t>(pde) * 0x400000u + static_cast<uint32_t>(i) * PAGE_SIZE;
            g_identity_pt[pde][i] = phys | PTE_PRESENT | PTE_RW;
        }
        g_kernel_pgdir[pde] =
            reinterpret_cast<uint32_t>(&g_identity_pt[pde][0]) | PDE_PRESENT | PDE_RW;
    }
    for (int pde = NUM_IDENTITY_PDES; pde < 1024; ++pde)
        g_kernel_pgdir[pde] = 0;

    /* Identity-map the VBE LFB window (supervisor RW) so present()
     * can blit to VRAM. Shared with every address space via the
     * "copy g_kernel_pgdir" step in create_address_space(). */
    for (int i = 0; i < 1024; ++i)
        g_io_pt[i] = (VBE_LFB_BASE + static_cast<uint32_t>(i) * PAGE_SIZE) |
                     PTE_PRESENT | PTE_RW;
    g_kernel_pgdir[VBE_LFB_PDE] =
        reinterpret_cast<uint32_t>(&g_io_pt[0]) | PDE_PRESENT | PDE_RW;

    uint32_t pgdir_phys = reinterpret_cast<uint32_t>(&g_kernel_pgdir[0]);
    g_current_pgdir = pgdir_phys;

    asm volatile("mov %0, %%cr3" : : "r"(pgdir_phys) : "memory");
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u; /* PG on */
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

uint32_t kernel_pgdir_phys()
{
    return reinterpret_cast<uint32_t>(&g_kernel_pgdir[0]);
}

uint32_t map_device_region(uint32_t phys, uint32_t size)
{
    if (size == 0)
        return 0;
    uint32_t offset = phys & 0x3FFFFFu;
    uint32_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    /* Must fully fit inside the single-table window. */
    if (offset + pages * PAGE_SIZE > 0x400000u)
        return 0;
    uint32_t page = offset / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; ++i)
        g_dev_pt[page + i] = ((phys & ~0xFFFu) + i * PAGE_SIZE) | PTE_PRESENT | PTE_RW;
    g_kernel_pgdir[DEV_MMIO_PDE] =
        reinterpret_cast<uint32_t>(&g_dev_pt[0]) | PDE_PRESENT | PDE_RW;
    /* Reload CR3 so the new PDE is live immediately (boot-time, cheap). */
    uint32_t pgdir = reinterpret_cast<uint32_t>(&g_kernel_pgdir[0]);
    g_current_pgdir = pgdir;
    asm volatile("mov %0, %%cr3" : : "r"(pgdir) : "memory");
    return DEV_MMIO_BASE + offset;
}

uint32_t create_address_space(const MapRange* ranges, int count)
{
    AddrSpaceMeta* meta = alloc_meta();
    if (!meta)
        return 0;

    uint32_t pgdir_phys = vnu::pmm::alloc_frame();
    if (!pgdir_phys) {
        meta->used = false;
        return 0;
    }
    meta->pgdir_phys = pgdir_phys;
    uint32_t* pgdir = reinterpret_cast<uint32_t*>(pgdir_phys);

    /* Start from the shared identity map, then punch in private page
     * tables for whichever PDEs the caller's ranges touch. */
    for (int i = 0; i < 1024; ++i)
        pgdir[i] = g_kernel_pgdir[i];

    for (int r = 0; r < count; ++r) {
        const MapRange& range = ranges[r];
        for (uint32_t p = 0; p < range.num_pages; ++p) {
            uint32_t vaddr = range.vaddr_start + p * PAGE_SIZE;
            uint32_t pde_idx = vaddr >> 22;
            uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

            uint32_t* pt;
            /* Does this PDE already have a private table (from an
             * earlier page in this same range, or an earlier range)? */
            uint32_t pde_val = pgdir[pde_idx];
            bool is_private = false;
            for (int k = 0; k < meta->pt_frame_count; ++k) {
                if ((meta->pt_frames[k] & ~0xFFFu) == (pde_val & ~0xFFFu)) {
                    is_private = true;
                    break;
                }
            }
            if (!is_private) {
                if (meta->pt_frame_count >= MAX_PRIVATE_PDES_PER_SPACE) {
                    destroy_address_space(pgdir_phys);
                    return 0;
                }
                uint32_t pt_phys = vnu::pmm::alloc_frame();
                if (!pt_phys) {
                    destroy_address_space(pgdir_phys);
                    return 0;
                }
                meta->pt_frames[meta->pt_frame_count++] = pt_phys;
                pgdir[pde_idx] = pt_phys | PDE_PRESENT | PDE_RW;
                /* A fresh page table would wipe this whole 4 MiB window,
                 * unmapping the kernel BSS/IDT that shares the same PDE
                 * (e.g. the stack at 0x900000 sits in the same window as
                 * the IDT at 0xA04022). Seed it with the shared identity
                 * entries, then let the private frames below override. */
                uint32_t* seed_pt = reinterpret_cast<uint32_t*>(pt_phys);
                if (pde_idx < NUM_IDENTITY_PDES) {
                    for (int k = 0; k < 1024; ++k)
                        seed_pt[k] = g_identity_pt[pde_idx][k];
                } else {
                    for (int k = 0; k < 1024; ++k)
                        seed_pt[k] = 0;
                }
            }
            pt = reinterpret_cast<uint32_t*>(pgdir[pde_idx] & ~0xFFFu);

            if (meta->data_frame_count >= MAX_FRAMES_PER_SPACE) {
                destroy_address_space(pgdir_phys);
                return 0;
            }
            uint32_t frame = vnu::pmm::alloc_frame();
            if (!frame) {
                destroy_address_space(pgdir_phys);
                return 0;
            }
            meta->data_frames[meta->data_frame_count++] = frame;
            pt[pte_idx] = frame | PTE_PRESENT | PTE_RW;
        }
    }

    return pgdir_phys;
}

/* Adds `num_pages` more mapped pages to an existing private address
 * space, starting at vaddr_start. Used by execve() when the new image
 * (e.g. a large guest-compiled binary) needs a bigger app region than
 * the process was spawned with. Mirrors the mapping loop of
 * create_address_space(): PDEs that are still shared identity entries
 * first get a private table seeded with the identity mapping. */
bool extend_address_space(uint32_t pgdir_phys, uint32_t vaddr_start, uint32_t num_pages)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return false;

    uint32_t* pgdir = reinterpret_cast<uint32_t*>(pgdir_phys);
    for (uint32_t p = 0; p < num_pages; ++p) {
        uint32_t vaddr = vaddr_start + p * PAGE_SIZE;
        uint32_t pde_idx = vaddr >> 22;
        uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

        uint32_t* pt;
        uint32_t pde_val = pgdir[pde_idx];
        bool is_private = false;
        for (int k = 0; k < meta->pt_frame_count; ++k) {
            if ((meta->pt_frames[k] & ~0xFFFu) == (pde_val & ~0xFFFu)) {
                is_private = true;
                break;
            }
        }
        if (!is_private) {
            if (meta->pt_frame_count >= MAX_PRIVATE_PDES_PER_SPACE)
                return false;
            uint32_t pt_phys = vnu::pmm::alloc_frame();
            if (!pt_phys)
                return false;
            meta->pt_frames[meta->pt_frame_count++] = pt_phys;
            pgdir[pde_idx] = pt_phys | PDE_PRESENT | PDE_RW;
            uint32_t* seed_pt = reinterpret_cast<uint32_t*>(pt_phys);
            if (pde_idx < NUM_IDENTITY_PDES) {
                for (int k = 0; k < 1024; ++k)
                    seed_pt[k] = g_identity_pt[pde_idx][k];
            } else {
                for (int k = 0; k < 1024; ++k)
                    seed_pt[k] = 0;
            }
        }
        pt = reinterpret_cast<uint32_t*>(pgdir[pde_idx] & ~0xFFFu);

        uint32_t pte = pt[pte_idx];
        if (pte & PTE_PRESENT)
            continue; /* already mapped by an earlier range in this exec */
        if (meta->data_frame_count >= MAX_FRAMES_PER_SPACE)
            return false;
        uint32_t frame = vnu::pmm::alloc_frame();
        if (!frame)
            return false;
        meta->data_frames[meta->data_frame_count++] = frame;
        pt[pte_idx] = frame | PTE_PRESENT | PTE_RW;
    }
    return true;
}

void destroy_address_space(uint32_t pgdir_phys)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return;
    for (int i = 0; i < meta->data_frame_count; ++i)
        vnu::pmm::free_frame(meta->data_frames[i]);
    for (int i = 0; i < meta->pt_frame_count; ++i)
        vnu::pmm::free_frame(meta->pt_frames[i]);
    vnu::pmm::free_frame(pgdir_phys);
    meta->used = false;
}

void switch_to(uint32_t pgdir_phys)
{
    if (pgdir_phys == g_current_pgdir)
        return;
    g_current_pgdir = pgdir_phys;
    asm volatile("mov %0, %%cr3" : : "r"(pgdir_phys) : "memory");
}

uint32_t current_pgdir()
{
    return g_current_pgdir;
}

uint32_t phys_frame_at(uint32_t pgdir_phys, uint32_t vaddr)
{
    if (!pgdir_phys)
        return 0;
    const uint32_t* pgdir = reinterpret_cast<const uint32_t*>(pgdir_phys);
    uint32_t pde = pgdir[vaddr >> 22];
    if (!(pde & PTE_PRESENT))
        return 0;
    const uint32_t* pt = reinterpret_cast<const uint32_t*>(pde & ~0xFFFu);
    uint32_t pte = pt[(vaddr >> 12) & 0x3FF];
    if (!(pte & PTE_PRESENT))
        return 0;
    return pte & ~0xFFFu;
}

} // namespace vnu::paging
