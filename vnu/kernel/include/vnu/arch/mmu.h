#pragma once
#include <stdint.h>

// vnu/arch/mmu.h - the processor address-space registers the core
// touches directly. Building page tables is arch/<arch>/paging.cpp's
// job, behind the vnu::paging API; these two are the operations that
// need a processor register rather than a data structure, and the only
// ones that leak out of that file (gui/wintask.cpp reads the live CR3).

namespace vnu::arch {

uintptr_t read_cr3();
void write_cr3(uintptr_t phys);

} // namespace vnu::arch
