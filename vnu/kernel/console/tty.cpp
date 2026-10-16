#include <vnu/tty.h>
#include <vnu/ansiterm.h>
#include <vnu/arch/io.h>

namespace {

constexpr uint16_t W = 80, H = 25;
volatile uint16_t* const vga = reinterpret_cast<volatile uint16_t*>(0xB8000);

/* The console is the VGA text plane plus the parser every console shares
 * (see <vnu/ansiterm.h>): the plane is the cell grid, so a character the
 * terminal puts down is a character on the screen. The installer draws
 * its own blue setup screens, so the escape subset has to be here.
 *
 * What is really this console's own is scrolling: a line off the top
 * shifts the plane and blanks the last row (a window's console parks the
 * row in its scrollback instead). */
void vga_scroll(vnu::ansi::Term* t);

/* Statically initialised, not set up in init(): the kernel prints a line
 * or two (a host name from an installed disk) before tty::init() runs,
 * and that output has to reach the plane too. */
vnu::ansi::Term g_term = {
    vga, H, W, 0, 0, 0x07, /* cells, rows, cols, row, col, attr */
    7, 0, false, false,    /* fg, bg, bright, blink */
    false, false,          /* esc, csi */
    {0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, /* params, nparams, cur */
    true, vga_scroll,      /* bs_erase (this console erases), on_scroll */
    nullptr, 0, true       /* user, dirty, cursor_moved */
};

/* The hardware cursor is not the terminal's: it is a CRTC register that
 * only moves when the flush decides the screen is at rest. */
bool cursor_dirty = false;

void hw_cursor_now()
{
    uint16_t pos = static_cast<uint16_t>(g_term.row * W + g_term.col);
    vnu::arch::io_out8(0x3D4, 0x0F);
    vnu::arch::io_out8(0x3D5, static_cast<uint8_t>(pos & 0xFF));
    vnu::arch::io_out8(0x3D4, 0x0E);
    vnu::arch::io_out8(0x3D5, static_cast<uint8_t>((pos >> 8) & 0xFF));
    cursor_dirty = false;
}

/* Fast scroll: copy row blocks as 32-bit words instead of a cell-by-cell
 * nested loop. The cursor is the terminal's to move (see term_scroll_up),
 * so this only shifts the plane and blanks the last row. */
void vga_scroll(vnu::ansi::Term* t)
{
    auto* dst = reinterpret_cast<volatile uint32_t*>(vga);
    auto* src = reinterpret_cast<volatile uint32_t*>(vga + W);
    /* (H-1) rows * W cells * 2 bytes / 4 = (H-1)*W/2 dwords */
    constexpr size_t dwords = (static_cast<size_t>(H - 1) * W) / 2;
    for (size_t i = 0; i < dwords; ++i)
        dst[i] = src[i];
    auto* last = reinterpret_cast<volatile uint32_t*>(vga + (H - 1) * W);
    const uint32_t cell = 0x20u | (static_cast<uint32_t>(t->attr) << 8);
    const uint32_t blank = cell | (cell << 16);
    for (size_t i = 0; i < W / 2; ++i)
        last[i] = blank;
    /* Every row moved, so every row is new as pixels. */
    t->dirty = vnu::ansi::term_all_rows(*t);
}

} // namespace

namespace vnu::tty {

void init()
{
    vnu::ansi::term_init(g_term, vga, H, W, /*bs_erase=*/true, vga_scroll);
    g_term.dirty = vnu::ansi::term_all_rows(g_term);
    cursor_dirty = true;
    flush_cursor();
}

void clear()
{
    vnu::ansi::term_clear(g_term);
    hw_cursor_now();
}

void flush_cursor()
{
    if (cursor_dirty)
        hw_cursor_now();
}

void putc(char c)
{
    vnu::ansi::term_putc(g_term, c);
    if (g_term.cursor_moved) {
        cursor_dirty = true;
        g_term.cursor_moved = false;
    }
}

void write(const char* s, size_t n)
{
    vnu::ansi::term_write(g_term, s, n);
    if (g_term.cursor_moved) {
        cursor_dirty = true;
        g_term.cursor_moved = false;
    }
    flush_cursor(); /* one CRTC update per write(2) batch */
}

void write_cstr(const char* s)
{
    if (!s)
        return;
    while (*s)
        putc(*s++);
    flush_cursor();
}

void set_cursor(uint16_t r, uint16_t c)
{
    vnu::ansi::term_set_cursor(g_term, r, c);
    cursor_dirty = true;
    g_term.cursor_moved = false;
    flush_cursor();
}

void get_cursor(uint16_t* r, uint16_t* c)
{
    if (r)
        *r = static_cast<uint16_t>(g_term.row);
    if (c)
        *c = static_cast<uint16_t>(g_term.col);
}

void get_size(uint16_t* r, uint16_t* c)
{
    if (r)
        *r = H;
    if (c)
        *c = W;
}

/* The text plane is not a separate piece of hardware: on a VGA the
 * characters live in the same VRAM a graphics mode uses as its
 * framebuffer, and every mode VNU can program here is wider than the
 * 128 KiB legacy window 0xA0000-0xBFFFF, so the desktop's first present
 * writes over the console. The screen is therefore kept aside on the
 * way into graphics mode and put back on the way out, which is the
 * whole of what it takes for the shell to be where the user left it
 * when a graphical program ends. */
uint16_t g_saved_screen[W * H];
bool g_screen_saved = false;

uint16_t cell(uint16_t row, uint16_t col)
{
    if (row >= H || col >= W)
        return 0x0720;   /* a blank light-on-black space */
    /* A saved screen is the console's own copy, and while it is saved
     * the live cells cannot be read: 0xB8000 is not the text plane
     * then, it is inside the card's aperture, which a mode with a
     * linear framebuffer has pointed at that framebuffer instead. */
    return g_screen_saved ? g_saved_screen[row * W + col]
                          : g_term.cells[row * W + col];
}

void save_screen()
{
    for (size_t i = 0; i < W * H; ++i)
        g_saved_screen[i] = g_term.cells[i];
    g_screen_saved = true;
}

void restore_screen()
{
    if (!g_screen_saved)
        return;
    for (size_t i = 0; i < W * H; ++i)
        g_term.cells[i] = g_saved_screen[i];
    g_screen_saved = false;   /* the live cells are the console's again */
    cursor_dirty = true;
    g_term.dirty = vnu::ansi::term_all_rows(g_term);
    flush_cursor();
}

/* The two ends of vgfx::console_tick()'s question. The rows are set
 * wherever a cell is written (the terminal's dirty mask), and cleared
 * row by row by whoever has just drawn them as pixels - the only caller
 * of that is present_text(), which clears what it drew and no more, so
 * a row written while a redraw was in flight is redrawn next time
 * instead of being lost with the row that was clean. */
uint32_t console_dirty_rows()
{
    return g_term.dirty;
}

void console_presented(uint32_t rows)
{
    g_term.dirty &= ~rows;
}

} // namespace vnu::tty
