#pragma once
#include <stdint.h>

// The system clipboard: one buffer of text that any program can put
// something into and any other program can read back. It is what
// backing /dev/clipboard with a device rather than a syscall buys:
// `clipboard copy' in a shell, Ctrl+C in a window and `cat
// /dev/clipboard` in a script all go through the same open/read/write
// the VFS already serves, so every program - console or windowed, C or
// shell - shares one clipboard without an API of its own.
//
// The store is one fixed buffer: writing replaces it, reading walks
// from the start to whatever is in it. It lives in kernel RAM and does
// not survive a reboot, which is what a clipboard is.

namespace vnu::clip {

/* Largest clipboard, in bytes. Large enough for a screenful of text or
 * a file, small enough to be one static buffer in the kernel image (the
 * PMM pool starts just above .bss, so a bigger one would cost memory
 * that is never used). */
constexpr uint32_t CAP = 64u * 1024u;

/* Drop the current contents: the next append starts from empty. This is
 * what opening the device for writing does, so a writer always replaces
 * what was there rather than piling onto someone else's text. */
void clear();

/* Bytes currently held. */
uint32_t size();

/* Copy up to `count` bytes starting at `pos` into `buf`, returning how
 * many were copied (0 once `pos` has reached the end). `pos` is the
 * caller's read position, which is what makes the device read like a
 * file: cat(1) loops until this returns 0. */
uint32_t read_at(uint32_t pos, void* buf, uint32_t count);

/* Append `count` bytes from `data`, clamped to the space left. Returns
 * how many were stored. */
uint32_t append(const void* data, uint32_t count);

} // namespace vnu::clip
