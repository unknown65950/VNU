#include <vnu/arch/io.h>

// arch/i386/io.cpp - the i386 half of vnu/arch/io.h: the x86 port space.

namespace vnu::arch {

uint8_t io_in8(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint16_t io_in16(uint16_t port)
{
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint32_t io_in32(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void io_out8(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void io_out16(uint16_t port, uint16_t value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void io_out32(uint16_t port, uint32_t value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

void io_wait()
{
    /* Write to an unused port (the POST diagnostic register) - the
     * canonical "slow down" for old ISA devices. */
    asm volatile("outb %0, $0x80" : : "a"(static_cast<uint8_t>(0)));
}

} // namespace vnu::arch
