#include <vnu/arch/boot.h>
#include <stdint.h>

/* arch/x86_64/boot/gdt.cpp - the long-mode GDT, the 64-bit counterpart
 * of arch/i386/boot/gdt.cpp. Five entries:
 *
 *   0x08  64-bit code, L=1 D=0: the kernel itself
 *   0x10  data
 *   0x18  32-bit code, L=0 D=1: the compat code segment the 32-bit
 *         userspace runs in (every port's userspace is 32-bit; here it
 *         executes in long mode's compatibility mode)
 *   0x20  32-bit data
 *
 * Base and limit are ignored by the processor for the 64-bit segments,
 * but the compat segments are decoded as the protected-mode ones they
 * are, so they carry a real 4 GiB limit - and every segment stays at
 * CPL0: VNU has no ring-3 boundary on either port, so a user segment
 * would only be a distinction without a difference. */

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

GdtEntry gdt[5];

void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i].limit_lo = static_cast<uint16_t>(limit & 0xFFFF);
    gdt[i].base_lo = static_cast<uint16_t>(base & 0xFFFF);
    gdt[i].base_mid = static_cast<uint8_t>((base >> 16) & 0xFF);
    gdt[i].base_hi = static_cast<uint8_t>((base >> 24) & 0xFF);
    gdt[i].gran = static_cast<uint8_t>(((limit >> 16) & 0x0F) | (gran & 0xF0));
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
    set_entry(0, 0, 0, 0x00, 0x00); /* null */
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0); /* code64 0x08: L=1, G=1 */
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC0); /* data 0x10 */
    set_entry(3, 0, 0xFFFFF, 0x9A, 0xCF); /* code32 0x18: L=0, D=1 */
    set_entry(4, 0, 0xFFFFF, 0x92, 0xCF); /* data32 0x20 */

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
