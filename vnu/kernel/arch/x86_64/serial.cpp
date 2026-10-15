#include <vnu/arch/serial.h>
#include <vnu/arch/io.h>

// arch/i386/serial.cpp - the PC's early log: a 16550 UART on COM1
// (0x3F8). QEMU's default -serial routes it to stdio, so the boot log
// is visible without a display. The UART is polled, never interrupt-
// driven, which is why this is safe from any context.

namespace vnu::arch {

namespace {
constexpr uint16_t COM1 = 0x3F8;
}

void debug_init()
{
    io_out8(COM1 + 1, 0x00); // disable interrupts
    io_out8(COM1 + 3, 0x80); // enable DLAB
    io_out8(COM1 + 0, 0x03); // divisor = 3 -> 38400 baud
    io_out8(COM1 + 1, 0x00);
    io_out8(COM1 + 3, 0x03); // 8 bits, no parity, 1 stop bit
    io_out8(COM1 + 2, 0xC7); // enable and clear the FIFO
    io_out8(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

void debug_putc(char c)
{
    while (!(io_in8(COM1 + 5) & 0x20)) {
    } /* wait for the transmit holding register to empty */
    io_out8(COM1, static_cast<uint8_t>(c));
}

} // namespace vnu::arch
