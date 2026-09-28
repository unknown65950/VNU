#pragma once
#include <stdint.h>

// Bitmap physical-frame allocator over everything between the kernel's
// own globals and the last usable byte of RAM. QEMU is launched with -m
// 32 (see run.sh), and the 32 MiB identity map covers the whole span.
//
// The pool must stay clear of the kernel's image (1-3 MiB) and of the
// (identity-mapped) .bss, which linker.ld places at 0xA00000: the pmm
// bitmap, the identity page tables, the kernel page directory, the TCP
// socket buffers, the ELF launcher buffer and the VFS node table all
// live there. VFS file contents are not a fixed 128*64 KiB BSS array
// either: each node's bytes live in this same pool as a growable
// contiguous run (see vfs.cpp node_reserve, with the /etc/vnu sound
// clips and picture pack mounted from it).
//
// That separation used to be a hand-written POOL_BASE that had to be
// bumped by hand every time .bss grew, and getting it wrong does not
// fail cleanly: alloc_frame() zeroes every frame it hands out, so a
// base that overlapped the .bss wiped g_bitmap/g_identity_pt/
// g_kernel_pgdir, the allocator then re-issued in-use frames and
// corrupted the freshly created process page directory (triple fault on
// the first context switch). It has bitten twice - once at
// POOL_BASE = 0x01520000 once the node table grew past 112 entries,
// once at 0x01700000 when the top mode's scanout no longer fitted in
// the 8 MiB that pool was. init() now reads the base off the linker's
// __bss_end, so the two cannot drift apart and a shrinking .bss hands
// its memory to the pool instead of stranding it: with the desktop's
// largest-mode buffers gone (see vga_gfx.h), __bss_end fell by 4.7 MiB
// and the pool grew to 10.9..30.75 MiB, 20.5 MiB of it, up from 14.4.

namespace vnu::pmm {

constexpr uint32_t FRAME_SIZE = 4096;

/* The top of the pool: QEMU's usable RAM ends at 0x1F7F000, so this
 * leaves the last 60 KiB alone. The bottom is not a constant - it is
 * __bss_end, read by init() (see the note above and pmm.cpp). */
constexpr uint32_t POOL_END = 0x01F70000; // 30.75 MiB, inside RAM

/* Reads __bss_end and works out the pool's size from it. Call once,
 * before the first allocation. */
void init();

// Returns the physical address of a freshly zeroed 4 KiB frame, or 0
// if the pool is exhausted.
uint32_t alloc_frame();

/* Allocates a run of `n` *physically contiguous* zeroed frames and
 * returns the base physical address (identity-mapped, so also a valid
 * virtual address), or 0 if no such run exists. Used by the wintask
 * manager, which needs contiguous storage for its multi-slot task
 * array and per-task root-image copies, by the desktop for its
 * framebuffer, and by execve to stage a binary read out of the VFS. */
uint32_t alloc_contig(uint32_t n);

void free_contig(uint32_t phys_addr, uint32_t n);

void free_frame(uint32_t phys_addr);

/* Pool statistics, for /proc/meminfo. */
uint32_t total_frames();
uint32_t free_frames();

} // namespace vnu::pmm
