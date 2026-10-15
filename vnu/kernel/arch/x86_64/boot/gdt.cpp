#include <vnu/arch/boot.h>
#include <cstdint>

namespace {
struct GdtEntry {
    std::uint16_t limit_lo;
    std::uint16_t base_lo;
    std::uint8_t base_mid;
    std::uint8_t access;
    std::uint8_t gran;
    std::uint8_t base_hi;
} __attribute__((packed));

struct GdtPtr {
    std::uint16_t limit;
    std::uint32_t base;
} __attribute__((packed));

GdtEntry gdt[3];
void set_entry(int i, std::uint32_t base, std::uint32_t limit, std::uint8_t access, std::uint8_t gran) {
    gdt[i].base_lo = static_cast<std::uint16_t>(base & 0xFFFF);
    gdt[i].base_mid = static_cast<std::uint8_t>((base >> 16) & 0xFF);
    gdt[i].base_hi = static_cast<std::uint8_t>((base >> 24) & 0xFF);
    gdt[i].limit_lo = static_cast<std::uint16_t>(limit & 0xFFFF);
    gdt[i].gran = static_cast<std::uint8_t>(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access = access;
}
} // namespace

void vnu::arch::disable_paging() {
    std::uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~0x80000000ULL;
    cr0 |= 0x1ULL;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void vnu::arch::gdt_init() {
    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    GdtPtr ptr{};
    ptr.limit = static_cast<std::uint16_t>(sizeof(gdt) - 1);
    ptr.base = reinterpret_cast<std::uint32_t>(&gdt[0]);
    asm volatile("lgdt %0" : : "m"(ptr));
}
