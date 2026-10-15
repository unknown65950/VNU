#include <vnu/arch/boot.h>
#include <cstdint>

extern "C" void vnu_syscall_entry();
extern "C" void vnu_timer_isr();

namespace {
struct IdtGate {
    std::uint16_t off_lo;
    std::uint16_t sel;
    std::uint16_t flags;
    std::uint16_t off_mid;
    std::uint32_t off_hi;
    std::uint32_t zero;
} __attribute__((packed));

IdtGate idt[256];
struct IdtPtr {
    std::uint16_t limit;
    std::uint64_t base;
} __attribute__((packed));

void set_gate(int vec, void (*handler)(), std::uint16_t flags) {
    std::uint64_t a = reinterpret_cast<std::uint64_t>(handler);
    idt[vec].off_lo = a & 0xFFFF;
    idt[vec].off_mid = (a >> 16) & 0xFFFF;
    idt[vec].off_hi = (a >> 32) & 0xFFFFFFFF;
    idt[vec].sel = 0x08;
    idt[vec].zero = 0;
    idt[vec].flags = flags;
}

void outb(std::uint16_t port, std::uint8_t val) {
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}
std::uint8_t inb(std::uint16_t port) {
    std::uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

void install_idt() {
    for (int i = 0; i < 256; ++i) {
        set_gate(i, reinterpret_cast<void(*)()>(0), 0x8E00);
    }
    set_gate(0x20, vnu_timer_isr, 0x8E00);
    set_gate(0x80, vnu_syscall_entry, 0xEE00);
    IdtPtr ptr{};
    ptr.limit = static_cast<std::uint16_t>(sizeof(idt) - 1);
    ptr.base = reinterpret_cast<std::uint64_t>(&idt[0]);
    asm volatile("lidt %0" : : "m"(ptr));
}
} // namespace

void vnu::arch::interrupt_controller_init() {
    std::uint8_t mask1 = inb(0x21);
    std::uint8_t mask2 = inb(0xA1);
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, mask1); outb(0xA1, mask2);
}
void vnu::arch::idt_init() {
    install_idt();
}
