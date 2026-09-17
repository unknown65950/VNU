#pragma once
#include <stdint.h>

// Polling PS/2 mouse driver (8042 auxiliary device, standard 3-byte
// packet stream mode). No IRQ12 handler is installed — the packet
// bytes sit in the controller's output buffer until read, so a plain
// poll loop (as already used for the keyboard in this kernel) is
// enough for a single-threaded GUI event loop.

namespace vnu::mouse {

void init();

// Non-blocking: returns true and fills dx/dy (screen-space, already
// sign-adjusted), buttons (bit0=left, bit1=right, bit2=middle) and the
// wheel delta (positive = up, negative = down, 0 = no scroll) if a full
// packet was assembled since the last call. Returns false (leaving
// outputs untouched) if there was nothing new to report. On devices
// without a wheel, `wheel` is always 0.
bool poll(int& dx, int& dy, uint8_t& buttons, int8_t& wheel);

} // namespace vnu::mouse
