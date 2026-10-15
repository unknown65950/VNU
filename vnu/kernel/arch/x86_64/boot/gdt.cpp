#include <vnu/arch/boot.h>
#include <stdint.h>

/* arch/x86_64/boot/gdt.cpp - the long-mode GDT, the 64-bit counterpart
 * of arch/i386/boot/gdt.cpp. A 64-bit code segment sets L=1 and leaves
 * D=0; base/limit are ignored by the processor in long mode, so the
 * only thing the descriptor has to say is its access rights. */

namespace {

struct GdtEntry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t base_mid;
    uint8_t access;
    uint8_t gran;
    uint8_t base_hi;
} __attribute__((packed));

struct GdtPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

GdtEntry gdt[3];

void set_entry(int i, uint8_t access, uint8_t gran)
{
    gdt[i].limit_lo = 0;
    gdt[i].base_lo = 0;
    gdt[i].base_mid = 0;
    gdt[i].base_hi = 0;
    gdt[i].gran = gran;
    gdt[i].access = access;
}

} // namespace

void vnu::arch::disable_paging()
{
    /* Long mode has no "paging off": CR0.PG is already set by the
     * boot stub's transition and clearing it would fault. The port keeps
     * its identity map from arch/x86_64/boot/boot.s live from the first
     * instruction; vnu::paging::init() then refines it. */
}

void vnu::arch::gdt_init()
{
    set_entry(0, 0x00, 0x00); /* null */
    set_entry(1, 0x9A, 0xA0); /* code 0x08: L=1, G=1 */
    set_entry(2, 0x92, 0xC0); /* data 0x10 */

    GdtPtr ptr{};
    ptr.limit = static_cast<uint16_t>(sizeof(gdt) - 1);
    ptr.base = reinterpret_cast<uint64_t>(&gdt[0]);

    asm volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "m"(ptr)
        : "ax", "rax", "memory");
}
