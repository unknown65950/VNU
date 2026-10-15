#include <vnu/arch/serial.h>
#include <vnu/arch/io.h>

namespace {
constexpr std::uint16_t COM1 = 0x3F8;
bool ready() {
    return (vnu::arch::io_in8(COM1 + 5) & 0x20) != 0;
}
}

void vnu::arch::debug_init() {
    io_out8(COM1 + 1, 0x00);
    io_out8(COM1 + 3, 0x80);
    io_out8(COM1 + 0, 0x01);
    io_out8(COM1 + 1, 0x00);
    io_out8(COM1 + 3, 0x03);
    io_out8(COM1 + 2, 0xC7);
    io_out8(COM1 + 4, 0x0B);
}
void vnu::arch::debug_putc(char c) {
    for (int i = 0; i < 100000; ++i) {
        if (ready()) break;
    }
    io_out8(COM1, static_cast<std::uint8_t>(c));
}
void vnu::arch::debug_write(const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) debug_putc(s[i]);
}
