#include <vnu/paging.h>
#include <vnu/pmm.h>
#include <vnu/arch/mmu.h>
#include <stdint.h>

/* arch/x86_64/paging.cpp - the x86_64 address-space builder.
 *
 * The same model as arch/i386/paging.cpp (a shared identity map plus a
 * private, per-process page table for the user windows), carried up to
 * long mode's four levels: PML4 -> PDPT -> PD -> PT, 4 KiB pages. Every
 * virtual address the core uses is below 4 GiB, so PML4 index 0 holds
 * the whole kernel and user world and PDPT index 3 holds the high
 * device windows (VBE LFB, PCI MMIO), exactly where the i386 port put
 * them in its single page directory.
 *
 * STATUS: bring-up skeleton (see arch/README.md). This file compiles
 * and links and is the intended long-mode shape, but the port does not
 * boot yet, so none of it has run on hardware. */

extern "C" void* memset(void* dst, int val, unsigned long count);
extern "C" void* memcpy(void* dst, const void* src, unsigned long count);

namespace {

constexpr uint64_t P_PRESENT = 1ull << 0;
constexpr uint64_t P_RW = 1ull << 1;
constexpr uint64_t P_PS = 1ull << 7;
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ull;

constexpr uint64_t PAGE_BYTES = 4096;

constexpr uint64_t pml4_idx(uintptr_t v) { return (v >> 39) & 0x1FF; }
constexpr uint64_t pdpt_idx(uintptr_t v) { return (v >> 30) & 0x1FF; }
constexpr uint64_t pd_idx(uintptr_t v) { return (v >> 21) & 0x1FF; }
constexpr uint64_t pt_idx(uintptr_t v) { return (v >> 12) & 0x1FF; }

constexpr uint32_t IDENTITY_MB = 32; /* matches run.sh/build_iso.sh's -m 32 */
constexpr int NUM_IDENTITY_PTS = IDENTITY_MB / 2; /* one PT per 2 MiB window */

/* Shared identity map: PTs covering 0..32 MiB, wired into the kernel PD.
 * Never freed, never duplicated; every private PD point at them. */
alignas(4096) uint64_t g_identity_pt[NUM_IDENTITY_PTS][512];
/* The kernel's first-level tables. g_pd and g_pdpt cover 0..1 GiB;
 * g_pd_hi is the PD for 3..4 GiB, where the device windows live. */
alignas(4096) uint64_t g_pd[512];
alignas(4096) uint64_t g_pd_hi[512];
alignas(4096) uint64_t g_pdpt[512];
alignas(4096) uint64_t g_pml4[512];

/* VBE LFB: a 16 MiB window at 0xFD000000, mapped with 4 KiB pages so
 * present() can blit to VRAM. Index 6 of dispi gives the address on
 * real Bochs hardware; QEMU 11 puts it here (found via `info pci`). */
constexpr uintptr_t VBE_LFB_BASE = 0xFD000000ull;
constexpr int VBE_LFB_PD = static_cast<int>(pd_idx(VBE_LFB_BASE)); /* 488 */
constexpr int VBE_LFB_PTS = 8; /* 16 MiB / 2 MiB */
alignas(4096) uint64_t g_vbe_pt[VBE_LFB_PTS][512];

/* Kernel-only device MMIO window at 0xFE800000 (pd index 500), above
 * the identity map and clear of the VBE window. PCI BARs in the high
 * part of the 4 GiB space are remapped here by map_device_region(). */
constexpr uintptr_t DEV_MMIO_BASE = 0xFE800000ull;
constexpr int DEV_MMIO_PD = static_cast<int>(pd_idx(DEV_MMIO_BASE)); /* 500 */
alignas(4096) uint64_t g_dev_pt[512];

uintptr_t g_current_pgdir = 0;

constexpr int MAX_ADDRESS_SPACES = 16;
constexpr int MAX_FRAMES_PER_SPACE = 2560;
/* Tables a space owns beyond its PML4: the private PDPT and PD, plus one
 * PT per privatized 2 MiB window (the app image, canvas, stacks, an
 * 8 MiB heap and a /dev/fb0 window are well under two dozen). */
constexpr int MAX_PRIVATE_TABLES_PER_SPACE = 32;

struct AddrSpaceMeta {
    bool used = false;
    uintptr_t pgdir_phys = 0; /* the PML4 */
    uintptr_t data_frames[MAX_FRAMES_PER_SPACE];
    int data_frame_count = 0;
    uintptr_t table_frames[MAX_PRIVATE_TABLES_PER_SPACE]; /* PDPT, PD, PTs */
    int table_frame_count = 0;
};

AddrSpaceMeta g_spaces[MAX_ADDRESS_SPACES];

AddrSpaceMeta* find_meta(uintptr_t pgdir_phys)
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

bool table_is_private(const AddrSpaceMeta* meta, uint64_t entry)
{
    for (int k = 0; k < meta->table_frame_count; ++k)
        if ((meta->table_frames[k] & ~0xFFFull) == (entry & ~0xFFFull))
            return true;
    return false;
}

/* Build the shared identity + device tables and turn on the MMU
 * features the kernel needs. Paging itself is already on (long mode). */
void build_kernel_tables()
{
    for (int w = 0; w < NUM_IDENTITY_PTS; ++w) {
        for (int i = 0; i < 512; ++i) {
            uintptr_t phys = static_cast<uintptr_t>(w) * 0x200000ull +
                             static_cast<uintptr_t>(i) * PAGE_BYTES;
            g_identity_pt[w][i] = phys | P_PRESENT | P_RW;
        }
        g_pd[w] = reinterpret_cast<uintptr_t>(&g_identity_pt[w][0]) | P_PRESENT | P_RW;
    }

    /* VBE LFB (supervisor RW, shared with every address space). */
    for (int t = 0; t < VBE_LFB_PTS; ++t) {
        for (int i = 0; i < 512; ++i) {
            uintptr_t phys = VBE_LFB_BASE + static_cast<uintptr_t>(t) * 0x200000ull +
                             static_cast<uintptr_t>(i) * PAGE_BYTES;
            g_vbe_pt[t][i] = phys | P_PRESENT | P_RW;
        }
        g_pd_hi[VBE_LFB_PD + t] =
            reinterpret_cast<uintptr_t>(&g_vbe_pt[t][0]) | P_PRESENT | P_RW;
    }

    g_pdpt[0] = reinterpret_cast<uintptr_t>(&g_pd[0]) | P_PRESENT | P_RW;
    g_pdpt[3] = reinterpret_cast<uintptr_t>(&g_pd_hi[0]) | P_PRESENT | P_RW;
    g_pml4[0] = reinterpret_cast<uintptr_t>(&g_pdpt[0]) | P_PRESENT | P_RW;
}

} // namespace

namespace vnu::paging {

void init()
{
    build_kernel_tables();

    uintptr_t pgdir = reinterpret_cast<uintptr_t>(&g_pml4[0]);
    g_current_pgdir = pgdir;
    vnu::arch::write_cr3(pgdir);

    /* CR0: paging is already on, but clear EM/TS so the FPU borrow in
     * gui/wallpaper.cpp is legal. */
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000ull;
    cr0 &= ~0x0Cull;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    /* CR4: PAE is already set by the boot stub; add OSFXSR/OSXMMEXCPT so
     * fxsave64/fxrstor64 are available. */
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x00000620ull; /* PAE | OSFXSR | OSXMMEXCPT */
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

uintptr_t kernel_pgdir_phys()
{
    return reinterpret_cast<uintptr_t>(&g_pml4[0]);
}

uintptr_t map_device_region(uintptr_t phys, uintptr_t size)
{
    if (size == 0)
        return 0;
    uintptr_t offset = phys & 0x1FFFFFull;
    uintptr_t pages = (size + PAGE_BYTES - 1) / PAGE_BYTES;
    if (offset + pages * PAGE_BYTES > 0x200000ull)
        return 0;
    uintptr_t page = offset / PAGE_BYTES;
    for (uintptr_t i = 0; i < pages; ++i)
        g_dev_pt[page + i] = ((phys & ~0xFFFull) + i * PAGE_BYTES) | P_PRESENT | P_RW;
    g_pd_hi[DEV_MMIO_PD] = reinterpret_cast<uintptr_t>(&g_dev_pt[0]) | P_PRESENT | P_RW;

    g_current_pgdir = reinterpret_cast<uintptr_t>(&g_pml4[0]);
    vnu::arch::write_cr3(g_current_pgdir);
    return DEV_MMIO_BASE + offset;
}

namespace {

/* Make sure `vaddr`'s 2 MiB window in this space has a private PT and
 * return it. The space's PDPT/PD were allocated with the space and are
 * already private, so only the PT may need creating. */
uint64_t* private_pt(AddrSpaceMeta* meta, uintptr_t* pd, uintptr_t vaddr)
{
    const uint64_t idx = pd_idx(vaddr);
    const uint64_t entry = pd[idx];
    if (table_is_private(meta, entry))
        return reinterpret_cast<uint64_t*>(entry & ADDR_MASK);

    if (meta->table_frame_count >= MAX_PRIVATE_TABLES_PER_SPACE)
        return nullptr;
    uintptr_t pt_phys = vnu::pmm::alloc_frame();
    if (!pt_phys)
        return nullptr;
    meta->table_frames[meta->table_frame_count++] = pt_phys;
    pd[idx] = pt_phys | P_PRESENT | P_RW;

    uint64_t* pt = reinterpret_cast<uint64_t*>(pt_phys);
    if (idx < NUM_IDENTITY_PTS) {
        for (int k = 0; k < 512; ++k)
            pt[k] = g_identity_pt[idx][k];
    } else {
        for (int k = 0; k < 512; ++k)
            pt[k] = 0;
    }
    return pt;
}

bool owns_frame(const AddrSpaceMeta* meta, uintptr_t frame)
{
    for (int i = 0; i < meta->data_frame_count; ++i)
        if ((meta->data_frames[i] & ~0xFFFull) == (frame & ~0xFFFull))
            return true;
    return false;
}

} // namespace

uintptr_t create_address_space(const MapRange* ranges, int count)
{
    AddrSpaceMeta* meta = alloc_meta();
    if (!meta)
        return 0;

    uintptr_t pgdir_phys = vnu::pmm::alloc_frame();
    uintptr_t pdpt_phys = vnu::pmm::alloc_frame();
    uintptr_t pd_phys = vnu::pmm::alloc_frame();
    if (!pgdir_phys || !pdpt_phys || !pd_phys) {
        if (pgdir_phys) vnu::pmm::free_frame(pgdir_phys);
        if (pdpt_phys) vnu::pmm::free_frame(pdpt_phys);
        if (pd_phys) vnu::pmm::free_frame(pd_phys);
        meta->used = false;
        return 0;
    }
    meta->pgdir_phys = pgdir_phys;
    meta->table_frames[meta->table_frame_count++] = pdpt_phys;
    meta->table_frames[meta->table_frame_count++] = pd_phys;

    uint64_t* pml4 = reinterpret_cast<uint64_t*>(pgdir_phys);
    uint64_t* pdpt = reinterpret_cast<uint64_t*>(pdpt_phys);
    uint64_t* pd = reinterpret_cast<uint64_t*>(pd_phys);
    for (int i = 0; i < 512; ++i)
        pml4[i] = g_pml4[i];
    for (int i = 0; i < 512; ++i)
        pdpt[i] = g_pdpt[i];
    for (int i = 0; i < 512; ++i)
        pd[i] = g_pd[i];
    pml4[0] = pdpt_phys | P_PRESENT | P_RW;
    pdpt[0] = pd_phys | P_PRESENT | P_RW;

    for (int r = 0; r < count; ++r) {
        const MapRange& range = ranges[r];
        for (uint32_t p = 0; p < range.num_pages; ++p) {
            uintptr_t vaddr = range.vaddr_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
            uint64_t* pt = private_pt(meta, pd, vaddr);
            if (!pt) {
                destroy_address_space(pgdir_phys);
                return 0;
            }
            if (meta->data_frame_count >= MAX_FRAMES_PER_SPACE) {
                destroy_address_space(pgdir_phys);
                return 0;
            }
            uintptr_t frame = vnu::pmm::alloc_frame();
            if (!frame) {
                destroy_address_space(pgdir_phys);
                return 0;
            }
            meta->data_frames[meta->data_frame_count++] = frame;
            pt[pt_idx(vaddr)] = frame | P_PRESENT | P_RW;
        }
    }
    return pgdir_phys;
}

bool extend_address_space(uintptr_t pgdir_phys, uintptr_t vaddr_start, uint32_t num_pages)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return false;
    uint64_t* pd = reinterpret_cast<uint64_t*>(meta->table_frames[1]);

    for (uint32_t p = 0; p < num_pages; ++p) {
        uintptr_t vaddr = vaddr_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
        uint64_t* pt = private_pt(meta, pd, vaddr);
        if (!pt)
            return false;
        uint64_t pte = pt[pt_idx(vaddr)];
        if ((pte & P_PRESENT) && owns_frame(meta, pte & ADDR_MASK))
            continue;
        if (meta->data_frame_count >= MAX_FRAMES_PER_SPACE)
            return false;
        uintptr_t frame = vnu::pmm::alloc_frame();
        if (!frame)
            return false;
        meta->data_frames[meta->data_frame_count++] = frame;
        pt[pt_idx(vaddr)] = frame | P_PRESENT | P_RW;
    }
    return true;
}

bool map_frames(uintptr_t pgdir_phys, uintptr_t vaddr_start, uint32_t num_pages, uintptr_t phys_start)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return false;
    uint64_t* pd = reinterpret_cast<uint64_t*>(meta->table_frames[1]);

    for (uint32_t p = 0; p < num_pages; ++p) {
        uintptr_t vaddr = vaddr_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
        uintptr_t frame = phys_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
        uint64_t* pt = private_pt(meta, pd, vaddr);
        if (!pt)
            return false;
        if (meta->data_frame_count >= MAX_FRAMES_PER_SPACE)
            return false;
        if (!owns_frame(meta, frame))
            meta->data_frames[meta->data_frame_count++] = frame;
        pt[pt_idx(vaddr)] = frame | P_PRESENT | P_RW;
    }
    return true;
}

bool map_borrowed_frames(uintptr_t pgdir_phys, uintptr_t vaddr_start,
                         uint32_t num_pages, uintptr_t phys_start)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return false;
    uint64_t* pd = reinterpret_cast<uint64_t*>(meta->table_frames[1]);

    for (uint32_t p = 0; p < num_pages; ++p) {
        uintptr_t vaddr = vaddr_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
        uint64_t* pt = private_pt(meta, pd, vaddr);
        if (!pt)
            return false;
        pt[pt_idx(vaddr)] = (phys_start + static_cast<uintptr_t>(p) * PAGE_BYTES) | P_PRESENT | P_RW;
    }
    return true;
}

bool unmap_frames(uintptr_t pgdir_phys, uintptr_t vaddr_start, uint32_t num_pages)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return false;
    uint64_t* pd = reinterpret_cast<uint64_t*>(meta->table_frames[1]);

    for (uint32_t p = 0; p < num_pages; ++p) {
        uintptr_t vaddr = vaddr_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
        uint64_t entry = pd[pd_idx(vaddr)];
        if (!table_is_private(meta, entry))
            return false;
        uint64_t* pt = reinterpret_cast<uint64_t*>(entry & ADDR_MASK);
        uint64_t pte = pt[pt_idx(vaddr)];
        if (!(pte & P_PRESENT))
            return false;
        uintptr_t frame = pte & ADDR_MASK;
        pt[pt_idx(vaddr)] = 0;
        for (int i = 0; i < meta->data_frame_count; ++i) {
            if ((meta->data_frames[i] & ~0xFFFull) == frame) {
                meta->data_frames[i] = meta->data_frames[meta->data_frame_count - 1];
                --meta->data_frame_count;
                break;
            }
        }
        vnu::pmm::free_frame(frame);
    }
    return true;
}

bool copy_pages(uintptr_t dst_pgdir, uintptr_t src_pgdir, uintptr_t vaddr_start, uint32_t num_pages)
{
    for (uint32_t p = 0; p < num_pages; ++p) {
        uintptr_t vaddr = vaddr_start + static_cast<uintptr_t>(p) * PAGE_BYTES;
        uintptr_t src = phys_frame_at(src_pgdir, vaddr);
        uintptr_t dst = phys_frame_at(dst_pgdir, vaddr);
        if (!src || !dst)
            return false;
        memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), PAGE_BYTES);
    }
    return true;
}

void destroy_address_space(uintptr_t pgdir_phys)
{
    AddrSpaceMeta* meta = find_meta(pgdir_phys);
    if (!meta)
        return;
    for (int i = 0; i < meta->data_frame_count; ++i)
        vnu::pmm::free_frame(meta->data_frames[i]);
    for (int i = 0; i < meta->table_frame_count; ++i)
        vnu::pmm::free_frame(meta->table_frames[i]);
    vnu::pmm::free_frame(pgdir_phys);
    meta->used = false;
}

void switch_to(uintptr_t pgdir_phys)
{
    if (pgdir_phys == g_current_pgdir)
        return;
    g_current_pgdir = pgdir_phys;
    vnu::arch::write_cr3(pgdir_phys);
}

uintptr_t current_pgdir()
{
    return g_current_pgdir;
}

uintptr_t phys_frame_at(uintptr_t pgdir_phys, uintptr_t vaddr)
{
    if (!pgdir_phys)
        return 0;
    const uint64_t* pml4 = reinterpret_cast<const uint64_t*>(pgdir_phys);
    uint64_t e = pml4[pml4_idx(vaddr)];
    if (!(e & P_PRESENT))
        return 0;
    const uint64_t* pdpt = reinterpret_cast<const uint64_t*>(e & ADDR_MASK);
    e = pdpt[pdpt_idx(vaddr)];
    if (!(e & P_PRESENT))
        return 0;
    if (e & P_PS)
        return (e & ADDR_MASK) + (vaddr & 0x3FFFFFFFull);
    const uint64_t* pd = reinterpret_cast<const uint64_t*>(e & ADDR_MASK);
    e = pd[pd_idx(vaddr)];
    if (!(e & P_PRESENT))
        return 0;
    if (e & P_PS)
        return (e & ADDR_MASK) + (vaddr & 0x1FFFFFull);
    const uint64_t* pt = reinterpret_cast<const uint64_t*>(e & ADDR_MASK);
    e = pt[pt_idx(vaddr)];
    if (!(e & P_PRESENT))
        return 0;
    return e & ADDR_MASK;
}

} // namespace vnu::paging
