#pragma once
#include <stdint.h>

// Minimal Multiboot2 information parser.
//
// boot.s passes the physical address of the Multiboot2 info structure to
// kernel_main. We care about two tag kinds: MODULE tags (type 3), because
// the disk installer is booted with the kernel file itself loaded as a
// module and can then copy the exact running kernel onto the target disk
// without the kernel embedding a second copy of itself, and because the
// initial ramdisk rides in as another module (see vnu/initrd.h); and the
// MEMORY MAP (type 6), so userspace can be told how much RAM the machine
// really has.

namespace vnu::mboot {

void set_info(uint32_t addr);

// Returns the first loaded module, or nullptr if GRUB passed none. The
// memory it points at is identity-mapped by paging::init().
const void* first_module(uint32_t& size);

// Every loaded module, in the order the boot loader passed them (the
// installer boots the kernel as module 0 and the initrd as module 1).
// module_at() returns nullptr once `index` is past the last one;
// module_count() is how many there are. Both are safe with no info
// structure at all (a hand-made -kernel boot).
int module_count();
const void* module_at(int index, uint32_t& size);

// Total bytes of "available" RAM per the boot loader's memory map,
// saturating at 4 GiB-1. Zero when the loader passed no map.
uint32_t ram_bytes();

} // namespace vnu::mboot
