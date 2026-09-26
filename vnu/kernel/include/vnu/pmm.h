#pragma once
#include <stdint.h>

// Bitmap physical-frame allocator over a fixed pool. QEMU is launched
// with -m 32 (see run.sh). The pool must stay clear of the kernel's
// own memory: the kernel image sits at 1-3 MiB and the (identity-mapped)
// BSS -- wallpaper buffers, embedded userspace blobs, the pmm bitmap,
// the identity page tables, the kernel page directory, the TCP socket
// buffers -- extends to ~0xA00000 (__bss_end). VFS file contents are no
// longer a fixed 128*64 KiB BSS array either: each node's bytes live in
// this same pool as a growable contiguous run (see vfs.cpp node_reserve,
// with the /sounds clips and /pics mounted from it).
// The former POOL_BASE = 0x01520000 (21.125 MiB) overlapped that BSS
// region again once the node table grew past 112 entries: alloc_frame()'s
// zeroing memset was wiping g_bitmap/g_identity_pt/g_kernel_pgdir, the
// allocator then re-issued in-use frames and corrupted the freshly
// created process page directory (triple fault on the first context
// switch). So the pool starts past __bss_end; bump it whenever
// __bss_end grows.

namespace vnu::pmm {

constexpr uint32_t FRAME_SIZE = 4096;
constexpr uint32_t POOL_BASE = 0x01700000; // 23 MiB, past __bss_end
constexpr uint32_t POOL_END = 0x01F00000;  // 31 MiB (8 MiB pool, 2048 frames)

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
