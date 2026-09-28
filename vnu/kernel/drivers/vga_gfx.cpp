#include <vnu/vga_gfx.h>
#include <vnu/pmm.h>
#include <vnu/tty.h>
#include <vnu/virtio_gpu.h>
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

void vbe_write(uint16_t index, uint16_t val)
{
    outw(VBE_INDEX_PORT, index);
    outw(VBE_DATA_PORT, val);
}

struct VgaState {
    uint8_t misc;
    uint8_t seq[8];
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

    for (int i = 0; i < 8; ++i) {
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
    /* Video enable (the attribute controller's index 0x20 write) goes
     * last, and the status read in front of it matters: the attribute
     * controller shares a single flip-flop between its index and its
     * data writes, so without that read the 0x20 below is taken as a
     * data byte instead and the display never comes back. */
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);
}

void read_registers(VgaState& s)
{
    s.misc = inb(0x3CC);

    for (int i = 0; i < 8; ++i) {
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
 * mode, so graphics-mode text keeps looking like the console.
 *
 * The glyphs are not somewhere the frame buffer cannot reach: a card
 * keeps the console font in plane 2 of the same VRAM the frame buffer
 * is painted into, and the text cells in planes 0/1 right next to them.
 * A graphics mode that fills VRAM therefore takes the font with it, and
 * a text mode whose characters are back but whose glyphs are not renders
 * as stripes of garbage. So the 4 KiB of glyphs have to be saved on the
 * way in and written back on the way out.
 *
 * Reaching them means pointing the memory map at 0xA0000 and selecting
 * plane 2, which is what enter_font_window() does. */
uint8_t g_font[256][16];
uint8_t g_font8[256][8];
bool g_have_font = false;

struct FontWindow {
    uint8_t sr2;
    uint8_t sr4;
    uint8_t gc4;
    uint8_t gc5;
    uint8_t gc6;
};

FontWindow enter_font_window()
{
    FontWindow w;
    outb(0x3C4, 2);
    w.sr2 = inb(0x3C5);
    outb(0x3C4, 4);
    w.sr4 = inb(0x3C5);
    outb(0x3CE, 4);
    w.gc4 = inb(0x3CF);
    outb(0x3CE, 5);
    w.gc5 = inb(0x3CF);
    outb(0x3CE, 6);
    w.gc6 = inb(0x3CF);

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
    return w;
}

void leave_font_window(const FontWindow& w)
{
    outb(0x3C4, 2);
    outb(0x3C5, w.sr2);
    outb(0x3C4, 4);
    outb(0x3C5, w.sr4);
    outb(0x3CE, 4);
    outb(0x3CF, w.gc4);
    outb(0x3CE, 5);
    outb(0x3CF, w.gc5);
    outb(0x3CE, 6);
    outb(0x3CF, w.gc6);
}

void capture_font()
{
    FontWindow w = enter_font_window();

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

    leave_font_window(w);

    g_have_font = true;
}

/* Put the console glyphs back after a graphics mode painted over VRAM.
 * Called once the text register table is in place again: this only
 * borrows the sequencer and graphics controller, and hands them back
 * exactly as it found them. */
void restore_font()
{
    if (!g_have_font)
        return;

    FontWindow w = enter_font_window();
    volatile uint8_t* dst = reinterpret_cast<volatile uint8_t*>(0xA0000);
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 16; ++r)
            dst[c * 32 + r] = g_font[c][r];
    leave_font_window(w);
}

/* Catppuccin Mocha DAC palette for the first 16 palette registers (the
 * ones COLOR_* references). The desktop renders 8-bpp palette indices,
 * so programming the DAC once restyles every surface - wallpaper, window
 * chrome, icons and the gfx apps' own drawing - with no app-code churn.
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

/* The ladder, smallest first: the index is what F12 cycles through and
 * what /proc/gfx lists. DEFAULT_MODE is the entry the desktop starts on
 * (and the one a /etc/vnuconfig/gfx.conf can pick instead). */
const vnu::vgfx::Mode g_modes[vnu::vgfx::MODE_COUNT] = {
    {640, 480},
    {800, 600},
    {1024, 768},
    {1280, 1024},
};

int g_width = vnu::vgfx::DEFAULT_MODE.width;
int g_height = vnu::vgfx::DEFAULT_MODE.height;
bool g_in_gfx_mode = false;

/* The desktop's framebuffer: one mode's worth of palette indices, from
 * the PMM pool rather than .bss, so a 640x480 desktop holds 300 KiB
 * instead of the 1.25 MiB of the top mode and a machine that never
 * reaches the top mode never reserves it. It is identity-mapped, so
 * the pointer is as good a virtual address as it is a physical one.
 *
 * A mode change swaps the pointer; the old run is released only once
 * the new one is in hand, so a display that cannot be shown is a
 * refusal (set_resolution() returns false) and never a half-state. */
uint8_t* g_backbuf = nullptr;

/* Whole frames a w x h mode's framebuffer takes. */
uint32_t backbuf_frames(int w, int h)
{
    const uint32_t px = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
    return (px + vnu::pmm::FRAME_SIZE - 1u) / vnu::pmm::FRAME_SIZE;
}

/* Take a zeroed framebuffer run for w x h, or 0 if the pool is out. */
uint8_t* alloc_backbuf(int w, int h)
{
    const uint32_t phys = vnu::pmm::alloc_contig(backbuf_frames(w, h));
    if (!phys)
        return nullptr;
    /* Identity-mapped by paging::init(), so the physical address is a
     * usable pointer in the kernel and in every task. */
    return reinterpret_cast<uint8_t*>(phys);
}

/* Give the framebuffer run back to the pool. The size is read off the
 * mode it was taken for, which g_width/g_height still are. */
void release_backbuf()
{
    if (!g_backbuf)
        return;
    vnu::pmm::free_contig(reinterpret_cast<uint32_t>(g_backbuf),
                          backbuf_frames(g_width, g_height));
    g_backbuf = nullptr;
}

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

/* Both glyph tables are part of the image whether or not the VGA font
 * was ever captured, so the size is reportable from the start (the
 * kernel accounts for it at boot). */
uint32_t font_bytes()
{
    return static_cast<uint32_t>(sizeof(g_font) + sizeof(g_font8));
}

/* Bochs VBE (the "std" VGA in QEMU): disable first, then select the
 * size/colour depth, then re-enable with the linear-framebuffer bit set
 * so the whole frame is a flat 8-bpp array at 0xFD000000. With LFB set,
 * the bank register (index 5) is ignored.
 *
 * Only the VBE registers change here: a mode switch reprograms the DAC,
 * which is the caller's business (both callers reload CATT_PAL right
 * after), and the backbuffer is left alone on purpose. */
void program_mode(int w, int h)
{
    vbe_write(VBE_INDEX_X_OFFSET, 0);
    vbe_write(VBE_INDEX_Y_OFFSET, 0);
    vbe_write(VBE_INDEX_ENABLE, VBE_DISABLED);
    vbe_write(VBE_INDEX_XRES, static_cast<uint16_t>(w));
    vbe_write(VBE_INDEX_YRES, static_cast<uint16_t>(h));
    vbe_write(VBE_INDEX_VIRT_WIDTH, static_cast<uint16_t>(w));
    vbe_write(VBE_INDEX_BPP, 8);
    vbe_write(VBE_INDEX_ENABLE, VBE_ENABLED | VBE_LFB | VBE_8BIT_DAC);
}

const Mode* modes()
{
    return g_modes;
}

uint32_t bpp()
{
    /* 8: the backbuffer the desktop composites into is one byte per
     * pixel on both paths. The VBE one reaches the card as is; the
     * virtio one is expanded through the DAC into a 32bpp scanout by
     * present(), which is a display detail and not the format a program
     * draws in. */
    return 8;
}

uint32_t driver()
{
    return virtio_gpu::active() ? DRIVER_VIRTIO_GPU : DRIVER_VGA;
}

int width()
{
    return g_width;
}

int height()
{
    return g_height;
}

bool mode_supported(int w, int h)
{
    for (int i = 0; i < MODE_COUNT; ++i)
        if (g_modes[i].width == w && g_modes[i].height == h)
            return true;
    return false;
}

bool enter_gfx_mode()
{
    /* Before the mode is programmed: from here on the framebuffer covers
     * the console's own characters. */
    vnu::tty::save_screen();
    if (!g_have_font)
        capture_font();
    if (!g_have_saved) {
        read_registers(g_saved);
        g_have_saved = true;
    }

    /* The framebuffer is taken before the card is programmed and reused
     * if set_resolution() already took one for this mode (the seeded
     * /etc/vnuconfig/gfx.conf asks for it from text mode, where the
     * pool is much emptier). A pool with nothing left is the one reason
     * the desktop cannot start, and it is better answered by leaving
     * the console exactly as it was than by entering a mode with
     * nowhere to draw. */
    if (!g_backbuf) {
        g_backbuf = alloc_backbuf(g_width, g_height);
        if (!g_backbuf)
            return false;
    }

    program_mode(g_width, g_height);
    g_in_gfx_mode = true;

    /* Swap in the Catppuccin DAC the first time we enter graphics mode;
     * the boot palette is saved so exit_to_text() can hand it back. */
    if (!g_have_saved_pal)
        save_palette();
    load_palette(CATT_PAL);
    return true;
}

bool set_resolution(int w, int h)
{
    if (!mode_supported(w, h))
        return false;
    if (w == g_width && h == g_height)
        return true;

    /* A mode change is a reallocation now, and it is the one part that
     * can run out of memory, so the new framebuffer is taken first: a
     * mode that cannot be shown leaves the display, the virtio scanout
     * and the old framebuffer exactly as they were. */
    uint8_t* next = alloc_backbuf(w, h);
    if (!next)
        return false;

    /* The virtio-gpu path owns a scanout resource of its own, which is
     * asked second for the same reason: the two drivers must never
     * disagree about the screen. */
    if (virtio_gpu::active() && !virtio_gpu::set_resolution(w, h)) {
        vnu::pmm::free_contig(reinterpret_cast<uint32_t>(next),
                              backbuf_frames(w, h));
        return false;
    }

    if (g_in_gfx_mode) {
        program_mode(w, h);
        /* The DAC is reprogrammed by the mode switch, so the desktop
         * palette has to go in again. */
        load_palette(CATT_PAL);
    }
    release_backbuf(); /* still sized for the mode being left */
    g_backbuf = next;
    g_width = w;
    g_height = h;
    return true;
}

void exit_to_text()
{
    /* The framebuffer goes back to the pool before the text mode is
     * restored, so a machine that boots to the console holds none of
     * it: the next desktop session takes a fresh run for whatever mode
     * is configured by then. */
    release_backbuf();
    if (!g_have_saved)
        return;
    g_in_gfx_mode = false;
    if (g_have_saved_pal)
        load_palette(g_saved_pal);
    vbe_write(VBE_INDEX_ENABLE, VBE_DISABLED);
    write_registers(g_saved.misc, g_saved.seq, g_saved.crtc, g_saved.gc, g_saved.ac);
    restore_font();
    vnu::tty::restore_screen();
    /* The virtio-gpu display has no text mode to fall back to and the
     * host keeps showing whatever was last presented, so a desktop that
     * quit would stay on the monitor: it gets the console's own blank
     * instead, the way the VGA side gets its text mode back. */
    if (vnu::virtio_gpu::active())
        vnu::virtio_gpu::blank();
}

void clear(uint8_t color)
{
    for (int i = 0; i < g_width * g_height; ++i)
        g_backbuf[i] = color;
}

/* The procedural wallpaper is authored in 1024x768 coordinates, so the
 * drawing calls below read as the pixels they were designed as and these
 * three helpers carry them into whichever mode is programmed now: px and
 * py for a position, pr for a radius (scaled like py, which is what the
 * vertical proportions of the scene - sun, clouds, hill heights - are
 * written against). */
constexpr int ART_W = 1024;
constexpr int ART_H = 768;

int px(int x)
{
    return x * g_width / ART_W;
}

int py(int y)
{
    return y * g_height / ART_H;
}

int pr(int r)
{
    return r * g_height / ART_H;
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
               static_cast<unsigned long>(g_width * g_height));
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
    for (int y = 0; y < g_height; ++y) {
        unsigned t = (unsigned)y * 256u / (unsigned)g_height;
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
        uint8_t* row = g_backbuf + y * g_width;
        for (int x = 0; x < g_width; ++x)
            row[x] = (u >= (unsigned)(bayer[x & 3] * 17)) ? bottom : top;
    }

    /* Sun: peach halo around a warm yellow core (Mocha dusk). */
    fill_circle(px(848), py(150), pr(58), COLOR_BROWN);
    fill_circle(px(848), py(150), pr(40), COLOR_YELLOW);

    /* Clouds: puffy white blobs. */
    fill_circle(px(180), py(140), pr(26), COLOR_WHITE);
    fill_circle(px(206), py(150), pr(26), COLOR_WHITE);
    fill_circle(px(148), py(152), pr(22), COLOR_WHITE);
    fill_circle(px(560), py(120), pr(20), COLOR_WHITE);
    fill_circle(px(582), py(128), pr(20), COLOR_WHITE);
    fill_circle(px(540), py(130), pr(16), COLOR_WHITE);

    /* Hills: per-column sine-profile silhouettes (far range first, then
     * a nearer, greener range, then a ground strip). */
    const int far_base = py(700);
    const int far_h = py(150);
    const int near_base = py(748);
    const int near_h = py(110);
    for (int x = 0; x < g_width; ++x) {
        int idx = (x * 3 * 64 / g_width) % 64;
        int h = far_h * static_cast<int>(HILL_T[idx]) / 256;
        for (int y = far_base - h; y < far_base; ++y)
            g_backbuf[y * g_width + x] = COLOR_DGRAY;
    }
    for (int x = 0; x < g_width; ++x) {
        int idx = (x * 2 * 64 / g_width + 16) % 64;
        int h = near_h * static_cast<int>(HILL_T[idx]) / 256;
        for (int y = near_base - h; y < near_base; ++y)
            g_backbuf[y * g_width + x] = COLOR_GREEN;
    }
    for (int y = near_base; y < g_height; ++y) {
        uint8_t* row = g_backbuf + y * g_width;
        for (int x = 0; x < g_width; ++x)
            row[x] = COLOR_GREEN;
    }
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
    for (int y = 0; y < g_height; ++y) {
        unsigned t = (unsigned)y * 256u / (unsigned)g_height;
        const uint8_t* bayer = B4[y & 3];
        uint8_t* row = g_backbuf + y * g_width;
        for (int x = 0; x < g_width; ++x)
            row[x] = (t >= (unsigned)(bayer[x & 3] * 17)) ? bottom : top;
    }
}

void put_pixel(int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || x >= g_width || y >= g_height)
        return;
    g_backbuf[y * g_width + x] = color;
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

void blit_scale(const uint8_t* src, int sw, int sh, int dx, int dy, int dw, int dh)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    for (int j = 0; j < dh; ++j) {
        int y = dy + j;
        if (y < 0 || y >= g_height)
            continue;
        int sy = (j * sh) / dh;
        if (sy < 0)
            sy = 0;
        if (sy >= sh)
            sy = sh - 1;
        const uint8_t* row = src + sy * sw;
        for (int i = 0; i < dw; ++i) {
            int x = dx + i;
            if (x < 0 || x >= g_width)
                continue;
            int sx = (i * sw) / dw;
            if (sx < 0)
                sx = 0;
            if (sx >= sw)
                sx = sw - 1;
            put_pixel(x, y, row[sx]);
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

namespace {

/* Rasterisers for the move/resize cursors. Each cursor is a 16x16 icon
 * with its two centre rows/columns (7 and 8) as the axis of symmetry,
 * so the hotspots sit in the middle of the glyph. */

/* Filled arrowhead pointing along a horizontal axis. `tip` is the col
 * where the point sits, `dir` (+1 / -1) which way the head opens,
 * `len` its length in columns and `w` its max half-height. */
void cone_h(int x, int y, int tip, int dir, int len, int w, uint8_t c)
{
    for (int d = 0; d <= len; ++d) {
        int px = tip + dir * d;
        int hh = d < w ? d : w;
        for (int r = 7 - hh; r <= 8 + hh; ++r)
            put_pixel(x + px, y + r, c);
    }
}

/* Same, pointing along a vertical axis (row `tip`, centre columns 7-8). */
void cone_v(int x, int y, int tip, int dir, int len, int w, uint8_t c)
{
    for (int d = 0; d <= len; ++d) {
        int py = tip + dir * d;
        int hh = d < w ? d : w;
        for (int cc = 7 - hh; cc <= 8 + hh; ++cc)
            put_pixel(x + cc, y + py, c);
    }
}

void blit_cursor_shape(int x, int y, CursorShape shape, uint8_t c)
{
    switch (shape) {
    case CursorShape::SizeH:
        cone_h(x, y, 0, 1, 4, 3, c);
        cone_h(x, y, 15, -1, 4, 3, c);
        fill_rect(x + 5, y + 7, 6, 2, c);
        break;
    case CursorShape::SizeV:
        cone_v(x, y, 0, 1, 4, 3, c);
        cone_v(x, y, 15, -1, 4, 3, c);
        fill_rect(x + 7, y + 5, 2, 6, c);
        break;
    case CursorShape::Move:
        cone_v(x, y, 0, 1, 3, 3, c);
        cone_v(x, y, 15, -1, 3, 3, c);
        cone_h(x, y, 0, 1, 3, 3, c);
        cone_h(x, y, 15, -1, 3, 3, c);
        fill_rect(x + 7, y + 7, 2, 2, c);
        break;
    case CursorShape::SizeDiagL:
        /* Double arrow along the top-left-to-bottom-right diagonal. */
        for (int t = 0; t <= 5; ++t) {
            int hh = t < 3 ? t : 3;
            for (int u = -hh; u <= hh; ++u) {
                put_pixel(x + t + u, y + t - u, c);            /* NW head */
                put_pixel(x + 15 - t - u, y + 15 - t + u, c);  /* SE head */
            }
        }
        for (int d = 6; d <= 9; ++d) {
            put_pixel(x + d, y + d, c);
            put_pixel(x + d, y + d + 1, c);
        }
        break;
    case CursorShape::SizeDiagR:
        /* Double arrow along the top-right-to-bottom-left diagonal. */
        for (int t = 0; t <= 5; ++t) {
            int hh = t < 3 ? t : 3;
            for (int u = -hh; u <= hh; ++u) {
                put_pixel(x + 15 - (t + u), y + t - u, c);     /* NE head */
                put_pixel(x + t + u, y + 15 - t + u, c);       /* SW head */
            }
        }
        for (int d = 6; d <= 9; ++d) {
            put_pixel(x + 15 - d, y + d, c);
            put_pixel(x + 15 - d, y + d + 1, c);
        }
        break;
    default:
        break;
    }
}

} // namespace

void draw_cursor_at(int x, int y, CursorShape shape)
{
    if (shape == CursorShape::Arrow)
        return;
    static const int off[8][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0},   {-1, 1}, {0, 1},  {1, 1},
    };
    for (int i = 0; i < 8; ++i)
        blit_cursor_shape(x + off[i][0], y + off[i][1], shape, COLOR_BLACK);
    blit_cursor_shape(x, y, shape, COLOR_WHITE);
}

void present()
{
    /* Virtio-gpu path (when QEMU was given a virtio display device):
     * the host scanout is a 32-bpp resource backed by guest memory, so
     * expand the 8-bpp backbuffer through the DAC palette first, then
     * hand the true-colour buffer over to the driver, which transfers
     * and flushes it. No VBE frame involved. */
    if (vnu::virtio_gpu::active()) {
        const uint32_t nsegs = vnu::virtio_gpu::scanout_segments();
        /* A mode change can leave the driver a moment without a scanout
         * resource (it frees the old frames before it takes the new
         * ones, see virtio_gpu::set_resolution). The desktop keeps
         * drawing into the backbuffer either way; there is simply
         * nothing to expand into until the next attempt gets frames. */
        if (nsegs == 0)
            return;

        /* The scanout is a list of frame runs, so the image is written
         * run by run: a row is 4 bytes per pixel wide and can straddle
         * two of them, while a run holds many rows. Splitting per row
         * keeps the pixel loop below free of any segment bookkeeping. */
        uint32_t seg = 0;
        vnu::virtio_gpu::ScanoutSegment s = vnu::virtio_gpu::scanout_segment(0);
        uint8_t* dst = reinterpret_cast<uint8_t*>(s.phys);
        uint32_t room = s.bytes;
        const uint32_t row_bytes = static_cast<uint32_t>(g_width) * 4u;
        for (int y = 0; y < g_height; ++y) {
            const uint8_t* src = g_backbuf + static_cast<unsigned long>(y) * g_width;
            uint32_t left = row_bytes;
            while (left > 0) {
                if (room == 0) {
                    if (++seg == nsegs)
                        return;   /* the list ran out: leave the rest alone */
                    s = vnu::virtio_gpu::scanout_segment(seg);
                    dst = reinterpret_cast<uint8_t*>(s.phys);
                    room = s.bytes;
                }
                const uint32_t px = (left < room) ? (left / 4u) : (room / 4u);
                for (uint32_t x = 0; x < px; ++x) {
                    const uint8_t* rgb = CATT_PAL[*src++];
                    /* 6-bit DAC -> 8-bit channel, then store B,G,R,X
                     * (pixel = 0x00RRGGBB) to match B8G8R8X8. */
                    dst[0] = static_cast<uint8_t>((rgb[2] << 2) | (rgb[2] >> 4));
                    dst[1] = static_cast<uint8_t>((rgb[1] << 2) | (rgb[1] >> 4));
                    dst[2] = static_cast<uint8_t>((rgb[0] << 2) | (rgb[0] >> 4));
                    dst[3] = 0xFF;
                    dst += 4;
                }
                room -= px * 4u;
                left -= px * 4u;
            }
        }
        vnu::virtio_gpu::present();
        return;
    }

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
    memcpy(reinterpret_cast<void*>(VBE_LFB_ADDR), g_backbuf,
           static_cast<unsigned long>(g_width * g_height));
}

} // namespace vnu::vgfx
