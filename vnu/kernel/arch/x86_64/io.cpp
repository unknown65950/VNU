#include <vnu/arch/io.h>

void vnu::arch::io_out8(std::uint16_t port, std::uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
std::uint8_t vnu::arch::io_in8(std::uint16_t port) {
    std::uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void vnu::arch::io_out16(std::uint16_t port, std::uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
std::uint16_t vnu::arch::io_in16(std::uint16_t port) {
    std::uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void vnu::arch::io_out32(std::uint16_t port, std::uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
std::uint32_t vnu::arch::io_in32(std::uint16_t port) {
    std::uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void vnu::arch::io_wait() {
    asm volatile("outb %%al, $0x80" : : "a"(0));
}
