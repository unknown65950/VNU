#pragma once
#include <stdint.h>

// vnu/arch/io.h - programmed ("port") I/O.
//
// x86 keeps a separate I/O address space reached with in/out, which is
// what the PC drivers use. An architecture without one implements the
// same calls over a memory-mapped register window; one whose devices
// are all MMIO can leave them empty and let the drivers use pointers
// instead. Port numbers are 16-bit because that is the widest the PC
// port space has - an MMIO-backed implementation narrows to whatever
// window it maps.

namespace vnu::arch {

uint8_t io_in8(uint16_t port);
uint16_t io_in16(uint16_t port);
uint32_t io_in32(uint16_t port);
void io_out8(uint16_t port, uint8_t value);
void io_out16(uint16_t port, uint16_t value);
void io_out32(uint16_t port, uint32_t value);

// A short delay with no side effect, for slow devices that need space
// between consecutive port writes; a no-op where the bus is fast enough.
void io_wait();

} // namespace vnu::arch
