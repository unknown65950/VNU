#pragma once

// vnu/arch/serial.h - the kernel's early log.
//
// A single byte stream that works before any driver is up and stays
// usable as a last-resort trace. On a PC it is COM1, so `qemu -serial
// stdio` shows the whole boot even with no display; a port routes it at
// whatever its platform offers, or makes it a no-op. This is
// diagnostics only - the user-visible console is vnu::tty and the
// desktop, never this.

namespace vnu::arch {

void debug_init();
void debug_putc(char c);

inline void debug_write(const char* s)
{
    if (!s)
        return;
    while (*s)
        debug_putc(*s++);
}

} // namespace vnu::arch
