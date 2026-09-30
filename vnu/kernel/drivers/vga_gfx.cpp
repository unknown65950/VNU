#include <vnu/vga_gfx.h>
#include <vnu/abi.h>   /* VNU_GFX_PALETTE_*: the shape of the answer below */
#include <vnu/font8x16.h>
#include <vnu/pmm.h>
#include <vnu/process.h>   /* now_jiffies(): the console refresh's rate limit */
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

    volatile const uint8_t* src = reinterpret_cast<volatile const uint8_t*>(0xA0000);
    bool captured_anything = false;
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 16; ++r) {
            g_font[c][r] = src[c * 32 + r];
            captured_anything = captured_anything || g_font[c][r] != 0;
        }

    /* Derive the compact 8x8 font by OR-pairing adjacent rows of the
     * captured 8x16 glyphs, so the desktop's small text keeps the
     * exact same letterforms (just denser) instead of a second,
     * discontiguous typeface. */
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 8; ++r)
            g_font8[c][r] = static_cast<uint8_t>(g_font[c][r * 2] |
                                                 g_font[c][r * 2 + 1]);

    leave_font_window(w);

    /* A display with no font memory behind it - a virtio-gpu has no
     * 8K of glyph ROM and its font aperture reads back empty - hands
     * back nothing at all, and text drawn from an empty table is a
     * screen full of blank cells. Such a card is given the font it
     * would have had, so the text mode and the text a gfx program
     * draws are the same letterforms either way. */
    if (!captured_anything) {
        for (int c = 0; c < 256; ++c)
            for (int r = 0; r < 16; ++r)
                g_font[c][r] = vnu::font8x16[c][r];
    }

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

/* The desktop's framebuffer, from the PMM pool rather than .bss, so a
 * 640x480 desktop holds 300 KiB instead of the 1.25 MiB of the top mode
 * and a machine that never reaches the top mode never reserves it. It
 * is identity-mapped, so the pointer is as good a virtual address as it
 * is a physical one.
 *
 * A mode change swaps the pointer; the old run is released only once
 * the new one is in hand, so a display that cannot be shown is a
 * refusal (set_resolution() returns false) and never a half-state. */
uint8_t* g_backbuf = nullptr;

/* Bits per pixel the framebuffer is held at, or 0 while no desktop has
 * asked for one yet.
 *
 * The depth follows the display rather than being a mode of its own,
 * because it is the display that decides what a frame costs: the Bochs
 * VBE path this driver programs is 8-bpp with a DAC in front of it, and
 * the virtio-gpu scanout is 32-bpp B8G8R8X8 with no DAC at all. A mode
 * is a resolution on this OS (gfx_setmode takes a width and a height,
 * see vnu/abi/ABI.md), and a resolution the hardware cannot show at its
 * own depth is a mode this driver does not have.
 *
 * So where a virtio-gpu holds the display the desktop composites
 * straight into the scanout's own format - four bytes a pixel, with the
 * fourth left opaque for now and free to carry a window's alpha - and
 * where the VBE path holds it, the framebuffer stays one byte per pixel
 * and present() expands through the DAC on the way to the card, as it
 * always did. The cost is named, because it is real: a 32bpp frame is
 * four times the memory (3 MiB at 1024x768, 5 MiB at 1280x1024) and
 * four times the bytes across a present, which is what the damage
 * tracking in the roadmap is for.
 *
 * Resolved once, when the first framebuffer is taken, and never changed:
 * the display cannot be swapped out from under a frame that is being
 * drawn into it, and a framebuffer whose size depended on a value that
 * could change would be a buffer the pool was never told about. */
int g_bpp = 0;

/* When the console was last put on a display that cannot show the text
 * plane itself, in 100 Hz jiffies. See console_tick() at the bottom:
 * the point is to collapse a burst of output into one redraw, and to do
 * nothing at all while the console is idle. Wraps with the counter, and
 * the compare is signed, so a wrap is not a 49-day stall. */
uint32_t g_console_tick = 0;
constexpr int CONSOLE_TICK_JIFFIES = 6;   /* 60 ms */

/* Every text row, as vnu::tty::console_dirty_rows() spells it. */
constexpr uint32_t ALL_CONSOLE_ROWS = 0xFFFFFFFFu;

/* True once the host has been pointed at a scanout resource of ours.
 * On a virtio display that is the moment the console stops being
 * something the hardware shows all by itself: until then the monitor
 * is on the legacy VGA text plane and every character lands in VRAM
 * with the hardware putting it on screen, and a console redraw would be
 * a megabyte of pixels and a transfer to a host that is not looking.
 * Nothing ever points the host back at the text plane - a desktop that
 * exits leaves the scanout up, which is the whole reason exit_to_text()
 * has to draw the console as pixels - so this stays true for the rest of
 * the session. A display with a text mode of its own never sets it and
 * never needs it. See console_tick(). */
bool g_scanout_shown = false;

/* True when g_backbuf is a frame nothing has drawn the console into
 * yet: freshly allocated, or left holding the desktop's last picture.
 * Set wherever a new backbuffer is taken, so the first console redraw
 * after it blacks the frame and repaints every row instead of trusting
 * whatever the pool handed over. Cleared by the redraw that does that
 * work, and only by it, so a frame that could not be taken stays to be
 * tried again. */
bool g_console_full = true;

/* The rectangle every write into the frame is confined to, as a
 * half-open box; the whole screen unless a caller has narrowed it with
 * clip_set(). Checked by put_pixel() and by the wallpaper's own row
 * loops, so a clipped repaint costs the region and not the screen.
 * Reset with the geometry, since a box of one mode's screen says nothing
 * about another's. */
int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 0, g_clip_y1 = 0;

/* Whether the clip box excludes a pixel. An empty box has not been
 * narrowed - clip_pop() is what narrows it back to the screen, and until
 * it has been called once there is nothing to clip to - so it excludes
 * nothing, which is also why put_pixel() and the wallpaper's loop bounds
 * both go through here. */
bool clipped_out(int x, int y)
{
    if (g_clip_x1 <= g_clip_x0 || g_clip_y1 <= g_clip_y0)
        return false;
    return x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1;
}

/* What has been written since the last present() went to the host, as a
 * half-open box (x0,y0)-(x1,y1) with x0 >= x1 meaning nothing pending.
 * The host's copy of the frame is the reference this is measured
 * against, so anything that writes a pixel has to widen it - which is
 * what damage_add() below is for, and why the two calls that present
 * without a compositor (present_text and a mode change) damage
 * everything. */
int g_dmg_x0 = 0, g_dmg_y0 = 0, g_dmg_x1 = 0, g_dmg_y1 = 0;

/* The box the last transfer moved, and the bytes it moved, kept for
 * /proc/gfx: the claim damage tracking makes is about how much crosses
 * to the host, so the amount that crossed has to be visible. */
int g_present_x = 0, g_present_y = 0, g_present_w = 0, g_present_h = 0;
uint32_t g_present_bytes = 0;


/* Point the host at our scanout, and remember that from now on the
 * console has to be drawn for it. The one place that does this, so the
 * bookkeeping cannot be left out of one of the call sites. */
void arm_scanout()
{
    if (!vnu::virtio_gpu::active())
        return;
    vnu::virtio_gpu::show_scanout();
    g_scanout_shown = true;
}

int resolve_bpp()
{
    if (g_bpp == 0)
        g_bpp = vnu::virtio_gpu::active() ? 32 : 8;
    return g_bpp;
}

inline int pixel_bytes()
{
    return g_bpp / 8;
}

/* Whole frames a w x h frame of `bpp` bits per pixel takes. */
uint32_t backbuf_frames(int w, int h, int bpp)
{
    const uint32_t px = static_cast<uint32_t>(w) *
                        static_cast<uint32_t>(h) *
                        static_cast<uint32_t>(bpp / 8);
    return (px + vnu::pmm::FRAME_SIZE - 1u) / vnu::pmm::FRAME_SIZE;
}

/* Take a zeroed framebuffer run for w x h at bpp, or 0 if the pool is
 * out. */
uint8_t* alloc_backbuf(int w, int h, int bpp)
{
    const uint32_t phys = vnu::pmm::alloc_contig(backbuf_frames(w, h, bpp));
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
                          backbuf_frames(g_width, g_height, g_bpp));
    g_backbuf = nullptr;
}

/* The start of a row, and one pixel along it. Everything that writes a
 * pixel goes through these, so the framebuffer's format is decided in
 * one place instead of at every call site. */
inline uint8_t* row_ptr(int y)
{
    return g_backbuf + static_cast<long>(y) * g_width * pixel_bytes();
}

inline uint8_t* pixel_ptr(int x, int y)
{
    return row_ptr(y) + x * pixel_bytes();
}

/* A colour as the bytes of one pixel in the current format: the palette
 * index itself at 8bpp, and the same index looked up in the desktop
 * palette at 32bpp, stored B,G,R,X the way the virtio-gpu scanout is.
 * `pal` is the palette to read, so the console can present in the one
 * the text mode booted with. */
uint32_t pixel_of(uint8_t color, const uint8_t (&pal)[16][3], uint8_t out[4])
{
    if (g_bpp == 8) {
        out[0] = color;
        return 1;
    }
    const uint8_t* rgb = pal[color];
    /* 6-bit DAC -> 8-bit channel, as present() has always done it on
     * the way to the scanout. */
    out[0] = static_cast<uint8_t>((rgb[2] << 2) | (rgb[2] >> 4));
    out[1] = static_cast<uint8_t>((rgb[1] << 2) | (rgb[1] >> 4));
    out[2] = static_cast<uint8_t>((rgb[0] << 2) | (rgb[0] >> 4));
    out[3] = 0xFF;   /* opaque: nothing has blended here yet */
    return 4;
}

inline void store_with(uint8_t* p, uint8_t color, const uint8_t (&pal)[16][3])
{
    uint8_t px[4];
    const uint32_t n = pixel_of(color, pal, px);
    for (uint32_t i = 0; i < n; ++i)
        p[i] = px[i];
}

/* The desktop's own colours, which is what every caller but the console
 * wants: at 8bpp a pixel is a palette index and the palette only
 * matters at present time, but at 32bpp the colour is decided as it is
 * drawn, so the caller has to say which palette it is drawing in. */
inline void store(uint8_t* p, uint8_t color)
{
    store_with(p, color, CATT_PAL);
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
/* The 16 palette slots as colours: 0xXXRRGGBB words, the same layout a
 * 32bpp pixel is in, so a program that knows the depth can put a slot
 * straight into one. A program on the 8bpp path is handed the same table
 * and has no use for the 32 bits it can see there, which is the point:
 * the roles (VGFX_LGRAY, and so on) mean the same thing on both paths
 * without either side hardcoding Catppuccin.
 *
 * It is the *desktop's* palette even before the desktop has started,
 * because what is being asked is what a slot of a canvas means, not
 * what the DAC is wired to at this instant: the moment anything is
 * composited the desktop has loaded CATT_PAL, and a program that drew
 * with any other table would be wrong from its first pixel. The DAC in
 * text mode is the console's own business. */
uint32_t palette(uint32_t* out, uint32_t count)
{
    if (!out || count < VNU_GFX_PALETTE_SLOTS)
        return 0;
    for (int i = 0; i < VNU_GFX_PALETTE_SLOTS; ++i) {
        /* 6-bit DAC -> 8-bit channel, the same expansion pixel_of()
         * makes on the way to the frame. */
        const uint8_t* rgb = CATT_PAL[i];
        const uint32_t b = (static_cast<uint32_t>(rgb[2] << 2) | (rgb[2] >> 4)) << 0;
        const uint32_t g = (static_cast<uint32_t>(rgb[1] << 2) | (rgb[1] >> 4)) << 8;
        const uint32_t r = (static_cast<uint32_t>(rgb[0] << 2) | (rgb[0] >> 4)) << 16;
        out[i] = 0xFF000000u | r | g | b;
    }
    return VNU_GFX_PALETTE_SLOTS;
}

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
    /* Resolving is a read of one driver flag, so this is asked rather
     * than assumed: a program that asks while the desktop is not running
     * is told what the display will be, which on a machine with no
     * virtio-gpu is 8 for good. A window's canvas is mapped in this
     * depth, so an answer that had to wait for the first frame would be
     * too late for the one program that most needs it. */
    (void)resolve_bpp();
    return g_bpp;
}

uint32_t driver()
{
    return virtio_gpu::active() ? DRIVER_VIRTIO_GPU : DRIVER_VGA;
}

/* What is on the screen, which is not always a mode this driver
 * programmed: with the desktop not running the display is in the text
 * mode, whose geometry is the console's own grid of 9x16 cells. The
 * mode the *next* session will come up in is the config file's, and it
 * is not what /proc/gfx or gfx_getinfo(2) answer here - those are asked
 * what is on the screen now, and after a desktop has given it up the
 * answer is 720x400 whether the card restored its own text mode or the
 * console was drawn into a graphics framebuffer to stand in for one. */
static void display_mode(int* w, int* h)
{
    if (g_in_gfx_mode) {
        *w = g_width;
        *h = g_height;
        return;
    }
    uint16_t rows = 0, cols = 0;
    vnu::tty::get_size(&rows, &cols);
    *w = cols * 9;
    *h = rows * 16;
}

int width()
{
    int w = 0, h = 0;
    display_mode(&w, &h);
    return w;
}

int height()
{
    int w = 0, h = 0;
    display_mode(&w, &h);
    return h;
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
        resolve_bpp();
        g_backbuf = alloc_backbuf(g_width, g_height, g_bpp);
        if (!g_backbuf)
            return false;
        g_console_full = true;
    }

    program_mode(g_width, g_height);
    g_in_gfx_mode = true;
    clip_pop();
    /* What is on the display now is whatever the mode programming left
     * there, so the host's copy of it is not this framebuffer's and
     * every pixel of it has to be sent. Reused frame or fresh one, this
     * is the one place a mode change makes the screen unknown. */
    damage_all();

    /* On a virtio-vga the host shows whichever plane it was pointed at
     * last, and a text-mode console is the legacy VGA one. The desktop
     * is what changes that, so the scanout is claimed here - after the
     * mode is programmed, never before, so a failed mode change leaves
     * the console on the screen where it already was. */
    arm_scanout();

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
    resolve_bpp();
    uint8_t* next = alloc_backbuf(w, h, g_bpp);
    if (!next)
        return false;

    /* The virtio-gpu path owns a scanout resource of its own, which is
     * asked second for the same reason: the two drivers must never
     * disagree about the screen. */
    if (virtio_gpu::active() && !virtio_gpu::set_resolution(w, h)) {
        vnu::pmm::free_contig(reinterpret_cast<uint32_t>(next),
                              backbuf_frames(w, h, g_bpp));
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
    g_console_full = true;
    /* The new geometry first, then the two boxes that describe the new
     * frame in it: damage_all() and clip_pop() both read the current
     * width and height, and a damage box or a clip left at the old
     * screen's size is not merely stale - the box is what a transfer
     * copies, so one that is too large sends the copy off the end of the
     * frame it belongs to. */
    g_width = w;
    g_height = h;
    damage_all();   /* a new frame: the host has none of it */
    clip_pop();     /* a box of the old screen is not a clip for the new one */
    /* A new resource is a new thing to be shown: the host is still
     * pointed at the one the mode being left had. This is where the new
     * size is already on record, which is what the host is told along
     * with the resource. Only inside a session - a mode applied from
     * /etc/vnuconfig/gfx.conf at boot changes the mode the desktop will
     * come up in, and the console keeps the display until something
     * actually draws. */
    if (g_in_gfx_mode)
        arm_scanout();
    return true;
}

/* Put the display back in the console's own mode: the text mode is
 * 9x16 per cell plus the blank column VGA puts between them, so an
 * 80x25 grid is 720x400. Same three steps as set_resolution() and for
 * the same reasons - the new frame is taken before the old one is let
 * go, the driver is asked second, and a machine that cannot do it keeps
 * the mode it had rather than ending up with neither. Returns whether
 * the display is in the console's geometry now. */
static bool restore_console_geometry(uint16_t cols, uint16_t rows)
{
    const int w = cols * 9;
    const int h = rows * 16;
    /* The right size *and* a frame to put it in: a machine sitting at
     * the console has none, the desktop's having gone back to the pool,
     * and the depth is resolved by the first frame this takes. */
    if (g_width == w && g_height == h && g_backbuf)
        return true;

    resolve_bpp();
    uint8_t* next = alloc_backbuf(w, h, g_bpp);
    if (!next)
        return false;
    if (vnu::virtio_gpu::active() && !vnu::virtio_gpu::set_resolution(w, h)) {
        vnu::pmm::free_contig(reinterpret_cast<uint32_t>(next),
                              backbuf_frames(w, h, g_bpp));
        return false;
    }
    release_backbuf();
    g_backbuf = next;
    g_console_full = true;
    /* The new geometry first, then the two boxes that describe the new
     * frame in it: damage_all() and clip_pop() both read the current
     * width and height, and a damage box or a clip left at the old
     * screen's size is not merely stale - the box is what a transfer
     * copies, so one that is too large sends the copy off the end of the
     * frame it belongs to. */
    g_width = w;
    g_height = h;
    damage_all();   /* a new frame: the host has none of it */
    clip_pop();     /* a box of the old screen is not a clip for the new one */
    /* A new resource is a new thing to show: the host is still pointed
     * at the one the mode being left had. */
    if (g_in_gfx_mode)
        arm_scanout();
    return true;
}

void exit_to_text()
{
    /* The console goes on the screen while the framebuffer is still
     * ours to draw it in, and before the mode is given up: the text
     * mode below only reaches the monitor on a display that has one. */
    if (vnu::virtio_gpu::active())
        present_text();

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
}

void clear(uint8_t color)
{
    if (!g_backbuf)
        return;
    /* One pixel built once and copied down the rows: at 32bpp that is
     * four bytes a pixel, and a screen of them is the one loop that
     * would be worth not doing per pixel. */
    uint8_t px[4];
    const uint32_t n = pixel_of(color, CATT_PAL, px);
    for (int y = 0; y < g_height; ++y) {
        uint8_t* row = row_ptr(y);
        for (int x = 0; x < g_width; ++x)
            for (uint32_t i = 0; i < n; ++i)
                row[x * n + i] = px[i];
    }
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
    /* The clip as loop bounds, so the four row loops below stay inside a
     * region repaint. A full-screen call sees the whole screen, which is
     * what an unset clip means (see clipped_out). */
    const int cx0 = g_clip_x1 > g_clip_x0 ? g_clip_x0 : 0;
    const int cx1 = g_clip_x1 > g_clip_x0 ? g_clip_x1 : g_width;
    const int cy0 = g_clip_y1 > g_clip_y0 ? g_clip_y0 : 0;
    const int cy1 = g_clip_y1 > g_clip_y0 ? g_clip_y1 : g_height;

    if (vnu::wallpaper::ready()) {
        /* A decoded wallpaper is held as one byte per pixel (it is
         * quantized to a palette at startup), so a 32bpp frame is fed
         * from it the way a scanout is: index by index through the same
         * palette the rest of the desktop draws in. */
        const uint8_t* src = vnu::wallpaper::frame();
        for (int y = cy0; y < cy1; ++y) {
            const uint8_t* in = src + static_cast<long>(y) * g_width;
            uint8_t* row = row_ptr(y);
            for (int x = cx0; x < cx1; ++x) {
                uint8_t px[4];
                const uint32_t n = pixel_of(in[x], CATT_PAL, px);
                for (uint32_t i = 0; i < n; ++i)
                    row[x * n + i] = px[i];
            }
        }
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
    for (int y = cy0; y < cy1; ++y) {
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
        uint8_t* row = row_ptr(y);
        for (int x = cx0; x < cx1; ++x)
            store(row + x * pixel_bytes(),
                  (u >= (unsigned)(bayer[x & 3] * 17)) ? bottom : top);
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
    /* The two ranges write through pixel_ptr rather than put_pixel, so
     * the clip has to bound their rows here as well as their columns. */
    for (int x = cx0; x < cx1; ++x) {
        int idx = (x * 3 * 64 / g_width) % 64;
        int h = far_h * static_cast<int>(HILL_T[idx]) / 256;
        int ytop = far_base - h < cy0 ? cy0 : far_base - h;
        for (int y = ytop; y < far_base && y < cy1; ++y)
            store(pixel_ptr(x, y), COLOR_DGRAY);
    }
    for (int x = cx0; x < cx1; ++x) {
        int idx = (x * 2 * 64 / g_width + 16) % 64;
        int h = near_h * static_cast<int>(HILL_T[idx]) / 256;
        int ytop = near_base - h < cy0 ? cy0 : near_base - h;
        for (int y = ytop; y < near_base && y < cy1; ++y)
            store(pixel_ptr(x, y), COLOR_GREEN);
    }
    for (int y = near_base < cy0 ? cy0 : near_base; y < cy1; ++y) {
        uint8_t* row = row_ptr(y);
        for (int x = cx0; x < cx1; ++x)
            store(row + x * pixel_bytes(), COLOR_GREEN);
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
        uint8_t* row = row_ptr(y);
        for (int x = 0; x < g_width; ++x)
            store(row + x * pixel_bytes(),
                  (t >= (unsigned)(bayer[x & 3] * 17)) ? bottom : top);
    }
}

void put_pixel(int x, int y, uint8_t color)
{
    /* The clip is the screen unless a caller narrowed it, so this one
     * test is both the bounds check and the region repaint. */
    if (clipped_out(x, y))
        return;
    store(pixel_ptr(x, y), color);
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

void blit_scale(const uint8_t* src, int sw, int sh, int dx, int dy, int dw,
                int dh, int src_bpp)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    /* A 32bpp source is a window's own canvas in the display's format,
     * and it can carry alpha in the byte the scanout has no use for: a
     * pixel whose top byte is not opaque is blended into what it covers
     * instead of replacing it. That is the whole reason the canvas
     * follows the display - a PNG with an alpha channel survives the
     * trip from px_decode to the screen instead of being quantized to
     * the nearest of sixteen palette entries on the way. */
    if (src_bpp == 32 && g_bpp == 32) {
        const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
        for (int j = 0; j < dh; ++j) {
            const int y = dy + j;
            if (y < 0 || y >= g_height)
                continue;
            int sy = (j * sh) / dh;
            if (sy < 0)
                sy = 0;
            if (sy >= sh)
                sy = sh - 1;
            const uint32_t* srow = s32 + static_cast<long>(sy) * sw;
            for (int i = 0; i < dw; ++i) {
                const int x = dx + i;
                if (x < 0 || x >= g_width)
                    continue;
                int sx = (i * sw) / dw;
                if (sx < 0)
                    sx = 0;
                if (sx >= sw)
                    sx = sw - 1;
                uint32_t src_px = srow[sx];
                uint8_t* p = pixel_ptr(x, y);
                const uint32_t a = src_px >> 24;
                if (a == 0xFF) {
                    p[0] = static_cast<uint8_t>(src_px);
                    p[1] = static_cast<uint8_t>(src_px >> 8);
                    p[2] = static_cast<uint8_t>(src_px >> 16);
                    p[3] = 0xFF;
                } else if (a == 0) {
                    continue;   /* fully transparent: leave what is under it */
                } else {
                    /* src OVER dst, on the 8-bit channels the scanout
                     * is made of. 0..255 alpha, so a>>8 is the 8-bit
                     * weight and (255-a) the other, and the sums are
                     * kept in 16 bits so a rounding pixel cannot wrap. */
                    const uint32_t wa = a * 257u;   /* a -> 0..65535 */
                    const uint32_t wb = 65535u - wa;
                    for (int c = 0; c < 3; ++c) {
                        const uint32_t s = (src_px >> (8 * c)) & 0xFFu;
                        const uint32_t d = p[c];
                        p[c] = static_cast<uint8_t>((s * wa + d * wb + 32768u) >> 16);
                    }
                    p[3] = 0xFF;
                }
            }
        }
        return;
    }
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

void damage_add(int x, int y, int w, int h)
{
    if (!g_backbuf || w <= 0 || h <= 0)
        return;   /* nothing to write, or nowhere to write it */
    /* Clipped to the frame, because a caller asks in desktop
     * coordinates and a window may hang off the edge of the screen. */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > g_width)
        x1 = g_width;
    if (y1 > g_height)
        y1 = g_height;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (g_dmg_x0 >= g_dmg_x1) {   /* nothing pending: this is the box */
        g_dmg_x0 = x0;
        g_dmg_y0 = y0;
        g_dmg_x1 = x1;
        g_dmg_y1 = y1;
        return;
    }
    if (x0 < g_dmg_x0)
        g_dmg_x0 = x0;
    if (y0 < g_dmg_y0)
        g_dmg_y0 = y0;
    if (x1 > g_dmg_x1)
        g_dmg_x1 = x1;
    if (y1 > g_dmg_y1)
        g_dmg_y1 = y1;
}

void damage_all()
{
    damage_add(0, 0, g_width, g_height);
}

void clip_set(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > g_width)
        x1 = g_width;
    if (y1 > g_height)
        y1 = g_height;
    /* A region entirely off the screen clips everything away, which is
     * the honest answer: there is nothing of it on the display. */
    if (x0 > x1)
        x0 = x1;
    if (y0 > y1)
        y0 = y1;
    g_clip_x0 = x0;
    g_clip_y0 = y0;
    g_clip_x1 = x1;
    g_clip_y1 = y1;
}

void clip_pop()
{
    g_clip_x0 = 0;
    g_clip_y0 = 0;
    g_clip_x1 = g_width;
    g_clip_y1 = g_height;
}

void damage_rect(int* x, int* y, int* w, int* h)
{
    if (g_dmg_x0 >= g_dmg_x1) {
        *x = *y = *w = *h = 0;
        return;
    }
    *x = g_dmg_x0;
    *y = g_dmg_y0;
    *w = g_dmg_x1 - g_dmg_x0;
    *h = g_dmg_y1 - g_dmg_y0;
}

void present_rect(int* x, int* y, int* w, int* h)
{
    *x = g_present_x;
    *y = g_present_y;
    *w = g_present_w;
    *h = g_present_h;
}

uint32_t present_bytes()
{
    return g_present_bytes;
}

void present()
{
    present_with(CATT_PAL);
}

/* The damage a transfer is about, taken out of the frame so the next one
 * starts from nothing, and recorded as what this present() is about to
 * move - including the nothing at all, because "a pass changed no pixels"
 * is the result the whole exercise is after and /proc/gfx prints it. */
struct Box {
    int x0, y0, x1, y1;
    bool empty() const { return x0 >= x1; }
    uint32_t pixels() const
    {
        return static_cast<uint32_t>(x1 - x0) * static_cast<uint32_t>(y1 - y0);
    }
};

static Box take_damage()
{
    const Box b{g_dmg_x0, g_dmg_y0, g_dmg_x1, g_dmg_y1};
    g_dmg_x0 = g_dmg_y0 = g_dmg_x1 = g_dmg_y1 = 0;
    g_present_x = b.empty() ? 0 : b.x0;
    g_present_y = b.empty() ? 0 : b.y0;
    g_present_w = b.empty() ? 0 : b.x1 - b.x0;
    g_present_h = b.empty() ? 0 : b.y1 - b.y0;
    g_present_bytes = 0;
    return b;
}

/* Give a box back to the pending damage, for a transfer that could not be
 * made: the host still owes the pixels in it, and a narrower damage on
 * the next pass would not cover them. Widening is all this does, so
 * putting back a box that is already covered costs nothing. */
static void put_damage(const Box& b)
{
    if (b.empty())
        return;
    if (g_dmg_x0 >= g_dmg_x1) {
        g_dmg_x0 = b.x0;
        g_dmg_y0 = b.y0;
        g_dmg_x1 = b.x1;
        g_dmg_y1 = b.y1;
        return;
    }
    if (b.x0 < g_dmg_x0)
        g_dmg_x0 = b.x0;
    if (b.y0 < g_dmg_y0)
        g_dmg_y0 = b.y0;
    if (b.x1 > g_dmg_x1)
        g_dmg_x1 = b.x1;
    if (b.y1 > g_dmg_y1)
        g_dmg_y1 = b.y1;
}

/* The console as pixels (see vga_gfx.h for why this exists at all),
 * drawing the given rows: a bitmask of text rows as
 * vnu::tty::console_dirty_rows() returns them, or ALL_CONSOLE_ROWS for
 * all of them, and putting the frame on the screen. The whole console
 * is what a caller with no better idea asks for, and the only thing
 * that forces it here is a frame that holds nothing of the console yet
 * (g_console_full). */
static void present_rows(uint32_t want)
{
    if (g_console_full) {
        want = ALL_CONSOLE_ROWS;
        g_console_full = false;
    }
    if (want == 0)
        return;   /* nothing to redraw, and nothing to send */

    /* The console is drawn on the grid the tty keeps its cells in, so a
     * text mode of another size would be drawn as itself. */
    uint16_t rows = 0, cols = 0;
    vnu::tty::get_size(&rows, &cols);

    /* In the text mode's own geometry, which is the one thing about it
     * the host can see. A display with a text mode gets it back from
     * the registers exit_to_text() restores; a virtio display shows
     * whatever the scanout resource is, which after a desktop session
     * is still the desktop's mode - so without this the console would
     * come back 1024x768 with 720x400 of text in the middle of it, and
     * stay that way until something changed the mode again. The console
     * is the display's original state, so it is put back to it. This
     * also takes a frame when there is none, which is the normal state
     * of a machine that is sitting at the console: exit_to_text() gives
     * the desktop's frame back to the pool, and the next thing to be
     * shown here is the console. */
    (void)restore_console_geometry(cols, rows);
    if (!g_backbuf) {
        g_console_full = true;   /* no frame to draw into: try again */
        return;
    }

    /* A VGA text cell is 8 glyph pixels wide and 16 tall, and the text
     * mode puts a ninth, always blank, column between cells - which is
     * what makes the mode 720 wide rather than 640. A screen too narrow
     * for a row of those (640x480) gets 8-wide cells, and lands on 640. */
    constexpr int ch = 16;
    const int cw = (g_width >= cols * 9) ? 9 : 8;
    const int grid_w = cols * cw;
    const int grid_h = rows * ch;
    const int x0 = (g_width - grid_w) / 2;
    const int y0 = (g_height - grid_h) / 2;

    /* The console is drawn in the palette the text mode booted with, at
     * either depth: at 8bpp the cells are indices and present_with()
     * expands them there, and at 32bpp they are already colours. */
    const uint8_t (&pal)[16][3] = g_have_saved_pal ? g_saved_pal : CATT_PAL;
    /* Blacking the frame is a whole screen of writes, and only a draw
     * that is doing all of it needs it: a redraw of one row paints over
     * every pixel of that row as it goes, and the rows either side are
     * already the console's. */
    if (want == ALL_CONSOLE_ROWS)
        clear(0);
    /* The rows about to be written are the damage: the console is the
     * only thing presenting here, so it says what it drew rather than
     * claiming the screen (which is what a caller that has not been
     * told anything should do). A single row of text is 16 pixels of a
     * 400-line console, and this is what makes one keystroke cost one
     * row instead of a frame. */
    for (int row = 0; row < rows; ++row) {
        if (!(want & (uint32_t{1} << row)))
            continue;
        damage_add(x0, y0 + row * ch, grid_w, ch);
        for (int col = 0; col < cols; ++col) {
            const uint16_t c = vnu::tty::cell(row, col);
            const uint8_t* bits = g_font[c & 0xFF];
            /* A cell is a character and an attribute: bits 0..3 of the
             * attribute are the foreground, 4..6 the background and bit
             * 7 blink, which the text mode ignores. These are the text
             * mode's own color numbers, so they are looked up in the
             * palette it booted with, not in the desktop's. */
            const uint8_t attr = static_cast<uint8_t>(c >> 8);
            const uint8_t fg = static_cast<uint8_t>(attr & 0x0F);
            const uint8_t bg = static_cast<uint8_t>((attr >> 4) & 0x07);
            for (int y = 0; y < ch; ++y) {
                const int py = y0 + row * ch + y;
                if (py >= g_height)
                    break;
                uint8_t* dst = pixel_ptr(x0 + col * cw, py);
                for (int x = 0; x < cw; ++x) {
                    if (x0 + col * cw + x >= g_width)
                        break;
                    const bool lit = (x < 8) && (bits[y] & (0x80u >> x));
                    store_with(dst, lit ? fg : bg, pal);
                    dst += pixel_bytes();
                }
            }
        }
    }
    present_with(pal);
    /* These cells are in that frame now, so a display that has been told
     * about the screen has nothing left to ask for them. A row written
     * since the mask was read is not in it and stays pending. */
    vnu::tty::console_presented(want);
}

void present_text()
{
    present_rows(ALL_CONSOLE_ROWS);
}

/* Housekeeping the scheduler calls between passes: put the console on
 * the screen when it has changed and the display cannot be doing it
 * itself. A display with a text mode of its own needs nothing - the
 * characters land in the text plane and the hardware shows them - but a
 * virtio display is showing a scanout resource, and the text plane is
 * not it, so after a desktop session has pointed the host at one the
 * console is a still picture of whatever was last presented and every
 * keystroke, every prompt and every line of output goes nowhere: the
 * shell is alive and the monitor is blank. Hence one redraw and
 * transfer whenever the console has moved, which is the same thing the
 * VGA path gets from the CRTC.
 *
 * Rate-limited, because a burst of output is many write(2) batches and
 * the frame is a megabyte and a bit at this geometry: a pass every
 * 60 ms is faster than anyone can read and collapses a page of output
 * into a handful of transfers. Nothing at all happens while the console
 * is idle, and nothing on a display with a text mode - nor on a virtio
 * display before a desktop has ever claimed the scanout, which is the
 * whole of a machine's first console session.
 *
 * Only the rows that changed are drawn, which is what makes the transfer
 * here cheap enough to sit in a scheduler pass: a keystroke echo is one
 * row of 720x16 pixels, not a screen of them. The transfer is of the
 * whole frame either way, but that is the host's work and the host is
 * not the guest's problem to wait on. */
void console_tick()
{
    if (!g_scanout_shown || g_in_gfx_mode)
        return;   /* either the text plane is still what the host shows, or
                   * the desktop owns the screen and presents every frame */
    if (vnu::tty::console_dirty_rows() == 0)
        return;
    if (static_cast<int32_t>(vnu::proc::now_jiffies() - g_console_tick) <
        CONSOLE_TICK_JIFFIES)
        return;
    g_console_tick = vnu::proc::now_jiffies();
    present_rows(vnu::tty::console_dirty_rows());
}

void present_with(const uint8_t (&pal)[16][3])
{
    /* What the host is owed. Taken here rather than in present() because
     * the console has its own idea of what changed (present_rows damages
     * the rows it drew) and both paths go through this one transfer. */
    Box dmg = take_damage();
    /* Clipped to the frame once more, here, where the box is about to
     * drive a copy. damage_add() clips too, but it clips with the geometry
     * of the moment it was called, and a frame that is taken after the
     * damage was declared is smaller than the box said: a transfer is
     * then asked for rows and columns the frame does not have, and the
     * copy walks off it. Clamping here makes that impossible from either
     * direction. */
    if (dmg.x1 > g_width)
        dmg.x1 = g_width;
    if (dmg.y1 > g_height)
        dmg.y1 = g_height;
    if (dmg.empty())
        return;   /* a pass that changed nothing: no copy, no notify */

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
         * nothing to hand over until the next attempt gets frames. */
        if (nsegs == 0) {
            put_damage(dmg);
            return;
        }

        /* The scanout is a list of frame runs, so the image is written
         * run by run: a row is 4 bytes per pixel wide and can straddle
         * two of them, while a run holds many rows. Splitting per row
         * keeps the copy below free of any segment bookkeeping.
         *
         * Only the damaged columns of the damaged rows are written, and
         * the run is walked from where the box starts rather than from
         * the top of the resource, which is the point of the whole
         * mechanism: the copy is proportional to what changed. */
        const uint32_t full_row = static_cast<uint32_t>(g_width) * 4u;
        /* Into the first damaged pixel: the start of the first damaged
         * row plus the columns to the left of the first damaged one. The
         * x term is the one that makes a subrect land where it belongs
         * rather than in the row's leftmost columns. */
        uint32_t lead = static_cast<uint32_t>(dmg.y0) * full_row +
                        static_cast<uint32_t>(dmg.x0) * 4u;
        uint32_t seg = 0;
        vnu::virtio_gpu::ScanoutSegment s = vnu::virtio_gpu::scanout_segment(0);
        uint32_t room = 0;
        /* Positioned at the first damaged pixel. */
        while (seg < nsegs) {
            s = vnu::virtio_gpu::scanout_segment(seg);
            if (lead < s.bytes) {
                room = s.bytes - lead;
                break;
            }
            lead -= s.bytes;
            ++seg;
        }
        if (seg == nsegs) {
            put_damage(dmg);   /* the list ran out: the host is still owed it */
            return;
        }
        uint8_t* dst = reinterpret_cast<uint8_t*>(s.phys + lead);
        const uint32_t row_bytes = static_cast<uint32_t>(dmg.x1 - dmg.x0) * 4u;
        for (int y = dmg.y0; y < dmg.y1; ++y) {
            /* A 32bpp frame is already B8G8R8X8 - the desktop composited
             * straight into the scanout's format, so this is the copy
             * the 8bpp path used to do after expanding every pixel
             * through the DAC. An 8bpp frame is still palette indices
             * and still has to be expanded, in the caller's palette so
             * the console presents in the one the text mode booted with. */
            const uint8_t* src = pixel_ptr(dmg.x0, y);
            uint32_t left = row_bytes;
            while (left > 0) {
                if (room == 0) {
                    if (++seg == nsegs) {
                        put_damage(dmg);
                        return;   /* the list ran out: still owed */
                    }
                    s = vnu::virtio_gpu::scanout_segment(seg);
                    dst = reinterpret_cast<uint8_t*>(s.phys);
                    room = s.bytes;
                }
                const uint32_t chunk = (left < room) ? left : room;
                if (g_bpp == 32) {
                    memcpy(dst, src, chunk);
                    src += chunk;
                } else {
                    uint8_t* out = dst;
                    const uint8_t* in = src;
                    for (uint32_t b = 0; b < chunk; b += 4u) {
                        const uint8_t* rgb = pal[*in++];
                        out[0] = static_cast<uint8_t>((rgb[2] << 2) | (rgb[2] >> 4));
                        out[1] = static_cast<uint8_t>((rgb[1] << 2) | (rgb[1] >> 4));
                        out[2] = static_cast<uint8_t>((rgb[0] << 2) | (rgb[0] >> 4));
                        out[3] = 0xFF;
                        out += 4;
                    }
                    src += chunk / 4u;
                }
                room -= chunk;
                left -= chunk;
                dst += chunk;
            }
        }
        g_present_bytes = dmg.pixels() * 4u;
        vnu::virtio_gpu::present(dmg.x0, dmg.y0, dmg.x1 - dmg.x0,
                                 dmg.y1 - dmg.y0);
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
    /* The VBE path never gets past resolve_bpp(): a 32bpp frame exists
     * only where a virtio-gpu is presenting, and it went out above. A
     * row is a byte per pixel here, and the copy is per row of the
     * damaged box rather than one memcpy of the frame: at 640x480 the
     * whole frame is 300 KiB and a cursor move is a few hundred bytes of
     * it. */
    auto* lfb = reinterpret_cast<uint8_t*>(VBE_LFB_ADDR);
    for (int y = dmg.y0; y < dmg.y1; ++y) {
        memcpy(lfb + static_cast<long>(y) * g_width + dmg.x0,
               row_ptr(y) + dmg.x0,
               static_cast<unsigned long>(dmg.x1 - dmg.x0));
    }
    g_present_bytes = dmg.pixels();
}

} // namespace vnu::vgfx
