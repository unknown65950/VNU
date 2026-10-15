#include <vnu/arch/mmu.h>
#include <cstdint>

void vnu::arch::mmu::write_cr3(std::uintptr_t phys) {
    asm volatile("mov %0, %%cr3" : : "r"(static_cast<std::uint64_t>(phys)) : "memory");
}
std::uintptr_t vnu::arch::mmu::read_cr3() {
    std::uint64_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return static_cast<std::uintptr_t>(v);
}
