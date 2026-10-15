#include <vnu/arch/mmu.h>

// arch/i386/mmu.cpp - the i386 control-register accessors behind
// vnu/arch/mmu.h. Everything else about the MMU lives in
// arch/i386/paging.cpp.

namespace vnu::arch {

uintptr_t read_cr3()
{
    uintptr_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

void write_cr3(uintptr_t phys)
{
    asm volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

} // namespace vnu::arch
