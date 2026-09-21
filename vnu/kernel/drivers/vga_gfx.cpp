#include <vnu/vga_gfx.h>
#include <vnu/wallpaper.h>

extern "C" void* memcpy(void* dst, const void* src, unsigned long count);

namespace {

inline void outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

inline void outw(uint16_t port, uint16_t val)
{
    asm volatile("outw %0,%1" : : "a"(val), "Nd"(port));
}

inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    asm volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* --- Bochs VBE "dispi" interface (what QEMU's std VGA emulates).
 * No BIOS calls are needed to switch resolution in protected mode:
 * any 8-bpp linear-framebuffer size up to the card's VRAM can be
 * selected by writing four registers. The LFB window (0xFD000000,
 * identity-mapped by mm/paging.cpp) then holds the full frame. */
constexpr uint16_t VBE_INDEX_PORT = 0x01CE;
constexpr uint16_t VBE_DATA_PORT = 0x01CF;

constexpr uint16_t VBE_INDEX_ID = 0x00;
constexpr uint16_t VBE_INDEX_XRES = 0x01;
constexpr uint16_t VBE_INDEX_YRES = 0x02;
constexpr uint16_t VBE_INDEX_BPP = 0x03;
constexpr uint16_t VBE_INDEX_ENABLE = 0x04;
constexpr uint16_t VBE_INDEX_VIRT_WIDTH = 0x06;
constexpr uint16_t VBE_INDEX_X_OFFSET = 0x08;
constexpr uint16_t VBE_INDEX_Y_OFFSET = 0x09;

constexpr uint16_t VBE_DISABLED = 0x00;
constexpr uint16_t VBE_ENABLED = 0x01;
constexpr uint16_t VBE_8BIT_DAC = 0x04;
constexpr uint16_t VBE_NOCLEARMEM = 0x80;
constexpr uint16_t VBE_LFB = 0x40;

/* QEMU 11's stdvga exposes VRAM as PCI BAR0 at 0xFD000000 (16 MiB
 * window; older QEMU used a fixed 0xE0000000 alias). paging
 * identity-maps it so present() can memcpy straight to it. */
constexpr uintptr_t VBE_LFB_ADDR = 0xFD000000u;

uint16_t vbe_read(uint16_t index)
{
    outw(VBE_INDEX_PORT, index);
    return inw(VBE_DATA_PORT);
}

void vbe_write(uint16_t index, uint16_t val)
{
    outw(VBE_INDEX_PORT, index);
    outw(VBE_DATA_PORT, val);
}

struct VgaState {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t crtc[25];
    uint8_t gc[9];
    uint8_t ac[21];
};

VgaState g_saved{};
bool g_have_saved = false;

void write_registers(uint8_t misc, const uint8_t* seq, const uint8_t* crtc,
                      const uint8_t* gc, const uint8_t* ac)
{
    outb(0x3C2, misc);

    for (int i = 0; i < 5; ++i) {
        outb(0x3C4, static_cast<uint8_t>(i));
        outb(0x3C5, seq[i]);
    }

    /* Unlock CRTC indices 0-7 (clear the write-protect bit in index 0x11)
     * before writing the rest of the table. */
    outb(0x3D4, 0x11);
    outb(0x3D5, static_cast<uint8_t>(inb(0x3D5) & 0x7F));
    for (int i = 0; i < 25; ++i) {
        outb(0x3D4, static_cast<uint8_t>(i));
        outb(0x3D5, crtc[i]);
    }

    for (int i = 0; i < 9; ++i) {
        outb(0x3CE, static_cast<uint8_t>(i));
        outb(0x3CF, gc[i]);
    }

    for (int i = 0; i < 21; ++i) {
        (void)inb(0x3DA); /* reset attribute controller flip-flop */
        outb(0x3C0, static_cast<uint8_t>(i));
        outb(0x3C0, ac[i]);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20); /* re-enable video output (PAS bit) */
}

void read_registers(VgaState& s)
{
    s.misc = inb(0x3CC);

    for (int i = 0; i < 5; ++i) {
        outb(0x3C4, static_cast<uint8_t>(i));
        s.seq[i] = inb(0x3C5);
    }
    for (int i = 0; i < 25; ++i) {
        outb(0x3D4, static_cast<uint8_t>(i));
        s.crtc[i] = inb(0x3D5);
    }
    for (int i = 0; i < 9; ++i) {
        outb(0x3CE, static_cast<uint8_t>(i));
        s.gc[i] = inb(0x3CF);
    }
    for (int i = 0; i < 21; ++i) {
        (void)inb(0x3DA);
        outb(0x3C0, static_cast<uint8_t>(i));
        s.ac[i] = inb(0x3C1);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);
}

/* --- Font capture: pull the 8x16 glyphs currently loaded in VGA plane 2
 * (the font the text console is rendering with) before we leave text
 * mode, so graphics-mode text keeps looking like the console. */
uint8_t g_font[256][16];
uint8_t g_font8[256][8];
bool g_have_font = false;

void capture_font()
{
    outb(0x3C4, 2);
    uint8_t seq2 = inb(0x3C5);
    outb(0x3C4, 4);
    uint8_t seq4 = inb(0x3C5);
    outb(0x3CE, 4);
    uint8_t gc4 = inb(0x3CF);
    outb(0x3CE, 5);
    uint8_t gc5 = inb(0x3CF);
    outb(0x3CE, 6);
    uint8_t gc6 = inb(0x3CF);

    outb(0x3C4, 2);
    outb(0x3C5, 0x04); /* write plane 2 only */
    outb(0x3C4, 4);
    outb(0x3C5, 0x07); /* sequential addressing */
    outb(0x3CE, 4);
    outb(0x3CF, 0x02); /* read plane 2 */
    outb(0x3CE, 5);
    outb(0x3CF, 0x00); /* read mode 0 */
    outb(0x3CE, 6);
    outb(0x3CF, 0x00); /* map at 0xA0000, no odd/even */

    volatile uint8_t* src = reinterpret_cast<volatile uint8_t*>(0xA0000);
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 16; ++r)
            g_font[c][r] = src[c * 32 + r];

    /* Derive the compact 8x8 font by OR-pairing adjacent rows of the
     * captured 8x16 glyphs, so the desktop's small text keeps the
     * exact same letterforms (just denser) instead of a second,
     * discontiguous typeface. */
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 8; ++r)
            g_font8[c][r] = static_cast<uint8_t>(g_font[c][r * 2] |
                                                 g_font[c][r * 2 + 1]);

    outb(0x3C4, 2);
    outb(0x3C5, seq2);
    outb(0x3C4, 4);
    outb(0x3C5, seq4);
    outb(0x3CE, 4);
    outb(0x3CF, gc4);
    outb(0x3CE, 5);
    outb(0x3CF, gc5);
    outb(0x3CE, 6);
    outb(0x3CF, gc6);

    g_have_font = true;
}

/* Catppuccin Mocha DAC palette for the first 16 palette registers (the
 * ones COLOR_* references). The desktop renders 8-bpp palette indices,
 * so programming the DAC once restyles every surface — wallpaper, window
 * chrome, icons and the gfx apps' own drawing — with no app-code churn.
 * Values are 6-bit per channel (0..63), as the VGA DAC expects. */
constexpr uint8_t CATT_PAL[16][3] = {
    {7, 7, 11},    /*  0 BLACK   base      #1e1e2e */
    {34, 45, 62},  /*  1 BLUE    blue      #89b4fa */
    {41, 56, 40},  /*  2 GREEN   green     #a6e3a1 */
    {29, 49, 59},  /*  3 CYAN    sapphire  #74c7ec */
    {60, 34, 42},  /*  4 RED     red       #f38ba8 */
    {50, 41, 61},  /*  5 MAGENTA mauve     #cba6f7 */
    {62, 44, 33},  /*  6 BROWN   peach     #fab387 */
    {12, 12, 17},  /*  7 LGRAY   surface0  #313244 */
    {6, 6, 9},     /*  8 DGRAY   mantle    #181825 */
    {45, 47, 63},  /*  9 LBLUE   lavender  #b4befe */
    {37, 56, 53},  /* 10 LGREEN  teal      #94e2d5 */
    {34, 55, 58},  /* 11 LCYAN   sky       #89dceb */
    {58, 40, 43},  /* 12 LRED    maroon    #eba0ac */
    {61, 48, 57},  /* 13 LMAGENTA pink     #f5c2e7 */
    {62, 56, 43},  /* 14 YELLOW  yellow    #f9e2af */
    {51, 53, 61},  /* 15 WHITE   text      #cdd6f4 */
};

uint8_t g_saved_pal[16][3];
bool g_have_saved_pal = false;

void load_palette(const uint8_t (&pal)[16][3])
{
    for (int i = 0; i < 16; ++i) {
        outb(0x3C8, static_cast<uint8_t>(i));
        outb(0x3C9, pal[i][0]);
        outb(0x3C9, pal[i][1]);
        outb(0x3C9, pal[i][2]);
    }
}

void save_palette()
{
    for (int i = 0; i < 16; ++i) {
        outb(0x3C7, static_cast<uint8_t>(i));
        g_saved_pal[i][0] = inb(0x3C9);
        g_saved_pal[i][1] = inb(0x3C9);
        g_saved_pal[i][2] = inb(0x3C9);
    }
    /* Reading leaves the DAC in read mode with a stale write pointer;
     * reset it by writing index 0 and clocking out three dummies. */
    outb(0x3C8, 0);
    inb(0x3C9);
    inb(0x3C9);
    inb(0x3C9);
    g_have_saved_pal = true;
}

uint8_t g_backbuf[vnu::vgfx::WIDTH * vnu::vgfx::HEIGHT];

/* Quarter-sine profile (0..255) used to carve the wallpaper's hill
 * silhouettes; indexed by column so the ranges stay procedural. */
const uint8_t HILL_T[64] = {
    0,   31,  62,  92,  120, 146, 170, 191,
    209, 224, 236, 245, 251, 254, 255, 254,
    251, 245, 236, 224, 209, 191, 170, 146,
    120, 92,  62,  31,  0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

} // namespace

namespace vnu::vgfx {

void enter_gfx_mode()
{
    if (!g_have_font)
        capture_font();
    if (!g_have_saved) {
        read_registers(g_saved);
        g_have_saved = true;
    }

    /* Bochs VBE (the "std" VGA in QEMU): disable first, then select
     * the size/colour depth, then re-enable with the linear-framebuffer
     * bit set so the whole frame is a flat 8-bpp array at 0xFD000000.
     * With LFB set, the bank register (index 5) is ignored. */
    vbe_write(VBE_INDEX_X_OFFSET, 0);
    vbe_write(VBE_INDEX_Y_OFFSET, 0);
    vbe_write(VBE_INDEX_ENABLE, VBE_DISABLED);
    vbe_write(VBE_INDEX_XRES, WIDTH);
    vbe_write(VBE_INDEX_YRES, HEIGHT);
    vbe_write(VBE_INDEX_VIRT_WIDTH, WIDTH);
    vbe_write(VBE_INDEX_BPP, 8);
    vbe_write(VBE_INDEX_ENABLE, VBE_ENABLED | VBE_LFB | VBE_8BIT_DAC);

    /* Swap in the Catppuccin DAC the first time we enter graphics mode;
     * the boot palette is saved so exit_to_text() can hand it back. */
    if (!g_have_saved_pal)
        save_palette();
    load_palette(CATT_PAL);
}

void exit_to_text()
{
    if (!g_have_saved)
        return;
    if (g_have_saved_pal)
        load_palette(g_saved_pal);
    vbe_write(VBE_INDEX_ENABLE, VBE_DISABLED);
    write_registers(g_saved.misc, g_saved.seq, g_saved.crtc, g_saved.gc, g_saved.ac);
}

void clear(uint8_t color)
{
    for (int i = 0; i < WIDTH * HEIGHT; ++i)
        g_backbuf[i] = color;
}

/* Desktop wallpaper: a three-band sky (dithered, like clear_gradient),
 * a big sun, a few clouds and two silhouetted hill ranges, drawn every
 * frame so windows and icons paint on top of it. Cheap enough to be a
 * per-frame clear (slightly more work than the plain gradient).
 *
 * If a /wallpaper picture was decoded and quantized at startup (see
 * gui/wallpaper.cpp), its ready-made palette frame wins and this
 * procedural scene only shows when there is no wallpaper file. */
void draw_wallpaper()
{
    if (vnu::wallpaper::ready()) {
        memcpy(g_backbuf, vnu::wallpaper::frame(),
               static_cast<unsigned long>(WIDTH * HEIGHT));
        return;
    }
    static const uint8_t B4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    /* Sky: bright blue at the top, fading through cyan toward a pale
     * horizon (three dithered bands so the 16-color palette is smooth). */
    for (int y = 0; y < HEIGHT; ++y) {
        unsigned t = (unsigned)y * 256u / (unsigned)HEIGHT;
        uint8_t top, bottom;
        unsigned lo, hi;
        if (t <= 96) {
            top = COLOR_LBLUE;
            bottom = COLOR_LCYAN;
            lo = 0;
            hi = 96;
        } else if (t <= 168) {
            top = COLOR_LCYAN;
            bottom = COLOR_CYAN;
            lo = 96;
            hi = 168;
        } else {
            top = COLOR_CYAN;
            bottom = COLOR_LGRAY;
            lo = 168;
            hi = 255;
        }
        unsigned u = (t - lo) * 256u / (hi - lo + 1);
        const uint8_t* bayer = B4[y & 3];
        uint8_t* row = g_backbuf + y * WIDTH;
        for (int x = 0; x < WIDTH; ++x)
            row[x] = (u >= (unsigned)(bayer[x & 3] * 17)) ? bottom : top;
    }

    /* Sun: peach halo around a warm yellow core (Mocha dusk). */
    fill_circle(848, 150, 58, COLOR_BROWN);
    fill_circle(848, 150, 40, COLOR_YELLOW);

    /* Clouds: puffy white blobs. */
    fill_circle(180, 140, 26, COLOR_WHITE);
    fill_circle(206, 150, 26, COLOR_WHITE);
    fill_circle(148, 152, 22, COLOR_WHITE);
    fill_circle(560, 120, 20, COLOR_WHITE);
    fill_circle(582, 128, 20, COLOR_WHITE);
    fill_circle(540, 130, 16, COLOR_WHITE);

    /* Hills: per-column sine-profile silhouettes (far range first, then
     * a nearer, greener range, then a ground strip). */
    uint8_t* row7 = g_backbuf;
    for (int x = 0; x < WIDTH; ++x) {
        int idx = (x * 3 * 64 / WIDTH) % 64;
        int h = 150 * static_cast<int>(HILL_T[idx]) / 256;
        for (int y = 700 - h; y < 700; ++y)
            g_backbuf[y * WIDTH + x] = COLOR_DGRAY;
    }
    for (int x = 0; x < WIDTH; ++x) {
        int idx = (x * 2 * 64 / WIDTH + 16) % 64;
        int h = 110 * static_cast<int>(HILL_T[idx]) / 256;
        for (int y = 748 - h; y < 748; ++y)
            g_backbuf[y * WIDTH + x] = COLOR_GREEN;
    }
    for (int y = 748; y < HEIGHT; ++y) {
        uint8_t* row = g_backbuf + y * WIDTH;
        for (int x = 0; x < WIDTH; ++x)
            row[x] = COLOR_GREEN;
    }
    (void)row7;
}

/* Vertical background fade between two palette colors using a 4x4
 * Bayer dither, so even the tiny 16-color VGA palette gets a smooth
 * gradient. `top` is the first row's color, `bottom` the last. */
void clear_gradient(uint8_t top, uint8_t bottom)
{
    static const uint8_t B4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    for (int y = 0; y < HEIGHT; ++y) {
        unsigned t = (unsigned)y * 256u / (unsigned)HEIGHT;
        const uint8_t* bayer = B4[y & 3];
        uint8_t* row = g_backbuf + y * WIDTH;
        for (int x = 0; x < WIDTH; ++x)
            row[x] = (t >= (unsigned)(bayer[x & 3] * 17)) ? bottom : top;
    }
}

void put_pixel(int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT)
        return;
    g_backbuf[y * WIDTH + x] = color;
}

void fill_rect(int x, int y, int w, int h, uint8_t color)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            put_pixel(x + i, y + j, color);
}

void hline(int x, int y, int w, uint8_t color)
{
    for (int i = 0; i < w; ++i)
        put_pixel(x + i, y, color);
}

void vline(int x, int y, int h, uint8_t color)
{
    for (int j = 0; j < h; ++j)
        put_pixel(x, y + j, color);
}

void rect(int x, int y, int w, int h, uint8_t color)
{
    hline(x, y, w, color);
    hline(x, y + h - 1, w, color);
    vline(x, y, h, color);
    vline(x + w - 1, y, h, color);
}

void line(int x0, int y0, int x1, int y1, uint8_t color)
{
    /* Bresenham. Works for any octant (abs deltas). */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void circle(int cx, int cy, int r, uint8_t color)
{
    /* Midpoint circle, drawn one pixel per octant. */
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        put_pixel(cx + x, cy + y, color);
        put_pixel(cx - x, cy + y, color);
        put_pixel(cx + x, cy - y, color);
        put_pixel(cx - x, cy - y, color);
        put_pixel(cx + y, cy + x, color);
        put_pixel(cx - y, cy + x, color);
        put_pixel(cx + y, cy - x, color);
        put_pixel(cx - y, cy - x, color);
        ++y;
        if (d < 0)
            d += 2 * y + 1;
        else {
            --x;
            d += 2 * (y - x) + 1;
        }
    }
}

void fill_circle(int cx, int cy, int r, uint8_t color)
{
    /* One horizontal span per scanline in the circle. */
    for (int dy = -r; dy <= r; ++dy) {
        int hw = 0;
        while (hw * hw + dy * dy <= r * r)
            ++hw;
        --hw; /* last x offset inside the disc */
        if (hw >= 0)
            hline(cx - hw, cy + dy, 2 * hw + 1, color);
    }
}

void blit_scaled(const uint8_t* src, int sw, int sh, int dx, int dy, int scale)
{
    if (!src || scale < 1) {
        if (src)
            scale = 1;
        else
            return;
    }
    for (int j = 0; j < sh; ++j) {
        const uint8_t* row = src + j * sw;
        for (int i = 0; i < sw; ++i) {
            int x0 = dx + i * scale;
            int y0 = dy + j * scale;
            int x1 = x0 + scale;
            int y1 = y0 + scale;
            if (x0 < 0 || y0 < 0 || x0 >= WIDTH || y0 >= HEIGHT)
                continue;
            if (x1 > WIDTH)
                x1 = WIDTH;
            if (y1 > HEIGHT)
                y1 = HEIGHT;
            uint8_t c = row[i];
            for (int yy = y0; yy < y1; ++yy) {
                uint8_t* dst = g_backbuf + yy * WIDTH;
                for (int xx = x0; xx < x1; ++xx)
                    dst[xx] = c;
            }
        }
    }
}

void draw_char(int x, int y, char c, uint8_t fg)
{
    const uint8_t* g = g_font[static_cast<uint8_t>(c)];
    for (int r = 0; r < 16; ++r) {
        uint8_t bits = g[r];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col))
                put_pixel(x + col, y + r, fg);
        }
    }
}

void draw_char8(int x, int y, char c, uint8_t fg)
{
    const uint8_t* g = g_font8[static_cast<uint8_t>(c)];
    for (int r = 0; r < 8; ++r) {
        uint8_t bits = g[r];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col))
                put_pixel(x + col, y + r, fg);
        }
    }
}

void draw_string8(int x, int y, const char* s, uint8_t fg)
{
    while (s && *s) {
        draw_char8(x, y, *s, fg);
        x += 8;
        ++s;
    }
}

void draw_string(int x, int y, const char* s, uint8_t fg)
{
    while (s && *s) {
        draw_char(x, y, *s, fg);
        x += 8;
        ++s;
    }
}

int text_width(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n * 8;
}

int text_width8(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n * 8;
}

void draw_cursor(int x, int y, uint8_t color)
{
    /* Small filled arrow, classic pointer shape. */
    static const uint8_t arrow[12] = {
        0b10000000,
        0b11000000,
        0b11100000,
        0b11110000,
        0b11111000,
        0b11111100,
        0b11111110,
        0b11111000,
        0b11011000,
        0b10001100,
        0b00001100,
        0b00000110,
    };
    for (int r = 0; r < 12; ++r) {
        uint8_t bits = arrow[r];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col))
                put_pixel(x + col, y + r, color);
        }
    }
}

void present()
{
    /* Wait for vertical retrace before copying, so the flip doesn't
     * happen while the CRT (real or emulated) is partway through
     * scanning out the previous frame — without this, a screen
     * capture (or a real display, or some emulators' own rendering)
     * can catch the copy mid-flight and show a torn frame: part old,
     * part new, split into vertical bands. Standard technique: poll
     * the VGA input status register (0x3DA) bit 3, which is set only
     * during vertical retrace.
     *
     * Two waits, not one: first wait for retrace to END (in case
     * we're already mid-retrace from a previous call) so the
     * subsequent "wait for it to START" reliably catches the next
     * one, giving the memcpy the whole retrace interval to complete
     * safely off-screen. */
    while (inb(0x3DA) & 0x08) {
    }
    while (!(inb(0x3DA) & 0x08)) {
    }
    memcpy(reinterpret_cast<void*>(VBE_LFB_ADDR), g_backbuf, sizeof(g_backbuf));
}

} // namespace vnu::vgfx
