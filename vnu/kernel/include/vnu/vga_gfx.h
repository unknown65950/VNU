#pragma once
#include <stdint.h>

// Minimal VGA graphics driver.
//
// The desktop runs in a linear-framebuffer 8-bpp mode set up through
// the Bochs VBE "dispi" port interface (the same interface QEMU's
// standard VGA emulates), so no BIOS calls are needed once GRUB has
// handed off control in protected mode. The 8x16 text font is not
// hand-authored here; instead it is captured straight out of VGA
// plane 2 while still in text mode (the BIOS/hardware font used to
// render the console you see before switching modes), so any text
// drawn in graphics mode looks like the console font.

namespace vnu::vgfx {

// A bigger desktop than the classic 320x200 mode-13h. The VBE linear
// framebuffer (identity-mapped at 0xFD000000) is blitted to each frame.
constexpr int WIDTH = 1024;
constexpr int HEIGHT = 768;

// VGA default 16-color palette indices (same as text-mode attributes).
constexpr uint8_t COLOR_BLACK = 0;
constexpr uint8_t COLOR_BLUE = 1;
constexpr uint8_t COLOR_GREEN = 2;
constexpr uint8_t COLOR_CYAN = 3;
constexpr uint8_t COLOR_RED = 4;
constexpr uint8_t COLOR_MAGENTA = 5;
constexpr uint8_t COLOR_BROWN = 6;
constexpr uint8_t COLOR_LGRAY = 7;
constexpr uint8_t COLOR_DGRAY = 8;
constexpr uint8_t COLOR_LBLUE = 9;
constexpr uint8_t COLOR_LGREEN = 10;
constexpr uint8_t COLOR_LCYAN = 11;
constexpr uint8_t COLOR_LRED = 12;
constexpr uint8_t COLOR_LMAGENTA = 13;
constexpr uint8_t COLOR_YELLOW = 14;
constexpr uint8_t COLOR_WHITE = 15;

// Switch from whatever text mode GRUB set up into WIDTHxHEIGHTx256.
// Captures the current font + full register state first so exit_to_text()
// can restore the exact mode the console was in.
void enter_gfx_mode();

// Restore the text mode saved by enter_gfx_mode().
void exit_to_text();

void clear(uint8_t color);
void clear_gradient(uint8_t top, uint8_t bottom);
void put_pixel(int x, int y, uint8_t color);
void fill_rect(int x, int y, int w, int h, uint8_t color);
void rect(int x, int y, int w, int h, uint8_t color); // outline only
void hline(int x, int y, int w, uint8_t color);
void vline(int x, int y, int h, uint8_t color);

// Vector primitives (used by the desktop's analog clock).
void line(int x0, int y0, int x1, int y1, uint8_t color);
void circle(int cx, int cy, int r, uint8_t color);        // outline only
void fill_circle(int cx, int cy, int r, uint8_t color);

// Nearest-neighbour upscale of an 8-bpp surface into the backbuffer
// (gfx apps draw a native 480x340 canvas shown 1:1; scale > 1 only for
// large-display resizing).
void blit_scaled(const uint8_t* src, int sw, int sh, int dx, int dy, int scale);

// Procedural desktop wallpaper (sky + sun + clouds + hills), drawn each
// frame in place of the plain gradient.
void draw_wallpaper();

// 8x16 glyphs, captured from the VGA hardware font.
void draw_char(int x, int y, char c, uint8_t fg);
void draw_string(int x, int y, const char* s, uint8_t fg);
int text_width(const char* s);  // in pixels (8 per glyph)

void draw_char8(int x, int y, char c, uint8_t fg);
void draw_string8(int x, int y, const char* s, uint8_t fg);
int text_width8(const char* s);  // in pixels (8 per glyph, 8-row height)

// Simple filled arrow mouse cursor.
void draw_cursor(int x, int y, uint8_t color = COLOR_BLACK);

// Flip the backbuffer to the VBE linear framebuffer (0xFD000000).
void present();

} // namespace vnu::vgfx
