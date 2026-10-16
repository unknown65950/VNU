/* vnu/ansiterm.h — the ANSI escape subset VNU's consoles understand.
 *
 * VNU has two consoles and a full-screen program has to draw the same
 * picture on both: the bare VGA text console (console/tty.cpp) and the
 * text console of every GUI window (gui/wintask.cpp). Both are a grid of
 * cells, a cursor and the same small escape subset, so the parser lives
 * here once and each console supplies only what is really its own -
 * where the cells are, and what scrolling a line off the top means.
 *
 * The subset is the whole of what libvlibc's term.h builders emit, which
 * is what the installer, the man pager and vibecommander draw with: SGR
 * (m) colours, cursor positioning (H/f), cursor moves (A/B/C/D/G/d),
 * erase display (J) and erase line (K). Anything else in the stream is
 * consumed and dropped, so random bytes in device output cannot wedge
 * the terminal.
 *
 * A cell is the VGA text format: the character in the low byte and the
 * attribute above it. That is what the VGA plane already holds, and it
 * is what the window renderer turns into a foreground and a background.
 *
 * The grid is a plain (volatile) pointer rather than an array the
 * terminal owns: the VGA console's cells are the plane at 0xB8000, a
 * window's are an array in its task slot. Scrolling is the one operation
 * the terminal cannot do on its own, because it is the one with a
 * different meaning on each side.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vnu::ansi {

struct Term {
    volatile uint16_t* cells; /* rows * cols cells, (attr << 8) | ch */
    int rows;
    int cols;
    int row;                  /* cursor, 0-based */
    int col;
    uint8_t attr;             /* the attribute new cells are written with */

    /* The SGR state `attr` is built from. */
    uint8_t fg;
    uint8_t bg;
    bool bright;
    bool blink;

    /* Parser state: an ESC is pending, or a CSI is being read. */
    bool esc;
    bool csi;
    uint8_t params[8];
    int nparams;
    int cur;

    /* Whether a backspace erases the cell it moves onto. The VGA console
     * has always done so; a window's console must not, because the shell
     * (vash) repaints a line with "\b \b" and would erase what it just
     * drew. */
    bool bs_erase;

    /* Shift the grid up one row and blank the last, parking the old top
     * row if the caller keeps a scrollback. Called by the terminal when a
     * line leaves the top; `user' is the caller's own pointer. */
    void (*on_scroll)(Term*);
    void* user;

    /* For a console that draws its cells as pixels: which rows changed
     * since the caller last looked, and whether the cursor moved (the VGA
     * console's hardware cursor). A window ignores both - it redraws by
     * comparing the whole cell array. */
    uint32_t dirty;
    bool cursor_moved;
};

/* All the rows of a grid that fits the mask below. */
inline uint32_t term_all_rows(const Term& t)
{
    return t.rows >= 32 ? 0xFFFFFFFFu : ((uint32_t{1} << t.rows) - 1);
}

inline void term_mark(Term& t, int row)
{
    if (row >= 0 && row < t.rows && row < 32)
        t.dirty |= uint32_t{1} << row;
}

inline uint16_t term_blank(const Term& t)
{
    return static_cast<uint16_t>(' ') |
           (static_cast<uint16_t>(t.attr) << 8);
}

inline void term_init(Term& t, volatile uint16_t* cells, int rows, int cols,
                      bool bs_erase, void (*on_scroll)(Term*))
{
    t.cells = cells;
    t.rows = rows;
    t.cols = cols;
    t.row = 0;
    t.col = 0;
    t.attr = 0x07; /* VNU default: light grey on black */
    t.fg = 7;
    t.bg = 0;
    t.bright = false;
    t.blink = false;
    t.esc = false;
    t.csi = false;
    t.nparams = 0;
    t.cur = 0;
    t.bs_erase = bs_erase;
    t.on_scroll = on_scroll;
    t.user = nullptr;
    t.dirty = 0;
    t.cursor_moved = true;
}

/* Fill the whole grid with blank cells in the current attribute. */
inline void term_clear(Term& t)
{
    const uint16_t blank = term_blank(t);
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.cols; ++c)
            t.cells[r * t.cols + c] = blank;
    t.row = 0;
    t.col = 0;
    t.dirty = term_all_rows(t);
    t.cursor_moved = true;
}

inline void term_set_cursor(Term& t, int row, int col)
{
    t.row = row < 0 ? 0 : (row >= t.rows ? t.rows - 1 : row);
    t.col = col < 0 ? 0 : (col >= t.cols ? t.cols - 1 : col);
    t.cursor_moved = true;
}

/* ANSI colour numbers (30-37) do not line up with the PC text palette
 * (0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown, 7
 * light grey), so translate: yellow->brown, blue->blue, red->red,
 * cyan->cyan. */
inline uint8_t term_ansi_to_pc(uint8_t an)
{
    static const uint8_t map[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return map[an & 7];
}

inline void term_sgr(Term& t, const uint8_t* p, int n)
{
    for (int i = 0; i < n; ++i) {
        switch (p[i]) {
        case 0:
            t.fg = 7;
            t.bg = 0;
            t.bright = false;
            t.blink = false;
            break;
        case 1:
            t.bright = true;
            break;
        case 5:
            t.blink = true;
            break;
        case 7: {
            /* reverse video swaps foreground and background */
            uint8_t s = t.fg;
            t.fg = t.bg;
            t.bg = s;
            break;
        }
        case 30 ... 37:
            t.bright = false;
            t.fg = term_ansi_to_pc(static_cast<uint8_t>(p[i] - 30));
            break;
        case 39:
            t.fg = 7;
            t.bright = false;
            break;
        case 40 ... 47:
            t.bg = term_ansi_to_pc(static_cast<uint8_t>(p[i] - 40));
            break;
        case 49:
            t.bg = 0;
            break;
        case 90 ... 97:
            t.bright = true;
            t.fg = term_ansi_to_pc(static_cast<uint8_t>(p[i] - 90));
            break;
        default:
            break;
        }
    }
    t.attr = static_cast<uint8_t>((t.fg + (t.bright ? 8 : 0)) |
                                  (t.bg << 4) | (t.blink ? 0x80 : 0));
}

inline void term_erase_line(Term& t, int mode)
{
    const uint16_t blank = term_blank(t);
    int from = 0, to = t.cols - 1;
    if (mode == 0)
        from = t.col;
    else if (mode == 1)
        to = t.col;
    for (int c = from; c <= to; ++c)
        t.cells[t.row * t.cols + c] = blank;
    term_mark(t, t.row);
}

inline void term_erase_display(Term& t, int mode)
{
    const uint16_t blank = term_blank(t);
    if (mode == 2 || mode == 3) {
        for (int r = 0; r < t.rows; ++r)
            for (int c = 0; c < t.cols; ++c)
                t.cells[r * t.cols + c] = blank;
        t.row = 0;
        t.col = 0;
    } else if (mode == 1) {
        for (int r = 0; r < t.row; ++r)
            for (int c = 0; c < t.cols; ++c)
                t.cells[r * t.cols + c] = blank;
        for (int c = 0; c <= t.col && c < t.cols; ++c)
            t.cells[t.row * t.cols + c] = blank;
    } else {
        for (int c = t.col; c < t.cols; ++c)
            t.cells[t.row * t.cols + c] = blank;
        for (int r = t.row + 1; r < t.rows; ++r)
            for (int c = 0; c < t.cols; ++c)
                t.cells[r * t.cols + c] = blank;
    }
    t.dirty = term_all_rows(t);
    t.cursor_moved = true;
}

/* The cursor may not go past the bottom: a new line where there is none
 * scrolls the grid, and the terminal leaves it to the caller (see
 * on_scroll). */
inline void term_scroll_up(Term& t)
{
    if (t.on_scroll)
        t.on_scroll(&t);
    else {
        for (int r = 1; r < t.rows; ++r)
            for (int c = 0; c < t.cols; ++c)
                t.cells[(r - 1) * t.cols + c] = t.cells[r * t.cols + c];
        const uint16_t blank = term_blank(t);
        for (int c = 0; c < t.cols; ++c)
            t.cells[(t.rows - 1) * t.cols + c] = blank;
        t.dirty = term_all_rows(t);
    }
}

inline int term_param(const Term& t, int i, int def)
{
    return i < t.nparams && t.params[i] ? t.params[i] : def;
}

inline void term_csi(Term& t, uint8_t final)
{
    if (t.nparams < 8)
        t.params[t.nparams++] = static_cast<uint8_t>(t.cur);
    t.cur = 0;
    t.csi = false;
    switch (final) {
    case 'm':
        term_sgr(t, t.params, t.nparams);
        break;
    case 'H':
    case 'f':
        /* CUP: default "1;1" homes; explicit row;col (1-based). */
        term_set_cursor(t, term_param(t, 0, 1) - 1, term_param(t, 1, 1) - 1);
        break;
    case 'A':
        term_set_cursor(t, t.row - term_param(t, 0, 1), t.col);
        break;
    case 'B':
        term_set_cursor(t, t.row + term_param(t, 0, 1), t.col);
        break;
    case 'C':
        term_set_cursor(t, t.row, t.col + term_param(t, 0, 1));
        break;
    case 'D':
        term_set_cursor(t, t.row, t.col - term_param(t, 0, 1));
        break;
    case 'G':
        term_set_cursor(t, t.row, term_param(t, 0, 1) - 1);
        break;
    case 'd':
        term_set_cursor(t, term_param(t, 0, 1) - 1, t.col);
        break;
    case 'J':
        term_erase_display(t, t.nparams == 0 ? 2 : term_param(t, 0, 0));
        break;
    case 'K':
        term_erase_line(t, term_param(t, 0, 0));
        break;
    default:
        break;
    }
}

inline void term_putc(Term& t, char ch)
{
    if (t.esc) {
        t.esc = false;
        if (ch == '[') {
            t.csi = true;
            t.nparams = 0;
            t.cur = 0;
        }
        t.cursor_moved = true;
        return;
    }
    if (t.csi) {
        if (ch >= '0' && ch <= '9') {
            t.cur = t.cur * 10 + (ch - '0');
            if (t.cur > 255)
                t.cur = 255;
        } else if (ch == ';') {
            if (t.nparams < 8)
                t.params[t.nparams++] = static_cast<uint8_t>(t.cur);
            t.cur = 0;
        } else if (ch == 0x1B) {
            t.esc = true;
            t.csi = false;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            term_csi(t, static_cast<uint8_t>(ch));
        } else {
            t.csi = false;
        }
        t.cursor_moved = true;
        return;
    }
    if (ch == 0x1B) {
        t.esc = true;
        t.cursor_moved = true;
        return;
    }
    if (ch == '\n') {
        t.col = 0;
        if (++t.row >= t.rows) {
            term_scroll_up(t);
            t.row = t.rows - 1;
        }
        t.cursor_moved = true;
        return;
    }
    if (ch == '\r') {
        t.col = 0;
        t.cursor_moved = true;
        return;
    }
    if (ch == '\b') {
        if (t.col > 0) {
            --t.col;
            if (t.bs_erase) {
                t.cells[t.row * t.cols + t.col] = term_blank(t);
                term_mark(t, t.row);
            }
        } else if (t.row > 0) {
            --t.row;
            t.col = t.cols - 1;
            if (t.bs_erase) {
                t.cells[t.row * t.cols + t.col] = term_blank(t);
                term_mark(t, t.row);
            }
        }
        t.cursor_moved = true;
        return;
    }
    if (ch == '\t') {
        /* Advance to the next multiple of four, on spaces. */
        int n = 4 - (t.col % 4);
        while (n-- > 0)
            term_putc(t, ' ');
        return;
    }
    if (t.col >= t.cols) {
        t.col = 0;
        if (++t.row >= t.rows) {
            term_scroll_up(t);
            t.row = t.rows - 1;
        }
    }
    t.cells[t.row * t.cols + t.col] =
        static_cast<uint16_t>(static_cast<uint8_t>(ch)) |
        (static_cast<uint16_t>(t.attr) << 8);
    term_mark(t, t.row);
    ++t.col;
    t.cursor_moved = true;
}

inline void term_write(Term& t, const char* s, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        term_putc(t, s[i]);
}

} // namespace vnu::ansi
