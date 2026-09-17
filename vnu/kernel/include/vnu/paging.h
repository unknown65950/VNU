#pragma once
#include <stdint.h>

// Real per-process address spaces. Until now this kernel ran with
// paging permanently disabled (a literal vnu_disable_paging() call in
// kernel_main) and a single flat physical=virtual address space, which
// is why only one non-shell app could ever be resident at
// 0x400000 at a time (see kernel/include/vnu/wintask.h's older
// comment). This module gives every process its own page directory:
// the low ~32 MiB (kernel code/data, the VFS, the framebuffer, the
// physical frame pool, etc.) stays identity-mapped and *shared* across
// every page directory, but the "app" region — code/data at 0x400000
// and whatever stack range a process is told to use — gets a private
// page table backed by freshly allocated physical frames per process,
// so two different processes' view of the same virtual addresses are
// physically different memory.

namespace vnu::paging {

constexpr uint32_t PAGE_SIZE = 4096;

// One virtual sub-range to back with fresh, private physical frames
// when building a new address space (e.g. "8 pages at 0x400000" for
// an app image, or "16 pages at 0x600000" for its stack).
struct MapRange {
    uint32_t vaddr_start;
    uint32_t num_pages;
};

// Builds the shared identity-mapped kernel page directory covering
// 0..32 MiB and enables paging (sets CR0.PG). Call once, early, after
// vnu::pmm::init().
void init();

// Physical address of the shared "no process" page directory — the
// one active before any process has run, and safe to switch back to
// whenever nothing process-specific needs to be resident.
uint32_t kernel_pgdir_phys();

// Allocates a new page directory that shares the kernel's identity
// mappings but has fresh, private physical frames backing each given
// range. Returns the new directory's physical address (suitable for
// loading into CR3), or 0 on allocation failure.
uint32_t create_address_space(const MapRange* ranges, int count);

// Frees every private frame `create_address_space` allocated for
// `pgdir_phys` (the ranges it was built with), plus the directory and
// any private page tables themselves. Never frees shared/identity
// frames. Safe to call with kernel_pgdir_phys() (a no-op).
void destroy_address_space(uint32_t pgdir_phys);

void switch_to(uint32_t pgdir_phys);
uint32_t current_pgdir();

// Physical frame (also a valid identity-mapped virtual address) backing
// `vaddr` in the address space `pgdir_phys`, or 0 if that page isn't
// mapped there. Lets the wintask manager load ELF images and build
// argv stacks straight into a *new* task's private frames without
// having to switch CR3 first.
uint32_t phys_frame_at(uint32_t pgdir_phys, uint32_t vaddr);

} // namespace vnu::paging
