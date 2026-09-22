#pragma once
#include <stdint.h>

namespace vnu::pci {

struct Address {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
};

// Configuration-space access (conventional 0xCF8/0xCFC mechanism).
uint32_t read_dword(Address a, uint16_t reg);
void write_dword(Address a, uint16_t reg, uint32_t val);
uint16_t read_word(Address a, uint16_t reg);

// Vendor/device IDs (registers 0 and 2).
uint16_t vendor(Address a);
uint16_t device(Address a);

// Scans bus 0..bus_max for the first device with the given vendor and
// device IDs. Returns true and fills `out` on a hit.
bool find(uint16_t vendor_id, uint16_t device_id, Address& out);

// Size (in bytes) of a 32-bit memory BAR, measured by writing all ones.
uint32_t bar_size(Address a, int index);

} // namespace vnu::pci