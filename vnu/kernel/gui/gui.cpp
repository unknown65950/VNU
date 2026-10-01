#include <vnu/gui.h>
#include <vnu/vga_gfx.h>
#include <vnu/ps2mouse.h>
#include <vnu/kbd.h>
#include <vnu/apps.h>
#include <vnu/wintask.h>
#include <vnu/vfs.h>
#include <vnu/tty.h>
#include <vnu/wallpaper.h>
#include <vnu/gfxconf.h>
#include <vnu/abi.h>

/* The freestanding kernel has no <string.h>: the two routines this file
 * needs are declared here, as vga_gfx.cpp declares its memcpy. */
extern "C" void* memcpy(void* dst, const void* src, unsigned long count);
extern "C" int memcmp(const void* a, const void* b, unsigned long count);

namespace {

using vnu::wintask::MAX_TASKS;
using vnu::wintask::TaskHandle;
using vnu::wintask::NO_TASK;
using vnu::wintask::CON_COLS;
using vnu::wintask::CON_ROWS;
using vnu::wintask::SCROLL_ROWS;
using vnu::wintask::CELL_W;
using vnu::wintask::CELL_H;

constexpr int TITLE_H = 20;
constexpr int PANEL_H = 34;
constexpr int BTN_H = 26;
constexpr int BTN_PAD_Y = (PANEL_H - BTN_H) / 2;

constexpr int ICON_SIZE = 24;
constexpr int ICON_CELL_W = 80;
constexpr int ICON_CELL_H = 48;
constexpr int ICON_ORIGIN_X = 8;
/* Icons live below the taskbar now. */
constexpr int ICON_ORIGIN_Y = PANEL_H + 8;

/* Gfx windows stretch their fixed 480x340 canvas to whatever client
 * area the window ends up with, so resizing is free-form just like the
 * text windows. These lower bounds keep a window far enough above a
 * meaningless sliver (~a third of native was about the point where the
 * app chrome stops being usable). */
constexpr int GFX_MIN_CLIENT_W = 160;
constexpr int GFX_MIN_CLIENT_H = 112;

/* Resize grip band width, in pixels, along the window's right/bottom
 * and along the left edge. The top band is thinner so the title bar
 * keeps most of its drag-to-move area. */
constexpr int RESIZE_W = 12;
constexpr int RESIZE_TOP = 6;

struct Window {
    int x, y, w, h;
};

/* A rectangle on the screen. Half-open, the way everything else here
 * measures, so a region of nothing is w == 0 rather than a special
 * case. */
struct Rect {
    int x, y, w, h;
};

bool same_rect(const Window& a, const Window& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* The smallest box holding both, which is what a moved window leaves
 * behind: where it was and where it is now. Repainting the wallpaper
 * over both is what puts the ground back under it. */
Rect both_rects(const Window& a, const Window& b)
{
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = (a.x + a.w) > (b.x + b.w) ? a.x + a.w : b.x + b.w;
    const int y1 = (a.y + a.h) > (b.y + b.h) ? a.y + a.h : b.y + b.h;
    return {x0, y0, x1 - x0, y1 - y0};
}

/* Half-open on both sides, so touching edges are not an overlap. Two
 * shapes, because a window and a region are asked about each other from
 * both directions. */
bool overlaps(const Rect& a, const Rect& b)
{
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w &&
           b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool overlaps(const Rect& r, const Window& w)
{
    return overlaps(r, Rect{w.x, w.y, w.w, w.h});
}

/* The part of `a` that lies inside `b`, empty when there is none.
 * Half-open both ways, like the rest of the rect work here. */
Rect clip_rect(const Rect& a, const Rect& b)
{
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    const int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return {0, 0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
}

/* The combined area of up to MAX_TASKS rectangles, by slabs on the x
 * axis: between each pair of neighbouring x edges, the y spans the
 * rectangles own in that column are merged and measured. The covers of
 * a window are never more than this many, so nothing here has to be
 * clever. */
long long union_area(const Rect* rs, int n)
{
    int xs[2 * MAX_TASKS];
    int nx = 0;
    for (int i = 0; i < n; ++i) {
        xs[nx++] = rs[i].x;
        xs[nx++] = rs[i].x + rs[i].w;
    }
    for (int i = 1; i < nx; ++i) {
        const int key = xs[i];
        int j = i;
        while (j > 0 && xs[j - 1] > key) {
            xs[j] = xs[j - 1];
            --j;
        }
        xs[j] = key;
    }
    int m = 0;
    for (int i = 0; i < nx; ++i)
        if (i == 0 || xs[i] != xs[m - 1])
            xs[m++] = xs[i];
    long long area = 0;
    for (int s = 0; s + 1 < m; ++s) {
        struct Yi {
            int y0, y1;
        } iv[2 * MAX_TASKS];
        int in = 0;
        for (int i = 0; i < n; ++i) {
            if (rs[i].x <= xs[s] && xs[s + 1] <= rs[i].x + rs[i].w) {
                iv[in].y0 = rs[i].y;
                iv[in].y1 = rs[i].y + rs[i].h;
                ++in;
            }
        }
        for (int i = 1; i < in; ++i) {
            const Yi key = iv[i];
            int j = i;
            while (j > 0 && iv[j - 1].y0 > key.y0) {
                iv[j] = iv[j - 1];
                --j;
            }
            iv[j] = key;
        }
        int lo = 0, hi = 0;
        long long span = 0;
        bool first = true;
        for (int i = 0; i < in; ++i) {
            if (first) {
                lo = iv[i].y0;
                hi = iv[i].y1;
                first = false;
            } else if (iv[i].y0 > hi) {
                span += hi - lo;
                lo = iv[i].y0;
                hi = iv[i].y1;
            } else if (iv[i].y1 > hi) {
                hi = iv[i].y1;
            }
        }
        if (!first)
            span += hi - lo;
        area += static_cast<long long>(xs[s + 1] - xs[s]) * span;
    }
    return area;
}

/* Whether the window at z-position `i` has nothing showing: the union
 * of the rectangles of the windows above it (later in the z-order)
 * covers every pixel of it. A cover counts only when it is known
 * opaque: a text window is (it fills its client area), a gfx window's
 * canvas is only below 32bpp, where no pixel has an alpha byte to make
 * itself see-through. At 32bpp the app may have written transparent
 * pixels (the depth is there to advertise that), and "a translucent
 * cover hides this corner of the window under it" is exactly the
 * picture this must never change.
 *
 * A fully covered window is neither drawn nor damaged, and that is the
 * whole of it: the transfer its redraw would make is the pixels that
 * lie on top of it, so compositing it is work no one sees, and damage
 * that says the screen changed when what moved is hidden. */
bool fully_covered(int i, const Window& win, const Window* geom,
                   const TaskHandle* order, int order_len,
                   const bool* minimized)
{
    const long long total = static_cast<long long>(win.w) * win.h;
    if (total == 0)
        return false;
    Rect covers[MAX_TASKS];
    int n = 0;
    for (int j = i + 1; j < order_len; ++j) {
        const TaskHandle h = order[j];
        if (minimized[h])
            continue;
        vnu::wintask::Console* con = vnu::wintask::console(h);
        if (!con || (con->gfx && vnu::vgfx::bpp() != 8))
            continue;
        const Rect c = clip_rect(Rect{win.x, win.y, win.w, win.h},
                                 Rect{geom[h].x, geom[h].y,
                                      geom[h].w, geom[h].h});
        if (c.w > 0 && c.h > 0)
            covers[n++] = c;
    }
    return n > 0 && union_area(covers, n) >= total;
}

/* The box the desktop's icons live in: the tiles and the labels under
 * them, which is what has to be redrawn if anything uncovers any of
 * it. */
Rect icon_area(int count)
{
    const int cols = 4;
    const int rows = (count + cols - 1) / cols;
    return {ICON_ORIGIN_X,
            ICON_ORIGIN_Y,
            cols * ICON_CELL_W,
            rows * ICON_CELL_H};
}

/* The pointer's box, and the frame as it was last put together. The
 * pointer is drawn over everything and is not part of any window, so
 * this is the compositor's own bookkeeping: where it was, which window
 * had the focus, and what each window looked like. Without it a pass
 * has no way to tell a redraw from a no-op, and the honest answer
 * (repaint everything) is the one that made 32bpp expensive. */
constexpr int CURSOR_BOX = 16;

struct Shown {
    Window rect;
    bool up;   /* the window was on the screen, not minimized */
    /* A text window's pixels are made of its cell grid and the four
     * numbers that say which row is where; a gfx window's are the
     * canvas the app owns, which is compared by hash instead (see
     * canvas_hash). */
    int hist_tail, hist_n, scroll_off, cur_row, cur_col;
    /* The canvas as it was when this window was last composited, and
     * how many times a window has been drawn. See canvas_hash. */
    uint64_t canvas;
    int canvas_w, canvas_h;
    char title[vnu::wintask::TITLE_CAP];
    char cells[CON_ROWS][CON_COLS];
};

/* A digest of a gfx window's canvas: FNV-1a over the pixels the app
 * owns, as they are in the display's own depth.
 *
 * A pixel window's contents are the app's, written straight into the
 * pages the compositor reads (see wintask.h), so the compositor has no
 * cell grid to compare and no way to be told "I drew". Repainting such
 * a window on every pass is what it used to do, and it is what made the
 * desktop feel slow: one 480x340 canvas is ~160 000 pixels, and paying
 * for all of them on every pass - a blit, a damage box and a transfer
 * to the host - for a window that had not changed a byte is work whose
 * only result is the frame that is already on the screen.
 *
 * So the canvas is read instead, once per pass: a 480x340 canvas is
 * 160 KiB of indices or 640 KiB of words, which is a linear scan over
 * memory the next blit is going to read anyway, and it settles the same
 * question the blit would have answered. Two things follow from it:
 * an app that is idle costs a scan rather than a repaint, and an app
 * that animates (the clock) is still repainted, because its canvas
 * really is different every time.
 *
 * A digest rather than a copy of the pixels, because a window-sized
 * shadow buffer per window is a megabyte of the pool that a six-window
 * desktop would rather spend on canvases; and a digest rather than a
 * generation counter from the app, because that would be a new syscall
 * for something the pixels already answer. Two different canvases that
 * collide would cost one redundant repaint, never a stale frame. */
uint64_t canvas_hash(const vnu::wintask::Console& con)
{
    if (!con.pixel || con.pix_w <= 0 || con.pix_h <= 0)
        return 0;
    const uint32_t bytes = static_cast<uint32_t>(con.pix_w) *
                           static_cast<uint32_t>(con.pix_h) *
                           (static_cast<uint32_t>(vnu::vgfx::bpp()) / 8u);
    const uint8_t* p = con.pixel;
    uint64_t h = 1469598103934665603ull;   /* FNV-1a 64 offset basis */
    /* A word at a time, not a byte: the digest is read on every pass of
     * every pixel window, and a byte loop over 160 KiB of canvas costs
     * more than the blit it is there to avoid. The canvas is a
     * page-aligned run of frames, so the words are aligned too, and one
     * load and one multiply per word is the whole scan - a digest of
     * every byte, mixed eight bytes to a step.
     *
     * Read through the pointer rather than a memcpy() per word: this
     * runs 20 000 times per pass, and a call is more than the load it
     * would be making. */
    const uint64_t* words = reinterpret_cast<const uint64_t*>(p);
    const uint32_t whole = bytes / 8u;
    for (uint32_t i = 0; i < whole; ++i)
        h = (h ^ words[i]) * 1099511628211ull;
    for (uint32_t w = whole * 8u; w < bytes; ++w) {
        h ^= p[w];
        h *= 1099511628211ull;
    }
    return h;
}

/* True when this window's canvas holds something the last composite of
 * it did not have: different pixels, or a canvas that was reallocated
 * since (which is what a resize does, and which a digest of the same
 * size would otherwise be free to miss - the app redrew into fresh
 * pages that happen to hash the same only if it drew the same picture,
 * so the size is compared to be sure).
 *
 * `digest` is this pass's reading of the canvas, taken once per window
 * in the pass (see the caller): the same canvas is asked about twice -
 * once for whether the window needs drawing at all, once when it is
 * drawn - and a digest is a scan of every pixel of it. */
bool canvas_changed(const Shown& s, const vnu::wintask::Console& con,
                    uint64_t digest)
{
    if (s.canvas_w != con.pix_w || s.canvas_h != con.pix_h)
        return true;
    return digest != s.canvas;
}

/* True when the frame on the screen already holds what this window
 * would draw: the same rectangle, the same title (the panel and the
 * title bar show it) and, for a text window, the same characters in the
 * same places. The cursor is part of the last two: it is a block in the
 * cell grid and moves with cur_col, so a console whose cursor blinked
 * somewhere else is a different frame. `digest` is this pass's reading
 * of a gfx window's canvas (see canvas_changed). */
bool same_window(const Shown& s, const Window& w, const vnu::wintask::Console& con,
                 uint64_t digest)
{
    if (!same_rect(s.rect, w))
        return false;
    for (int i = 0; i < static_cast<int>(vnu::wintask::TITLE_CAP); ++i)
        if (s.title[i] != con.title[i])
            return false;
    if (con.gfx)
        return !canvas_changed(s, con, digest);
    if (s.hist_tail != con.hist_tail || s.hist_n != con.hist_n ||
        s.scroll_off != con.scroll_off || s.cur_row != con.cur_row ||
        s.cur_col != con.cur_col)
        return false;
    return memcmp(s.cells, con.cell, sizeof(s.cells)) == 0;
}

enum class DragOp {
    None,
    Move,
    ResizeCorner, /* bottom-right: resizes both axes */
    ResizeTop,
    ResizeLeft,
    ResizeRight,
    ResizeBottom,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
};

void icon_rect(int index, int& x, int& y)
{
    int col = index % 4;
    int row = index / 4;
    x = ICON_ORIGIN_X + col * ICON_CELL_W;
    y = ICON_ORIGIN_Y + row * ICON_CELL_H;
}

/* 16x16 pictograms drawn inside each 24x24 app tile (kernel-side, so
 * every /apps entry gets an artful icon without shipping bitmap data).
 * All shapes are built from the vgfx primitives; white details sit on
 * the accent tile behind. Coordinates are relative to the tile origin. */

void glyph_smile(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    fill_circle(ox + 8, oy + 8, 6, COLOR_BLACK);        /* face */
    put_pixel(ox + 5, oy + 6, COLOR_WHITE);             /* eyes */
    put_pixel(ox + 11, oy + 6, COLOR_WHITE);
    put_pixel(ox + 4, oy + 9, COLOR_WHITE);             /* smile */
    put_pixel(ox + 5, oy + 10, COLOR_WHITE);
    put_pixel(ox + 6, oy + 11, COLOR_WHITE);
    put_pixel(ox + 7, oy + 11, COLOR_WHITE);
    put_pixel(ox + 8, oy + 11, COLOR_WHITE);
    put_pixel(ox + 9, oy + 11, COLOR_WHITE);
    put_pixel(ox + 10, oy + 11, COLOR_WHITE);
    put_pixel(ox + 11, oy + 10, COLOR_WHITE);
    put_pixel(ox + 12, oy + 9, COLOR_WHITE);
}

void glyph_doc(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    rect(ox + 1, oy + 1, 13, 13, COLOR_BLACK);          /* page */
    fill_rect(ox + 2, oy + 2, 11, 11, COLOR_WHITE);     /* paper */
    hline(ox + 3, oy + 4, 6, COLOR_BLACK);              /* text lines */
    hline(ox + 3, oy + 7, 8, COLOR_BLACK);
    hline(ox + 3, oy + 10, 6, COLOR_BLACK);
}

void glyph_term(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    fill_rect(ox, oy + 1, 16, 14, COLOR_BLACK);         /* window */
    draw_char8(ox + 1, oy + 7, '>', COLOR_WHITE);       /* prompt */
    fill_rect(ox + 9, oy + 9, 4, 3, COLOR_LGREEN);      /* cursor block */
}

void glyph_keypad(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    fill_rect(ox + 1, oy + 1, 14, 5, COLOR_WHITE);      /* display */
    rect(ox + 1, oy + 1, 14, 5, COLOR_BLACK);
    for (int r = 0; r < 3; ++r)                         /* 3x3 keys */
        for (int c = 0; c < 3; ++c)
            fill_rect(ox + 1 + c * 5, oy + 7 + r * 3, 3, 3, COLOR_BLACK);
}

void glyph_folder(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    fill_rect(ox + 1, oy + 2, 4, 2, COLOR_BLACK);       /* tab */
    fill_rect(ox + 1, oy + 4, 14, 9, COLOR_BLACK);      /* body */
}

void glyph_picture(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    rect(ox + 1, oy + 1, 14, 14, COLOR_BLACK);          /* frame */
    fill_rect(ox + 2, oy + 2, 12, 12, COLOR_WHITE);     /* scene */
    fill_circle(ox + 10, oy + 4, 2, COLOR_BLACK);       /* sun */
    for (int yy = 6; yy <= 12; ++yy) {                  /* left mountain */
        int hw = yy - 6;
        hline(ox + 7 - hw, oy + yy, 1 + 2 * hw, COLOR_BLACK);
    }
    for (int yy = 9; yy <= 12; ++yy) {                  /* right peak */
        int hw = yy - 9;
        hline(ox + 11 - hw, oy + yy, 1 + 2 * hw, COLOR_BLACK);
    }
}

void glyph_sliders(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    vline(ox + 4, oy + 2, 12, COLOR_BLACK);
    vline(ox + 11, oy + 2, 12, COLOR_BLACK);
    fill_rect(ox + 2, oy + 8, 5, 2, COLOR_BLACK);
    fill_rect(ox + 9, oy + 5, 5, 2, COLOR_BLACK);
}

void glyph_clock(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    fill_circle(ox + 8, oy + 8, 6, COLOR_BLACK);        /* face */
    hline(ox + 5, oy + 8, 4, COLOR_WHITE);              /* minute */
    vline(ox + 8, oy + 4, 5, COLOR_WHITE);              /* hour */
}

void glyph_music(int x, int y)
{
    using namespace vnu::vgfx;
    const int ox = x + 4, oy = y + 4;
    fill_rect(ox + 3, oy + 9, 8, 4, COLOR_BLACK);       /* note head */
    put_pixel(ox + 4, oy + 8, COLOR_BLACK);
    vline(ox + 9, oy + 2, 7, COLOR_BLACK);              /* stem */
    hline(ox + 9, oy + 2, 5, COLOR_BLACK);              /* flag */
    hline(ox + 10, oy + 3, 4, COLOR_BLACK);
    put_pixel(ox + 13, oy + 4, COLOR_BLACK);
}

/* 24x24 app tile with the accent fill, crisp border and pictogram —
 * shared by the desktop icons and the taskbar's running-app buttons. */
void draw_app_tile(int x, int y, const vnu::apps::AppEntry& app)
{
    using namespace vnu::vgfx;
    fill_rect(x, y, ICON_SIZE, ICON_SIZE, app.icon_color);
    hline(x, y, ICON_SIZE, COLOR_BLACK);
    vline(x, y, ICON_SIZE, COLOR_BLACK);
    hline(x, y + ICON_SIZE - 1, ICON_SIZE, COLOR_BLACK);
    vline(x + ICON_SIZE - 1, y, ICON_SIZE, COLOR_BLACK);
    hline(x + 1, y + 1, ICON_SIZE - 2, COLOR_WHITE);
    vline(x + 1, y + 1, ICON_SIZE - 2, COLOR_WHITE);
    /* 16x16 pictogram, or the first letter for unknown glyphs. */
    switch (app.icon_glyph) {
    case vnu::apps::IconGlyph::ICON_SMILE:   glyph_smile(x, y); break;
    case vnu::apps::IconGlyph::ICON_DOC:     glyph_doc(x, y); break;
    case vnu::apps::IconGlyph::ICON_TERM:    glyph_term(x, y); break;
    case vnu::apps::IconGlyph::ICON_KEYPAD:  glyph_keypad(x, y); break;
    case vnu::apps::IconGlyph::ICON_FOLDER:  glyph_folder(x, y); break;
    case vnu::apps::IconGlyph::ICON_PICTURE: glyph_picture(x, y); break;
    case vnu::apps::IconGlyph::ICON_SLIDERS: glyph_sliders(x, y); break;
    case vnu::apps::IconGlyph::ICON_CLOCK:   glyph_clock(x, y); break;
    case vnu::apps::IconGlyph::ICON_MUSIC:   glyph_music(x, y); break;
    default: {
        char letter[2] = {app.name[0], 0};
        draw_char(x + (ICON_SIZE - 8) / 2, y + (ICON_SIZE - 16) / 2,
                  letter[0], COLOR_BLACK);
        break;
    }
    }
}

void draw_icons(const vnu::apps::AppEntry* apps, int count)
{
    using namespace vnu::vgfx;
    for (int i = 0; i < count; ++i) {
        int x, y;
        icon_rect(i, x, y);
        draw_app_tile(x, y, apps[i]);
        int label_w = text_width(apps[i].name);
        int label_x = x + (ICON_SIZE - label_w) / 2;
        if (label_x < 0)
            label_x = 0;
        draw_string(label_x, y + ICON_SIZE + 3, apps[i].name, COLOR_WHITE);
    }
}

int hit_test_icon(const vnu::apps::AppEntry* apps, int count, int mx, int my)
{
    (void)apps;
    for (int i = 0; i < count; ++i) {
        int x, y;
        icon_rect(i, x, y);
        if (mx >= x && mx < x + ICON_SIZE && my >= y && my < y + ICON_SIZE)
            return i;
    }
    return -1;
}

void clamp_to_desktop(Window& win)
{
    if (win.x < 0) win.x = 0;
    if (win.y < 0) win.y = 0;
    if (win.x + win.w > vnu::vgfx::width()) win.x = vnu::vgfx::width() - win.w;
    if (win.y + win.h > vnu::vgfx::height()) win.y = vnu::vgfx::height() - win.h;
}

void draw_chrome(const Window& win, bool active, bool fill_client = true)
{
    using namespace vnu::vgfx;
    if (fill_client)
        fill_rect(win.x, win.y, win.w, win.h, COLOR_LGRAY);
    rect(win.x, win.y, win.w, win.h, COLOR_BLACK);
    /* Flat chrome: light edge at top/left, dark bottom/right. */
    hline(win.x + 1, win.y + 1, win.w - 2, COLOR_WHITE);
    vline(win.x + 1, win.y + 2, win.h - 3, COLOR_WHITE);
    hline(win.x + 2, win.y + win.h - 2, win.w - 3, COLOR_DGRAY);
    vline(win.x + win.w - 2, win.y + 2, win.h - 3, COLOR_DGRAY);
    /* Active window gets a lavender title bar, inactive ones a dark one. */
    fill_rect(win.x + 1, win.y + 1, win.w - 2, TITLE_H - 1,
              active ? COLOR_LBLUE : COLOR_DGRAY);
    hline(win.x + 1, win.y + TITLE_H, win.w - 2, COLOR_BLACK);
}

/* Visible rows/columns that fit the window's client area. */
int fit_cols(const Window& win)
{
    int cols = (win.w - 4) / CELL_W;
    if (cols < 1)
        cols = 1;
    if (cols > CON_COLS)
        cols = CON_COLS;
    return cols;
}

int fit_rows(const Window& win)
{
    int rows = (win.h - (TITLE_H + 4)) / CELL_H;
    if (rows < 1)
        rows = 1;
    if (rows > CON_ROWS)
        rows = CON_ROWS;
    return rows;
}

/* Map a "total output row" (0 = oldest history row, hist_n + CON_ROWS -
 * 1 = live bottom) to a source row buffer. Returns nullptr for history
 * rows that fall outside the ring (shouldn't happen given the clamps). */
const char* output_row(const vnu::wintask::Console& con, int li, int* screen_row)
{
    if (li < con.hist_n) {
        int idx = con.hist_tail - con.hist_n + li;
        while (idx < 0)
            idx += SCROLL_ROWS;
        idx %= SCROLL_ROWS;
        return con.hist[idx];
    }
    *screen_row = li - con.hist_n;
    return con.cell[*screen_row];
}

void draw_console_window(const Window& win, const vnu::wintask::Console& con, bool active)
{
    draw_chrome(win, active);
    using namespace vnu::vgfx;
    draw_string(win.x + 4, win.y + 2, con.title, active ? COLOR_BLACK : COLOR_WHITE);

    int cols = fit_cols(win);
    int rows = fit_rows(win);
    int total = con.hist_n + CON_ROWS;
    int scroll = con.scroll_off;
    if (scroll < 0)
        scroll = 0;
    if (scroll > con.hist_n)
        scroll = con.hist_n;
    /* First displayed total-output row. At scroll == 0 the live bottom
     * (hist_n + rows - 1 ... ) — i.e. the bottom of the cell screen plus
     * (rows - 1) pre-scroll rows — must line up with the window's bottom
     * row, so we anchor on the last screen line. */
    int first_li = total - rows - scroll;
    if (first_li < 0)
        first_li = 0;

    int cx = win.x + 2;
    int cy = win.y + TITLE_H + 2;
    /* Terminal-style client area: white text on black. */
    fill_rect(cx, cy, cols * CELL_W, rows * CELL_H, COLOR_BLACK);
    (void)rows;
    for (int v = 0; v < rows; ++v) {
        int li = first_li + v;
        int scr = 0;
        const char* row = output_row(con, li, &scr);
        for (int c = 0; c < cols; ++c) {
            char ch = row[c];
            if (ch != ' ' && ch != 0)
                draw_char(cx + c * CELL_W, cy + v * CELL_H, ch, COLOR_WHITE);
        }
    }

    /* Cursor (live bottom only). */
    if (con.scroll_off == 0 || con.scroll_off > con.hist_n) {
        int live_row = con.hist_n + con.cur_row;
        if (live_row >= first_li && live_row < first_li + rows && con.cur_col < cols) {
            int v = live_row - first_li;
            int ccx = cx + con.cur_col * CELL_W;
            int ccy = cy + v * CELL_H + CELL_H - 3;
            hline(ccx, ccy, CELL_W - 1, COLOR_LGRAY);
        }
    }

    /* Scrollbar: only drawn when there is scrollback to view. */
    if (con.hist_n > 0 && rows < CON_ROWS) {
        int bx = win.x + win.w - 4 - 10;
        int top = win.y + TITLE_H + 2;
        int bh = win.h - (TITLE_H + 4);
        fill_rect(bx, top, 10, bh, COLOR_DGRAY);
        int maxoff = con.hist_n;
        int thumb_h = bh * rows / CON_ROWS;
        if (thumb_h < 10)
            thumb_h = 10;
        if (thumb_h > bh)
            thumb_h = bh;
        int thumb_y = top + ((bh - thumb_h) * scroll) / maxoff;
        fill_rect(bx, thumb_y, 10, thumb_h, COLOR_LGRAY);
        vline(bx, top, bh, COLOR_BLACK);
    }
}

void draw_gfx_window(const Window& win, const vnu::wintask::Console& con, bool active)
{
    /* The client area is left alone: the canvas covers it pixel for
     * pixel, so filling it first would be paint the blit below writes
     * over - half a window's worth per open, per raise and per repaint.
     * What shows through instead is the desktop, which is what a canvas
     * with transparent pixels is for. */
    draw_chrome(win, active, /*fill_client=*/false);
    using namespace vnu::vgfx;
    draw_string(win.x + 4, win.y + 2, con.title, active ? COLOR_BLACK : COLOR_WHITE);
    int cw = win.w - 8;
    int ch = win.h - (TITLE_H + 12);
    if (cw < 1)
        cw = 1;
    if (ch < 1)
        ch = 1;
    int origin_x = win.x + 2;
    int origin_y = win.y + TITLE_H + 2;
    /* The canvas is in the display's depth, so the blit is told which:
     * at 32bpp the source is the frame's own format and its top byte is
     * coverage, which is how a window with a transparent background
     * lets the desktop through instead of a black rectangle.
     *
     * Drawn at the canvas's own size, not the window's: the canvas
     * follows the window, so a drag reallocates it and the two agree
     * again by the time this runs. The stretch only remains as the
     * clamp for the one case where they cannot - a window too small to
     * hold its canvas - so a client area is never left showing pixels
     * from beyond the canvas. */
    const int src_w = con.pix_w > 0 ? con.pix_w : vnu::wintask::GFX_W;
    const int src_h = con.pix_h > 0 ? con.pix_h : vnu::wintask::GFX_H;
    blit_scale(con.pixel, src_w, src_h, origin_x, origin_y, cw, ch,
               static_cast<int>(bpp()));
    rect(origin_x - 1, origin_y - 1, cw + 2, ch + 2, COLOR_BLACK);
}

/* Client-area size for a task's window: adapts to the console's mode
 * (text grid vs gfx framebuffer). Gfx windows start at the native 1:1
 * canvas size; text windows at the full 80x24 grid. */
Window size_app_window(int x, int y, const vnu::wintask::Console* con)
{
    Window w;
    w.x = x;
    w.y = y;
    bool gfx = con && con->gfx;
    if (gfx) {
        /* The canvas, not the constants: an app that asked for another
         * size gets a window that size, and one that did not gets the
         * 480x340 its canvas started at. */
        const int cw = (con && con->pix_w > 0) ? con->pix_w : vnu::wintask::GFX_W;
        const int ch = (con && con->pix_h > 0) ? con->pix_h : vnu::wintask::GFX_H;
        w.w = cw + 8;
        w.h = TITLE_H + ch + 12;
    } else {
        w.w = CON_COLS * CELL_W + 8;
        w.h = TITLE_H + CON_ROWS * CELL_H + 12;
    }
    return w;
}

bool hit_test_titlebar(const Window& win, int mx, int my)
{
    return mx >= win.x && mx < win.x + win.w && my >= win.y && my < win.y + TITLE_H;
}

/* Moves a gfx window's chrome to fit a canvas the app resized itself.
 *
 * follow_canvas() is the other direction: there the user moved the edge
 * and the canvas had to follow. Here the app called gfx_canvas(2) and
 * the window has to follow, or the canvas is composited into a client
 * area of a different size and the app's layout is stretched - which is
 * exactly what this milestone exists to stop. Nothing is asked for: the
 * canvas already has the size, so this only computes the chrome around
 * it and re-clamps.
 *
 * Returns true when the window moved, so the caller knows the geometry
 * changed underneath it and the redraw has to cover the whole window. */
bool adopt_canvas(vnu::wintask::Console* con, Window& win)
{
    if (!con->gfx || con->pix_w <= 0 || con->pix_h <= 0)
        return false;
    const int want_w = con->pix_w + 8;
    const int want_h = TITLE_H + con->pix_h + 12;
    if (want_w == win.w && want_h == win.h)
        return false;
    win.w = want_w;
    win.h = want_h;
    clamp_to_desktop(win);
    return true;
}

/* Makes a gfx window's canvas the size the user just dragged it to, so
 * the window is not scaling a smaller canvas up (which is what a fixed
 * 480x340 canvas was doing at every mode, 1024x768 included). The client
 * area is the window minus its chrome; asking for it is what raises
 * SIGWINCH, and the app redraws at the new size.
 *
 * Returns false when the canvas could not be backed at the size asked
 * for, in which case the window is pulled back to the size the canvas
 * still has - the window follows the canvas and never the other way
 * round, so a resize that failed cannot leave a stretched window. */
bool follow_canvas(vnu::wintask::Console* con, Window& win)
{
    const int want_w = win.w - 8;
    const int want_h = win.h - (TITLE_H + 12);
    if (want_w < VNU_GFX_CANVAS_MIN_W || want_h < VNU_GFX_CANVAS_MIN_H)
        return false;
    if (want_w == con->pix_w && want_h == con->pix_h)
        return true;

    vnu_gfx_canvas req{};
    req.want_w = static_cast<uint32_t>(want_w);
    req.want_h = static_cast<uint32_t>(want_h);
    vnu_gfx_canvas got{};
    /* Asked on the GUI's own stack rather than through the syscall
     * entry, because no task is executing to make the call about - but
     * it is the same canvas_resize() the app drives itself, so the
     * clamping and the SIGWINCH are the same either way. */
    if (vnu::wintask::canvas_resize(*con, vnu::wintask::pgdir_of(*con), &req, &got) < 0) {
        /* Out of memory, most likely: leave the window where it was
         * and let the next redraw stretch the canvas it has. */
        return false;
    }
    win.w = static_cast<int>(got.width) + 8;
    win.h = TITLE_H + static_cast<int>(got.height) + 12;
    clamp_to_desktop(win);
    return true;
}

DragOp hit_test_resize(const Window& win, int mx, int my)
{
    if (mx < win.x || mx >= win.x + win.w || my < win.y || my >= win.y + win.h)
        return DragOp::None;
    const int R = RESIZE_W;
    const int TR = RESIZE_TOP;
    bool onL = mx - win.x < R;
    bool onR = win.x + win.w - mx <= R;
    bool onT = my - win.y < TR;
    bool onB = win.y + win.h - my <= R;
    if (onL && onT) return DragOp::ResizeTopLeft;
    if (onR && onT) return DragOp::ResizeTopRight;
    if (onL && onB) return DragOp::ResizeBottomLeft;
    if (onR && onB) return DragOp::ResizeCorner;
    if (onL && my >= win.y + TITLE_H) return DragOp::ResizeLeft;
    if (onR && my >= win.y + TITLE_H) return DragOp::ResizeRight;
    if (onB) return DragOp::ResizeBottom;
    if (onT) return DragOp::ResizeTop;
    return DragOp::None;
}

/* Which mouse pointer the window manager should show for a drag/hover
 * drawn from the hit-test result. */
vnu::vgfx::CursorShape mouse_shape_for(DragOp op)
{
    switch (op) {
    case DragOp::Move:
        return vnu::vgfx::CursorShape::Move;
    case DragOp::ResizeLeft:
    case DragOp::ResizeRight:
        return vnu::vgfx::CursorShape::SizeH;
    case DragOp::ResizeTop:
    case DragOp::ResizeBottom:
        return vnu::vgfx::CursorShape::SizeV;
    case DragOp::ResizeTopLeft:
    case DragOp::ResizeCorner:
        return vnu::vgfx::CursorShape::SizeDiagL;
    case DragOp::ResizeTopRight:
    case DragOp::ResizeBottomLeft:
        return vnu::vgfx::CursorShape::SizeDiagR;
    default:
        return vnu::vgfx::CursorShape::Arrow;
    }
}

bool hit_test_scrollbar(const Window& win, int mx, int my)
{
    int bx = win.x + win.w - 4 - 10;
    int top = win.y + TITLE_H + 2;
    int bh = win.h - (TITLE_H + 4);
    return mx >= bx && mx < bx + 10 && my >= top && my < top + bh;
}

/* --- Taskbar --- */

enum class PB : uint8_t { None = 0, Close, Min, Reboot, Exit, App };

struct PBtn {
    int x, y, w, h;
    PB kind;
    int handle;
};

/* Matches a window title to /apps/ launcher entry (windows spawned
 * from the desktop carry the app name as their title). Returns the
 * app index, or -1 if the window isn't one of the desktop apps. */
int find_app_by_title(const vnu::apps::AppEntry* apps, int count, const char* title)
{
    if (!apps || !title || !*title)
        return -1;
    for (int i = 0; i < count; ++i) {
        int k = 0;
        while (apps[i].name[k] && title[k] && apps[i].name[k] == title[k])
            ++k;
        if (apps[i].name[k] == 0 && title[k] == 0)
            return i;
    }
    return -1;
}

void draw_panel(const PBtn* out, int n, const vnu::apps::AppEntry* apps, int app_count);
int layout_panel(PBtn* out, int focused, const vnu::apps::AppEntry* apps, int app_count);

/* Set when a resolution change happened while the desktop was up, so the
 * event loop can re-place what it owns (window geometry, the cursor)
 * against the new screen. Cleared by the loop, which is the only thing
 * that can act on it. */
bool g_relayout = false;

int layout_panel(PBtn* out, int focused, const vnu::apps::AppEntry* apps, int app_count)
{
    using namespace vnu::vgfx;
    int n = 0;
    int y = BTN_PAD_Y;

    /* --- Left cluster --- */
    int x = 6;

    const char* exit_label = "Exit";
    int w2 = text_width(exit_label) + 10;
    out[n++] = {x, y, w2, BTN_H, PB::Exit, -1};
    x += w2 + 4;

    const char* reb_label = "Reboot";
    w2 = text_width(reb_label) + 10;
    out[n++] = {x, y, w2, BTN_H, PB::Reboot, -1};
    x += w2 + 4;

    /* Running-app list: one button per open window ("active windows").
     * Known /apps/ entries get a square icon tile; anything else falls
     * back to a text button so off-launcher windows stay reachable. */
    for (int h = 0; h < MAX_TASKS && n < 24; ++h) {
        if (!vnu::wintask::has_window(h))
            continue;
        const char* title = vnu::wintask::console(h) ? vnu::wintask::console(h)->title : "";
        if (find_app_by_title(apps, app_count, title) >= 0) {
            out[n++] = {x, y, ICON_SIZE + 2, BTN_H, PB::App, h};
            x += ICON_SIZE + 2 + 3;
        } else {
            w2 = text_width(title) + 12;
            out[n++] = {x, y, w2, BTN_H, PB::App, h};
            x += w2 + 3;
        }
    }

    /* --- Right cluster --- */
    x = width() - 6;
    if (focused >= 0) {
        w2 = 22;
        x -= w2;
        out[n++] = {x, y, w2, BTN_H, PB::Close, focused};
        x -= 4;
        x -= w2;
        out[n++] = {x, y, w2, BTN_H, PB::Min, focused};
        x -= 4;

        const char* title =
            vnu::wintask::console(focused) ? vnu::wintask::console(focused)->title : "";
        w2 = text_width(title) + 8;
        x -= w2;
        out[n++] = {x, y, w2, BTN_H, PB::None, focused}; /* active window name */
    }
    return n;
}

int hit_panel(const PBtn* out, int n, int mx, int my)
{
    if (my >= PANEL_H)
        return -1;
    for (int i = 0; i < n; ++i) {
        const PBtn& b = out[i];
        if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h)
            return i;
    }
    return -1;
}

void draw_panel(const PBtn* out, int n, const vnu::apps::AppEntry* apps, int app_count)
{
    using namespace vnu::vgfx;
    fill_rect(0, 0, width(), PANEL_H, COLOR_LGRAY);
    hline(0, 0, width(), COLOR_WHITE);
    hline(0, PANEL_H - 1, width(), COLOR_BLACK);

    for (int i = 0; i < n; ++i) {
        const PBtn& b = out[i];
        switch (b.kind) {
        case PB::Min:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            hline(b.x + 5, b.y + b.h / 2, b.w - 10, COLOR_WHITE);
            break;
        case PB::Close:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            line(b.x + 4, b.y + 4, b.x + b.w - 5, b.y + b.h - 5, COLOR_WHITE);
            line(b.x + b.w - 5, b.y + 4, b.x + 4, b.y + b.h - 5, COLOR_WHITE);
            break;
        case PB::Reboot:
        case PB::Exit:
            fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
            hline(b.x, b.y, b.w, COLOR_WHITE);
            vline(b.x, b.y, b.h, COLOR_WHITE);
            hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
            vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
            draw_string(b.x + 5, b.y + 5, b.kind == PB::Reboot ? "Reboot" : "Exit",
                        COLOR_WHITE);
            break;
        case PB::App: {
            int ai = find_app_by_title(apps, app_count,
                                       vnu::wintask::console(b.handle)
                                           ? vnu::wintask::console(b.handle)->title
                                           : "");
            if (ai >= 0) {
                /* Running-app icon: the same tile the desktop shows. */
                draw_app_tile(b.x + 1, b.y + 1, apps[ai]);
            } else {
                fill_rect(b.x, b.y, b.w, b.h, COLOR_DGRAY);
                hline(b.x, b.y, b.w, COLOR_WHITE);
                vline(b.x, b.y, b.h, COLOR_WHITE);
                hline(b.x, b.y + b.h - 1, b.w, COLOR_BLACK);
                vline(b.x + b.w - 1, b.y, b.h, COLOR_BLACK);
                draw_string(b.x + 5, b.y + 5,
                            vnu::wintask::console(b.handle)
                                ? vnu::wintask::console(b.handle)->title
                                : "",
                            COLOR_WHITE);
            }
            break;
        }
        case PB::None:
            draw_string(b.x + 4, b.y + 5,
                        vnu::wintask::console(b.handle) ? vnu::wintask::console(b.handle)->title : "",
                        COLOR_BLACK);
            break;
        default:
            break;
        }
    }
}

void reboot_now()
{
    asm volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    for (;;) {
    }
}

bool name_is(const char* a, const char* b)
{
    int i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == 0 && b[i] == 0;
}

} // namespace

namespace vnu::gui {

/* Drag-and-drop state, written by the dnd_declare syscall (a windowed
 * task announces the file under the cursor for the press in flight) and
 * consumed by run()'s mouse handling below. The path stays valid only
 * while the declaring press is held; a fresh press, Esc or a release
 * disarms it. */
static char drag_path[96];
static vnu::wintask::TaskHandle drag_src = vnu::wintask::NO_TASK;

void dnd_declare(const char* path)
{
    drag_src = vnu::wintask::current_handle();
    int i = 0;
    while (path && path[i] && i < static_cast<int>(sizeof(drag_path)) - 1) {
        drag_path[i] = path[i];
        ++i;
    }
    drag_path[i] = 0;
}

int set_resolution(int w, int h)
{
    if (!vnu::vgfx::set_resolution(w, h))
        return -VNU_EINVAL;
    /* Remember the choice: the next boot reads it back before it
     * programs the card (see gfxconf.h). A setting that could not be
     * written is not a reason to refuse the switch the user can see. */
    (void)vnu::gfxconf::store(w, h);
    /* The wallpaper is stretched to the desktop when it is decoded, so
     * it has to be rebuilt for the new geometry or the next frame would
     * blit a frame of the wrong size. */
    vnu::wallpaper::resize();
    /* The desktop redraws every frame, so only the geometry it *keeps*
     * between frames needs fixing up, and that is the event loop's job:
     * it owns the window rectangles and the cursor position. */
    g_relayout = true;
    return 0;
}

void run(const char* open_app)
{
    /* The mode from the last session, while the card is still in text
     * mode: vgfx::set_resolution() only records it here, and
     * enter_gfx_mode() below programs the card for it. */
    (void)vnu::gfxconf::load();
    if (!vnu::vgfx::enter_gfx_mode()) {
        /* The framebuffer comes out of the pool, and a pool with nothing
         * left is the one thing that can stop a desktop. Nothing was
         * programmed, so the console is still the console: say so on it
         * and hand the shell straight back. */
        vnu::tty::write_cstr("vnu: not enough memory for the desktop\r\n");
        return;
    }
    vnu::vgfx::draw_wallpaper();
    vnu::kbd::drain_excess();
    vnu::mouse::init();
    vnu::wintask::init();
    vnu::wallpaper::load();

    vnu::apps::AppEntry apps[vnu::apps::MAX_APPS];
    int app_count = vnu::apps::list(apps, vnu::apps::MAX_APPS);

    Window geom[MAX_TASKS];
    bool minimized[MAX_TASKS] = {};
    TaskHandle order[MAX_TASKS];
    int order_len = 0;
    TaskHandle focused = NO_TASK;
    int cascade = 0;
    bool was_gfx[MAX_TASKS] = {};

    static uint8_t load_buf[65536];

    auto raise_window = [&](TaskHandle h) {
        int j = -1;
        for (int i = 0; i < order_len; ++i)
            if (order[i] == h) {
                j = i;
                break;
            }
        if (j >= 0) {
            for (int i = j; i < order_len - 1; ++i)
                order[i] = order[i + 1];
            --order_len;
        }
        order[order_len++] = h;
        focused = h;
    };

    auto focus_top = [&]() { focused = order_len > 0 ? order[order_len - 1] : NO_TASK; };

    auto window_by_title = [&](const char* name) -> TaskHandle {
        for (int i = 0; i < order_len; ++i) {
            vnu::wintask::Console* c = vnu::wintask::console(order[i]);
            if (c && name_is(name, c->title))
                return order[i];
        }
        return NO_TASK;
    };

    auto window_exists = [&](TaskHandle h) { return vnu::wintask::has_window(h); };

    auto close_window = [&](TaskHandle h) {
        if (!window_exists(h)) {
            vnu::wintask::close_task(h);
            return;
        }
        int j = -1;
        for (int i = 0; i < order_len; ++i)
            if (order[i] == h) {
                j = i;
                break;
            }
        if (j >= 0) {
            for (int i = j; i < order_len - 1; ++i)
                order[i] = order[i + 1];
            --order_len;
        }
        vnu::wintask::close_task(h);
        minimized[h] = false;
        if (focused == h)
            focus_top();
    };

    auto sweep_finished = [&]() {
        for (int i = 0; i < order_len;) {
            TaskHandle h = order[i];
            if (!vnu::wintask::has_window(h)) {
                vnu::wintask::close_task(h);
                minimized[h] = false;
                for (int k = i; k < order_len - 1; ++k)
                    order[k] = order[k + 1];
                --order_len;
            } else {
                ++i;
            }
        }
        if (focused != NO_TASK && !vnu::wintask::has_window(focused))
            focus_top();
    };

    auto spawn_from_icon = [&](int app_idx) {
        const char* name = apps[app_idx].name;
        TaskHandle existing = window_by_title(name);
        if (existing != NO_TASK) {
            minimized[existing] = false;
            raise_window(existing);
            return;
        }
        uint32_t n = vnu::apps::read_bin(name, load_buf, sizeof(load_buf));
        if (n == 0)
            return;
        TaskHandle h = vnu::wintask::spawn(name, load_buf, n, name);
        if (h == NO_TASK)
            return;
        int cx = 36 + (cascade % 6) * 26;
        int cy = PANEL_H + 8 + (cascade % 6) * 22;
        ++cascade;
        geom[h] = size_app_window(cx, cy, vnu::wintask::console(h));
        clamp_to_desktop(geom[h]);
        minimized[h] = false;
        raise_window(h);
    };

    /* Drop targets for an in-flight drag-and-drop, hit in order: the
     * window under the pointer (deliver a DROP message / no-op for the
     * source window), an app icon (launch that app with the file), or
     * the bare desktop (move the file into /root/desktop). */
    auto resolve_drop = [&](int x, int y) {
        if (!drag_path[0])
            return;
        for (int i = order_len - 1; i >= 0; --i) {
            TaskHandle h = order[i];
            if (!window_exists(h) || minimized[h])
                continue;
            const Window& win = geom[h];
            if (!win.w || !win.h)
                continue;
            if (x < win.x || x >= win.x + win.w || y < win.y || y >= win.y + win.h)
                continue;
            if (h == drag_src)
                return; /* dropped back where it came from: no-op */
            vnu::wintask::Console* con = vnu::wintask::console(h);
            if (con && con->gfx)
                vnu::wintask::feed_drop(h, drag_path);
            return;
        }
        int hit = hit_test_icon(apps, app_count, x, y);
        if (hit >= 0) {
            uint32_t n = vnu::apps::read_bin(apps[hit].name, load_buf, sizeof(load_buf));
            if (n != 0) {
                TaskHandle h = vnu::wintask::spawn_argv(apps[hit].name, drag_path,
                                                        load_buf, n, apps[hit].name);
                if (h != NO_TASK) {
                    int cx = 36 + (cascade % 6) * 26;
                    int cy = PANEL_H + 8 + (cascade % 6) * 22;
                    ++cascade;
                    geom[h] = size_app_window(cx, cy, vnu::wintask::console(h));
                    clamp_to_desktop(geom[h]);
                    minimized[h] = false;
                    raise_window(h);
                }
            }
            return;
        }
        if (y >= PANEL_H) {
            /* The bare desktop: move the file there. /root/desktop is
             * created on demand; a name clash or the rare I/O error is
             * silently a no-op. */
            vnu::vfs::mkdir("/root/desktop");
            const char* base = drag_path;
            for (const char* p = drag_path; *p; ++p)
                if (*p == '/')
                    base = p + 1;
            char dst[128];
            int di = 0;
            for (const char* d = "/root/desktop/"; *d && di < 127; ++d)
                dst[di++] = *d;
            for (const char* s = base; *s && di < 127; ++s)
                dst[di++] = *s;
            dst[di] = 0;
            if (vnu::vfs::move_file(drag_path, dst) == 0)
                vnu::wintask::feed_drop(drag_src, drag_path); /* source refreshes */
        }
    };

    int mx = vnu::vgfx::width() / 2;
    int my = vnu::vgfx::height() / 2;
    DragOp drag_op = DragOp::None;
    TaskHandle drag_h = NO_TASK;
    int drag_off_x = 0, drag_off_y = 0;
    int rs_orig_x = 0, rs_orig_y = 0, rs_orig_w = 0, rs_orig_h = 0, rs_mx = 0, rs_my = 0;
    bool left_was_down = false;
    TaskHandle gfx_press_h = NO_TASK;
    int gfx_press_px = 0, gfx_press_py = 0;
    bool have_gfx_press = false;
    bool dnd_active = false;
    int press_gx = 0, press_gy = 0;

    PBtn panel_btns[24];
    int panel_n = 0;
    uint32_t last_tick_sod = 0;

    /* The frame as it was last put together, so this pass can tell what
     * it has to change (see the redraw below). Static because it is a
     * desktop's worth of memory - six windows' cell grids, 12 KiB - and
     * the event loop is not where that belongs; a second session starts
     * by declaring itself a first frame, which resets all of it. */
    static Shown shown[MAX_TASKS];
    static Rect restore[MAX_TASKS + 3];
    static bool redraw[MAX_TASKS];
    /* This pass's reading of each gfx window's canvas, so a window that
     * has to be painted and then checked again is scanned once (see
     * canvas_hash). */
    static uint64_t digest_of[MAX_TASKS];
    static int shown_mx, shown_my;
    static TaskHandle shown_focused = NO_TASK;
    static vnu::vgfx::CursorShape shown_cursor =
        vnu::vgfx::CursorShape::Arrow;
    bool first_frame = true;
    bool redraw_icons = false;
    bool redraw_panel = false;
    /* Set by the sweep above, folded into redraw_panel once this pass's
     * scratch is cleared. */
    bool window_list_changed = false;
    int shown_live = -1;   /* -1 so the first pass redraws the panel */

    /* "gui <app>" — the window is up before the first frame, so the
     * desktop never flashes an empty desktop on the way in. */
    if (open_app && open_app[0]) {
        for (int i = 0; i < app_count; ++i) {
            if (name_is(apps[i].name, open_app)) {
                spawn_from_icon(i);
                break;
            }
        }
    }

    for (;;) {
        /* A resolution change (VNU_SYS_gfx_setmode, or the key below)
         * left the window rectangles and the cursor placed for the old
         * screen. Policy: a mode change never closes a window and never
         * loses what is on it -- it only moves windows back into view.
         * A window too big for the new screen shrinks to fit it (a text
         * window just shows fewer columns/rows: fit_cols/fit_rows read
         * the client area off the rectangle), a window that fits keeps
         * its size and only gets clamped. The cascade offset restarts,
         * since a step chosen for the old geometry would land off the
         * new screen. Everything else the desktop shows is redrawn from
         * scratch every frame, so nothing else needs redoing. */
        bool g_relayout_pending = g_relayout;
        if (g_relayout) {
            g_relayout = false;
            int max_h = vnu::vgfx::height() - PANEL_H;
            for (int h = 0; h < MAX_TASKS; ++h) {
                if (!vnu::wintask::is_running(h))
                    continue;
                if (geom[h].w > vnu::vgfx::width())
                    geom[h].w = vnu::vgfx::width();
                if (geom[h].h > max_h)
                    geom[h].h = max_h;
                clamp_to_desktop(geom[h]);
            }
            cascade = 0;
            if (mx >= vnu::vgfx::width()) mx = vnu::vgfx::width() - 1;
            if (my >= vnu::vgfx::height()) my = vnu::vgfx::height() - 1;
        }

        /* Give every live task a burst; each either blocks on empty
         * input or exits, so this always comes straight back. */
        vnu::wintask::run_all_slices();
        sweep_finished();

        /* The panel is a function of what is open and what has the
         * focus, so anything that changed either of them has to redraw
         * it. A window that is merely alive is not enough: the panel
         * lists windows, and one that appeared or went away is a
         * different list. */
        {
            int live = 0;
            for (int h = 0; h < MAX_TASKS; ++h)
                if (vnu::wintask::has_window(h))
                    ++live;
            if (live != shown_live) {
                window_list_changed = true;
                shown_live = live;
            }
        }

        /* One per-second tick for every gfx-mode window (the analog
         * clock app animates from it). The RTC is the only clock, so on
         * a second boundary we wake each pixel task once with TICK_BYTE. */
        uint32_t sod = vnu::vfs::time_seconds();
        if (sod != last_tick_sod) {
            last_tick_sod = sod;
            vnu::wintask::heartbeat_gfx_tasks();
        }

        /* Hand any pending SIGWINCH to the task it belongs to.
         *
         * Every pass, not once a second: the flag is raised by the
         * resize itself, which is this same loop (a drag) or a syscall
         * from the app (which came through between two passes), and the
         * notice has to arrive at the next boundary rather than up to a
         * second later. A task parked in a read is woken here, so it
         * comes back, is interrupted and redraws at the new size before
         * the next composite. */
        vnu::wintask::raise_winch_signals();

        int key = vnu::kbd::poll_char();
        if (key == vnu::kbd::K_F12) {
            /* Step to the next resolution in the ladder. A desktop key
             * rather than a command: the modes are a property of the
             * display, so the key belongs to the desktop, and the same
             * switch is open to any program through
             * VNU_SYS_gfx_setmode. */
            const vnu::vgfx::Mode* modes = vnu::vgfx::modes();
            for (int i = 0; i < vnu::vgfx::MODE_COUNT; ++i) {
                if (modes[i].width != vnu::vgfx::width() ||
                    modes[i].height != vnu::vgfx::height())
                    continue;
                const vnu::vgfx::Mode& next =
                    modes[(i + 1) % vnu::vgfx::MODE_COUNT];
                (void)set_resolution(next.width, next.height);
                break;
            }
        } else if (key >= 0) {
            if (dnd_active && key == 27) {
                /* Esc cancels an in-flight drag-and-drop. */
                dnd_active = false;
                drag_path[0] = 0;
                drag_src = NO_TASK;
            } else if (key == 27 && focused == NO_TASK) {
                /* Esc with no windows open leaves the desktop. */
                break;
            } else if (focused != NO_TASK && vnu::wintask::is_running(focused)) {
                vnu::wintask::Console* con = vnu::wintask::console(focused);
                if (con) {
                    con->scroll_off = 0;
                    if (con->gfx && key == 27)
                        vnu::wintask::feed_mouse(focused, 3, 0, 0);
                    else
                        vnu::wintask::feed_input(focused, static_cast<char>(key));
                }
                vnu::wintask::run_slice(focused);
            }
        }

        int dx = 0, dy = 0;
        uint8_t buttons = 0;
        int8_t wheel = 0;
        if (vnu::mouse::poll(dx, dy, buttons, wheel)) {
            mx += dx;
            my += dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= vnu::vgfx::width()) mx = vnu::vgfx::width() - 1;
            if (my >= vnu::vgfx::height()) my = vnu::vgfx::height() - 1;

            bool left_down = (buttons & 0x01) != 0;
            bool just_pressed = left_down && !left_was_down;
            left_was_down = left_down;

            if (just_pressed) {
                /* A fresh click disarms whatever the previous press
                 * declared as draggable. */
                drag_path[0] = 0;
                drag_src = NO_TASK;
                dnd_active = false;
                panel_n = layout_panel(panel_btns, focused, apps, app_count);
                int pb = hit_panel(panel_btns, panel_n, mx, my);
                if (pb >= 0) {
                    const PBtn& b = panel_btns[pb];
                    switch (b.kind) {
                    case PB::Close:
                        if (b.handle != NO_TASK)
                            close_window(b.handle);
                        break;
                    case PB::Min:
                        if (b.handle != NO_TASK)
                            minimized[b.handle] = true;
                        break;
                    case PB::Reboot:
                        reboot_now();
                        break;
                    case PB::Exit:
                        goto exit_gui;
                    case PB::App:
                        if (b.handle != NO_TASK) {
                            minimized[b.handle] = false;
                            raise_window(b.handle);
                        }
                        break;
                    default:
                        break;
                    }
                } else if (my >= PANEL_H) {
                    int hit = hit_test_icon(apps, app_count, mx, my);
                    if (hit >= 0)
                        spawn_from_icon(hit);
                    /* Click in a window: topmost first. An icon click
                     * falls through to here as well, and that is what
                     * puts the new window under the one the click was
                     * really on: the app is spawned, then the window
                     * under the pointer is raised over it. */
                    for (int i = order_len - 1; i >= 0; --i) {
                        TaskHandle h = order[i];
                        if (!window_exists(h) || minimized[h])
                            continue;
                        const Window& win = geom[h];
                        vnu::wintask::Console* con = vnu::wintask::console(h);
                        if (!win.w || !win.h)
                            continue;
                        int o0 = mx >= win.x && mx < win.x + win.w;
                        if (!o0 || my < win.y || my >= win.y + win.h)
                            continue;
                        raise_window(h);
                        DragOp op = hit_test_resize(win, mx, my);
                        if (op != DragOp::None) {
                            drag_op = op;
                            drag_h = h;
                            rs_orig_x = win.x;
                            rs_orig_y = win.y;
                            rs_orig_w = win.w;
                            rs_orig_h = win.h;
                            rs_mx = mx;
                            rs_my = my;
                        } else if (con && con->gfx && mx >= win.x + 2 &&
                                   mx < win.x + 2 + (win.w - 8) &&
                                   my >= win.y + TITLE_H + 2 &&
                                   my < win.y + TITLE_H + 2 + (win.h - TITLE_H - 12)) {
                            int cw = win.w - 8;
                            int ch = win.h - TITLE_H - 12;
                            if (cw < 1)
                                cw = 1;
                            if (ch < 1)
                                ch = 1;
                            /* Canvas pixels, so a click lands where
                             * the app drew it: the canvas is the
                             * window's size, so this is the same
                             * mapping the blit used. */
                            const int src_w = con->pix_w > 0 ? con->pix_w : vnu::wintask::GFX_W;
                            const int src_h = con->pix_h > 0 ? con->pix_h : vnu::wintask::GFX_H;
                            int px = (mx - (win.x + 2)) * src_w / cw;
                            int py = (my - (win.y + TITLE_H + 2)) * src_h / ch;
                            vnu::wintask::feed_mouse(h, 1, px, py);
                            gfx_press_h = h;
                            gfx_press_px = px;
                            gfx_press_py = py;
                            have_gfx_press = true;
                            press_gx = mx;
                            press_gy = my;
                        } else if (hit_test_scrollbar(win, mx, my)) {
                            if (con && !con->gfx && con->hist_n > 0) {
                                int top = win.y + TITLE_H + 2;
                                int bh = win.h - (TITLE_H + 4);
                                int maxoff = con->hist_n;
                                int off = ((my - top) * maxoff) / bh;
                                if (off > maxoff)
                                    off = maxoff;
                                con->scroll_off = off;
                            }
                        } else if (hit_test_titlebar(win, mx, my)) {
                            drag_op = DragOp::Move;
                            drag_h = h;
                            drag_off_x = mx - win.x;
                            drag_off_y = my - win.y;
                        }
                        break;
                    }
                    }
                }

            /* A press armed a draggable item and the pointer wanders
             * at least ~8px with the button held: that press is now a
             * drag of drag_path. Tell the source app immediately
             * (mouse button 4 = "your click became a drag", no
             * release follows) so it doesn't read the release as a
             * click. */
            if (left_down && !dnd_active && drag_path[0] &&
                gfx_press_h == drag_src &&
                vnu::wintask::is_running(gfx_press_h) &&
                (mx - press_gx) * (mx - press_gx) +
                        (my - press_gy) * (my - press_gy) >= 64) {
                dnd_active = true;
                vnu::wintask::feed_mouse(gfx_press_h, 4, gfx_press_px, gfx_press_py);
                have_gfx_press = false;
            }

            if (!left_down) {
                if (dnd_active) {
                    resolve_drop(mx, my);
                }
                dnd_active = false;
                drag_path[0] = 0;
                drag_src = NO_TASK;
                drag_op = DragOp::None;
                drag_h = NO_TASK;
                if (have_gfx_press && gfx_press_h != NO_TASK &&
                    vnu::wintask::is_running(gfx_press_h)) {
                    vnu::wintask::Console* con = vnu::wintask::console(gfx_press_h);
                    if (con && con->gfx)
                        vnu::wintask::feed_mouse(gfx_press_h, 2, gfx_press_px, gfx_press_py);
                }
                have_gfx_press = false;
            }

            if (drag_op != DragOp::None && drag_h != NO_TASK) {
                Window& win = geom[drag_h];
                vnu::wintask::Console* con = vnu::wintask::console(drag_h);
                bool g = con && con->gfx;
                int minw = g ? GFX_MIN_CLIENT_W + 8 : 20 * CELL_W + 8;
                int maxw = g ? vnu::vgfx::width() - 8 : CON_COLS * CELL_W + 8;
                int minh = g ? TITLE_H + GFX_MIN_CLIENT_H + 12
                             : TITLE_H + 3 * CELL_H + 12;
                int maxh = g ? vnu::vgfx::height() - (PANEL_H + 8)
                             : TITLE_H + CON_ROWS * CELL_H + 12;
                auto cw = [&](int v) -> int { return v < minw ? minw : (v > maxw ? maxw : v); };
                auto ch = [&](int v) -> int { return v < minh ? minh : (v > maxh ? maxh : v); };
                switch (drag_op) {
                case DragOp::Move:
                    win.x = mx - drag_off_x;
                    win.y = my - drag_off_y;
                    break;
                case DragOp::ResizeRight:
                    win.w = cw(rs_orig_w + (mx - rs_mx));
                    break;
                case DragOp::ResizeBottom:
                    win.h = ch(rs_orig_h + (my - rs_my));
                    break;
                case DragOp::ResizeCorner:
                    win.w = cw(rs_orig_w + (mx - rs_mx));
                    win.h = ch(rs_orig_h + (my - rs_my));
                    break;
                case DragOp::ResizeLeft: {
                    int right = rs_orig_x + rs_orig_w;
                    win.w = cw(rs_orig_w - (mx - rs_mx));
                    win.x = right - win.w;
                    break;
                }
                case DragOp::ResizeTop: {
                    int bottom = rs_orig_y + rs_orig_h;
                    win.h = ch(rs_orig_h - (my - rs_my));
                    win.y = bottom - win.h;
                    break;
                }
                case DragOp::ResizeTopLeft: {
                    int right = rs_orig_x + rs_orig_w;
                    int bottom = rs_orig_y + rs_orig_h;
                    win.w = cw(rs_orig_w - (mx - rs_mx));
                    win.x = right - win.w;
                    win.h = ch(rs_orig_h - (my - rs_my));
                    win.y = bottom - win.h;
                    break;
                }
                case DragOp::ResizeTopRight: {
                    int bottom = rs_orig_y + rs_orig_h;
                    win.w = cw(rs_orig_w + (mx - rs_mx));
                    win.h = ch(rs_orig_h - (my - rs_my));
                    win.y = bottom - win.h;
                    break;
                }
                case DragOp::ResizeBottomLeft: {
                    int right = rs_orig_x + rs_orig_w;
                    win.w = cw(rs_orig_w - (mx - rs_mx));
                    win.x = right - win.w;
                    win.h = ch(rs_orig_h + (my - rs_my));
                    break;
                }
                default:
                    break;
                }
                clamp_to_desktop(win);
                if (g && con->pixel)
                    follow_canvas(con, win);
            }

            /* Mouse-wheel: scroll the focused text window. */
            if (wheel != 0 && focused != NO_TASK) {
                vnu::wintask::Console* con = vnu::wintask::console(focused);
                const Window& win = geom[focused];
                if (con && !con->gfx && mx >= win.x && mx < win.x + win.w &&
                    my >= win.y && my < win.y + win.h) {
                    if (wheel > 0)
                        ++con->scroll_off;
                    else
                        --con->scroll_off;
                    if (con->scroll_off < 0)
                        con->scroll_off = 0;
                    if (con->scroll_off > con->hist_n)
                        con->scroll_off = con->hist_n;
                }
            }
        }

        /* The scratch for this pass: the regions to rebuild, the windows
         * to draw again, and the two elements that are a function of
         * the window list rather than of a window. */
        int restore_n = 0;
        for (int i = 0; i < MAX_TASKS; ++i)
            redraw[i] = false;
        redraw_icons = false;
        /* Both of these are this pass's, cleared here and set again by
         * whatever below has something new to say: the focus that moved,
         * a window that left pixels bare, a canvas that was reallocated.
         *
         * The panel is the expensive one - it is a full-width strip of
         * buttons and icons - so a flag left standing across passes would
         * repaint it (and hand the host its 1024x34) for the rest of the
         * session, once per pass, over a panel that has not changed since
         * the last time. */
        redraw_panel = false;
        if (window_list_changed) {
            redraw_panel = true;
            window_list_changed = false;
        }

        /* Redraw, as layers, and only where the layer underneath has
         * actually changed (see the damage notes in vga_gfx.h: at 32bpp
         * a full frame is 3 MiB to the host, and almost every pass here
         * is about one window, one line of text or the pointer).
         *
         * The three questions this pass answers, in order:
         *
         *   - Which regions of the screen are no longer what they were?
         *     A window's old ground when it moved, was minimized, closed
         *     or was raised over another, and the pointer's old spot,
         *     which belongs to whatever is under it.
         *   - What has to be drawn back into those regions? The
         *     wallpaper, then every window that overlaps one - not
         *     because those windows changed, but because the wallpaper
         *     just went under them - then the panel and the icons if the
         *     region reaches them, and the pointer itself.
         *   - Which windows have new pixels of their own? Drawn in
         *     z-order afterwards, so a window that changed is never
         *     painted under one that did not.
         */
        if (first_frame || g_relayout_pending) {
            /* Nothing on the display is known to match this desktop: a
             * session that has just started, or a mode change that took
             * the whole frame with it. */
            restore[restore_n++] = {0, 0, vnu::vgfx::width(), vnu::vgfx::height()};
        }
        const bool pointer_moved = shown_mx != mx || shown_my != my;
        if (pointer_moved) {
            /* The pointer is the top layer, so where it was is a region
             * that has to be put back like any other. */
            restore[restore_n++] = {shown_mx, shown_my, CURSOR_BOX, CURSOR_BOX};
            shown_mx = mx;
            shown_my = my;
        }
        if (shown_focused != focused) {
            /* Focus is drawn into the title bars, in both the window
             * that had it and the one that took it. */
            if (shown_focused >= 0 && shown_focused < MAX_TASKS)
                redraw[shown_focused] = true;
            if (focused >= 0 && focused < MAX_TASKS)
                redraw[focused] = true;
            shown_focused = focused;
            redraw_panel = true;
        }

        for (int h = 0; h < MAX_TASKS; ++h) {
            vnu::wintask::Console* con = vnu::wintask::console(h);
            const bool live = vnu::wintask::has_window(h) && con;
            /* If the app flipped between text and gfx mode, re-fit the
             * window to the new client area (an app that asked for its
             * shared canvas, or one that never did). Done here, before
             * anything compares rectangles, so the fit is one change
             * rather than two. */
            if (live && con->gfx != was_gfx[h]) {
                was_gfx[h] = con->gfx;
                Window sized = size_app_window(geom[h].x, geom[h].y, con);
                geom[h].w = sized.w;
                geom[h].h = sized.h;
                clamp_to_desktop(geom[h]);
            }
            /* A window is up when the task has one and it is not
             * minimized; anything else is not drawn and leaves nothing
             * of itself on the screen. */
            const bool up = live && !minimized[h];
            const bool was_up = shown[h].up;
            /* A gfx window's pixels are the app's own, and this is where
             * they are read: one digest per window per pass, for the two
             * questions below that need one. A text window has no canvas
             * and reads nothing. */
            const uint64_t digest = (up && con->gfx)
                                        ? canvas_hash(*con)
                                        : 0;
            digest_of[h] = digest;
            if (up == was_up && up && same_window(shown[h], geom[h], *con, digest)) {
                /* Same window, same place, same pixels: the frame on
                 * the screen already holds it. */
                continue;
            }
            if (up) {
                redraw[h] = true;
                if (was_up && !same_rect(shown[h].rect, geom[h]))
                    restore[restore_n++] = both_rects(shown[h].rect, geom[h]);
            } else if (was_up) {
                /* It went away, or was minimized: what it covered
                 * belongs to the desktop again. */
                const Window& old = shown[h].rect;
                restore[restore_n++] = {old.x, old.y, old.w, old.h};
            }
        }

        /* Layer 1: the wallpaper, in the regions that are bare. Drawn
         * once when the frame is new and afterwards only here - it is a
         * scene of whole screen, and a window covers it rather than
         * changing it, so there is nothing else to redraw. */
        for (int i = 0; i < restore_n; ++i) {
            const Rect& r = restore[i];
            vnu::vgfx::clip_set(r.x, r.y, r.w, r.h);
            vnu::vgfx::draw_wallpaper();
            vnu::vgfx::clip_pop();
            vnu::vgfx::damage_add(r.x, r.y, r.w, r.h);
        }

        /* Anything drawn into a region that has just been put back has
         * to go again on top of the wallpaper, whether it changed or
         * not. */
        for (int i = 0; i < restore_n; ++i)
            for (int h = 0; h < MAX_TASKS; ++h)
                if (vnu::wintask::has_window(h) && !minimized[h] &&
                    overlaps(restore[i], geom[h]))
                    redraw[h] = true;
        if (restore_n > 0) {
            const Rect icons = icon_area(app_count);
            const Rect panel = {0, 0, vnu::vgfx::width(), PANEL_H};
            for (int i = 0; i < restore_n; ++i) {
                if (overlaps(restore[i], icons))
                    redraw_icons = true;
                if (overlaps(restore[i], panel))
                    redraw_panel = true;
            }
        }

        /* Layer 2: the icons, which the desktop never changes after it
         * has listed them. */
        if (redraw_icons) {
            draw_icons(apps, app_count);
            const Rect icons = icon_area(app_count);
            vnu::vgfx::damage_add(icons.x, icons.y, icons.w, icons.h);
        }

        /* Layer 3: the windows, in z-order, so one that changed is never
         * painted under one that did not. A gfx window is entered here
         * when its canvas has new pixels in it: the app writes them
         * itself, into the pages the compositor reads, so the canvas is
         * where "something changed" is answered (see canvas_hash). */
        for (int i = 0; i < order_len; ++i) {
            TaskHandle h = order[i];
            if (minimized[h])
                continue;
            vnu::wintask::Console* con = vnu::wintask::console(h);
            if (!con)
                continue;
            if (redraw[h] || (con->gfx && canvas_changed(shown[h], *con, digest_of[h]))) {
                /* An app that resized its own canvas has moved the
                 * window without touching it, so the chrome is brought
                 * back into agreement before the blit: the two have to
                 * match or the app's layout is scaled, which is what the
                 * fixed canvas used to do. */
                if (con->gfx && adopt_canvas(con, geom[h])) {
                    vnu::vgfx::damage_add(geom[h].x, geom[h].y, geom[h].w, geom[h].h);
                    redraw_panel = true;
                }
                const Window& win = geom[h];
                /* A window with only occluded pixels on screen is not
                 * composited: drawing it would be overdraw nothing shows,
                 * and damaging it would charge the host for a screen that
                 * did not change. It stays alive and keeps its canvas -
                 * as soon as anything above it moves, it is drawn again. */
                if (fully_covered(i, win, geom, order, order_len, minimized))
                    continue;
                if (con->gfx)
                    draw_gfx_window(win, *con, h == focused);
                else
                    draw_console_window(win, *con, h == focused);
                vnu::vgfx::damage_add(win.x, win.y, win.w, win.h);
                /* What the frame now holds, so the next pass can tell a
                 * window that needs painting from one that does not.
                 * A window that was skipped as fully covered keeps the
                 * old digest on purpose: nothing of it is on the screen,
                 * so the next pass must still find it worth drawing. */
                if (con->gfx) {
                    shown[h].canvas = digest_of[h];
                    shown[h].canvas_w = con->pix_w;
                    shown[h].canvas_h = con->pix_h;
                }
            }
        }

        /* Layer 4: the panel, which is a function of the focus, the open
         * windows and the mode - so it changes when any of those do, and
         * not otherwise. */
        if (redraw_panel) {
            panel_n = layout_panel(panel_btns, focused, apps, app_count);
            draw_panel(panel_btns, panel_n, apps, app_count);
            vnu::vgfx::damage_add(0, 0, vnu::vgfx::width(), PANEL_H);
        }

        if (dnd_active && drag_path[0]) {
            /* Drag ghost: a little document page riding the cursor, with
             * the dragged file's bare name beside it. It rides with the
             * pointer, so the pointer's own restore region covers the
             * ground it vacates; the name beside it can reach further
             * than the pointer's box, so that is the extra damage. */
            const int gx = mx - 10, gy = my - 4;
            vnu::vgfx::fill_rect(gx, gy, 13, 16, vnu::vgfx::COLOR_WHITE);
            vnu::vgfx::rect(gx, gy, 13, 16, vnu::vgfx::COLOR_BLACK);
            vnu::vgfx::hline(gx + 2, gy + 5, 9, vnu::vgfx::COLOR_DGRAY);
            vnu::vgfx::hline(gx + 2, gy + 10, 9, vnu::vgfx::COLOR_DGRAY);
            const char* base = drag_path;
            for (const char* p = drag_path; *p; ++p)
                if (*p == '/')
                    base = p + 1;
            const int lx = gx + 17, ly = gy + 3;
            vnu::vgfx::draw_string8(lx + 1, ly + 1, base, vnu::vgfx::COLOR_BLACK);
            vnu::vgfx::draw_string8(lx, ly, base, vnu::vgfx::COLOR_WHITE);
            vnu::vgfx::damage_add(gx - 1, gy - 1,
                                  lx - gx + vnu::vgfx::text_width8(base) + 1,
                                  18);
        }

        vnu::vgfx::CursorShape cursor = vnu::vgfx::CursorShape::Arrow;
        if (drag_op != DragOp::None && drag_h != NO_TASK) {
            cursor = mouse_shape_for(drag_op);
        } else if (!left_was_down) {
            /* Hover: point at the topmost window under the pointer and
             * show the resize arrows while it is on a resizable edge. */
            for (int i = order_len - 1; i >= 0; --i) {
                TaskHandle h = order[i];
                if (!window_exists(h) || minimized[h])
                    continue;
                const Window& win = geom[h];
                if (mx >= win.x && mx < win.x + win.w &&
                    my >= win.y && my < win.y + win.h) {
                    DragOp op = hit_test_resize(win, mx, my);
                    if (op != DragOp::None)
                        cursor = mouse_shape_for(op);
                    break;
                }
            }
        }
        if (cursor == vnu::vgfx::CursorShape::Arrow)
            vnu::vgfx::draw_cursor(mx, my, vnu::vgfx::COLOR_BLACK);
        else
            vnu::vgfx::draw_cursor_at(mx - 8, my - 8, cursor);
        /* The pointer is drawn every pass, and a pass that draws the same
         * pointer on the same spot has changed nothing: its bitmap lands
         * on the pixels that already hold it. Damaging it anyway would
         * charge the host for a transfer of a screen that did not move,
         * once per pass, for as long as the session lasts - which is what
         * an occlusion check reads as the session's own cost. So the box
         * is damaged when the pointer moved onto it, when its shape
         * changed under it, or when the frame is new. */
        if (first_frame || pointer_moved || shown_cursor != cursor)
            vnu::vgfx::damage_add(mx, my, CURSOR_BOX, CURSOR_BOX);
        shown_cursor = cursor;

        /* What the frame now holds, which is what the next pass
         * compares against. Taken after the drawing, and only for the
         * windows that were up: a gfx window's cells are not its pixels
         * and are left alone. */
        for (int h = 0; h < MAX_TASKS; ++h) {
            vnu::wintask::Console* con = vnu::wintask::console(h);
            shown[h].up = vnu::wintask::has_window(h) && con && !minimized[h];
            if (!shown[h].up)
                continue;
            shown[h].rect = geom[h];
            memcpy(shown[h].title, con->title, sizeof(shown[h].title));
            shown[h].hist_tail = con->hist_tail;
            shown[h].hist_n = con->hist_n;
            shown[h].scroll_off = con->scroll_off;
            shown[h].cur_row = con->cur_row;
            shown[h].cur_col = con->cur_col;
            memcpy(shown[h].cells, con->cell, sizeof(shown[h].cells));
        }

        vnu::vgfx::present();
        first_frame = false;
    }

exit_gui:
    for (int i = 0; i < order_len; ++i)
        vnu::wintask::close_task(order[i]);
    /* The mouse goes with the desktop: it was told to report for this
     * event loop, and an event loop that has stopped reading it would
     * leave its last packet in the controller, in front of every
     * keystroke the shell is about to be sent. */
    vnu::mouse::shutdown();
    /* Nothing is left on screen to draw the wallpaper into, and the
     * pool is worth more to the console than to a picture nobody is
     * looking at. */
    vnu::wallpaper::unload();
    vnu::vgfx::exit_to_text();
}

} // namespace vnu::gui
