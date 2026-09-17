#pragma once
#include <stdint.h>

// Bitmap physical-frame allocator over a fixed pool. QEMU is launched
// with -m 32 (see run.sh) and the kernel's own fixed layout already
// uses physical memory up to ~0x940000 (see kernel/include/vnu/wintask.h
// and vnu::proc's user-stack base), so the pool sits well clear of
// that, comfortably inside 32 MiB.

namespace vnu::pmm {

constexpr uint32_t FRAME_SIZE = 4096;
constexpr uint32_t POOL_BASE = 0x01400000; // 20 MiB
constexpr uint32_t POOL_END = 0x01E00000;  // 30 MiB (10 MiB pool, 2560 frames)

void init();

// Returns the physical address of a freshly zeroed 4 KiB frame, or 0
// if the pool is exhausted.
uint32_t alloc_frame();

/* Allocates a run of `n` *physically contiguous* zeroed frames and
 * returns the base physical address (identity-mapped, so also a valid
 * virtual address), or 0 if no such run exists. Used by the wintask
 * manager, which needs contiguous storage for its multi-slot task
 * array and per-task root-image copies. */
uint32_t alloc_contig(uint32_t n);

void free_contig(uint32_t phys_addr, uint32_t n);

void free_frame(uint32_t phys_addr);

/* Pool statistics, for /proc/meminfo. */
uint32_t total_frames();
uint32_t free_frames();

} // namespace vnu::pmm
