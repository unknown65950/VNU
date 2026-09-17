#pragma once
#include <stdint.h>

// Minimal Multiboot2 information parser.
//
// boot.s passes the physical address of the Multiboot2 info structure to
// kernel_main. We only care about MODULE tags (type 8): the disk
// installer is booted with the kernel file itself loaded as a module, so
// it can copy the exact running kernel onto the target disk without the
// kernel having to embed a second copy of itself.

namespace vnu::mboot {

void set_info(uint32_t addr);

// Returns the first loaded module, or nullptr if GRUB passed none. The
// memory it points at is identity-mapped by paging::init().
const void* first_module(uint32_t& size);

} // namespace vnu::mboot
