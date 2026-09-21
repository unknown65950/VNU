#pragma once
#include <stdint.h>

/* Desktop wallpaper support (kernel-side).
 *
 * On desktop startup the GUI calls load(), which reads the /wallpaper
 * VFS node (a BMP/PNG/JPEG, decoded with the freestanding px.h) and
 * renders it stretched across the whole 1024x768 desktop, quantizing
 * each pixel down to the 16-color Catppuccin DAC palette the GUI
 * displays. draw_wallpaper() then blits that ready-made frame instead
 * of the procedural scene whenever one was loaded. */

namespace vnu::wallpaper {

/* Decode /wallpaper and build the full-desktop palette frame. Returns
 * true (and flips ready()) only when a format was probed, decoded and
 * quantized end to end. Safe to call more than once: a failed or
 * missing file just leaves the previous frame (or the procedural
 * fallback) in place. */
bool load();

/* Ready-made 1024x768 palette-index frame of the wallpaper. Valid only
 * when ready(). */
const uint8_t* frame();

bool ready();

} // namespace vnu::wallpaper