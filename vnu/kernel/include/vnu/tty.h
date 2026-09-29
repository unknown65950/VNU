#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vnu::tty {

void init();
void clear();
void putc(char c);
void write(const char* s, size_t n);
void write_cstr(const char* s);
void set_cursor(uint16_t row, uint16_t col);
void get_cursor(uint16_t* row, uint16_t* col);
/* Text-plane geometry in character cells (80x25 today). Userspace asks
 * for it through SYS_tty_size instead of hardcoding the numbers, so a
 * full-screen program (a pager) keeps its layout when the mode changes. */
void get_size(uint16_t* rows, uint16_t* cols);
/* Flush pending hardware cursor update (after a batch of putc). */
void flush_cursor();
/* Which text rows have changed since they were last drawn as pixels, as
 * a bitmask of rows (bit n is row n), or 0 if none. A display with no
 * text mode of its own has to be told the console is not what it last
 * showed, and the scheduler asks this on every pass to find out (see
 * vgfx::console_tick); it is a mask and not a single bit because
 * redrawing one line of the console has to cost one line and not the
 * 400 of them. A display with a text mode never asks: the characters
 * are in the plane and the hardware shows them. */
uint32_t console_dirty_rows();
/* The console has been drawn as pixels: clear these rows (a mask in the
 * shape console_dirty_rows() returns) from what is pending. */
void console_presented(uint32_t rows);
/* The character and attribute word of one cell: the character in the
 * low byte, the attribute above it. The console renderer on a display
 * with no text mode of its own draws these as pixels (see
 * vgfx::present_text), and it reads them from here rather than from
 * 0xB8000 itself. */
uint16_t cell(uint16_t row, uint16_t col);

/* Put the text plane aside / put it back, around a graphics mode that
 * has the same VRAM as the console. See tty.cpp for why. */
void save_screen();
void restore_screen();

} // namespace vnu::tty
