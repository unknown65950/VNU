#pragma once
#include <stdint.h>

// Bitmap physical-frame allocator over a fixed pool. QEMU is launched
// with -m 32 (see run.sh). The pool must stay clear of the kernel's
// own memory: the kernel image sits at 1-3 MiB and the (identity-mapped)
// BSS -- wallpaper buffers, embedded userspace blobs, the pmm bitmap,
// the identity page tables and the kernel page directory -- extends to
// ~0x14d13f0 (__bss_end). The former POOL_BASE = 0x01400000 (20 MiB)
// overlapped that BSS region: alloc_frame()'s zeroing memset was wiping
// g_bitmap/g_identity_pt/g_kernel_pgdir, the allocator then re-issued
// in-use frames and corrupted the freshly created process page
// directory. So the pool starts at 21 MiB, above __bss_end.

namespace vnu::pmm {

constexpr uint32_t FRAME_SIZE = 4096;
constexpr uint32_t POOL_BASE = 0x01500000; // 21 MiB
constexpr uint32_t POOL_END = 0x01F00000;  // 31 MiB (10 MiB pool, 2560 frames)

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
