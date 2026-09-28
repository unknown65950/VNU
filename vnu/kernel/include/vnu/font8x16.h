#pragma once
#include <stdint.h>

namespace vnu {

/* The VGA BIOS's own 8x16 console font: 256 glyphs, one bit per pixel,
 * 8 pixels wide and 16 tall, row 0 at the top. It is what vga_gfx draws
 * text with when the card cannot be read for one (see the font capture
 * there), so text looks the same however the display is attached. */
extern const uint8_t font8x16[256][16];

} // namespace vnu
