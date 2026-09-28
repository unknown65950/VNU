/*
 * vgfx — pixel framebuffer + input helper for VNU gfx-windowed apps.
 *
 * The app draws into a private 480x340 pixel buffer using the vgfx_*
 * primitives, then calls vgfx_flush() to push it to the kernel's gfx
 * surface (fd 3, an /dev/fb-like pipe: lseek(3,0) then write(3,...
 * VGFX_W*VGFX_H)).
 *
 * Mouse events come back through stdin as:
 *   ESC '[' 'M' <button> <xl> <xh> <yl> <yh>
 * (little-endian 16-bit coordinates; a 480-wide canvas doesn't fit in
 * a single byte). vgfx_poll() parses that stream and returns
 * vgfx_event_t structs.
 */
#include <vlibc/vgfx.h>
#include <vlibc/stdlib.h>
#include <vlibc/vgfx_font.h>
#include <vlibc/unistd.h>
#include <vlibc/sys/syscall.h>
#include <vnu/abi.h>

/* The canvas this program draws into.
 *
 * In a window it is *shared memory*: gfx_surface(2) maps one set of
 * pages into the app at VGFX_SURFACE_VA and into the kernel, so the
 * pixels are written once, in place, and the compositor blits those
 * very bytes on its next pass. Nothing is copied and no frame ever
 * crosses a syscall.
 *
 * fb_private is only for a program drawing with no window at all (vgfx
 * called from a console, or a task whose surface could not be mapped):
 * it has to draw into something, and the result simply is not shown. */
static unsigned char* fb;
static unsigned char* fb_private;

/* The canvas is in the depth of the display the program is drawing on,
 * and the palette is the kernel's - the image palette the desktop
 * composites with, and the one a VGFX_* slot means. Both are asked for
 * once, on the first draw, and both are the same on every machine: only
 * the numbers differ. A program never has to know which it got, and
 * never has to hardcode a colour. */
static int bpp;
static unsigned int pal[16];

static void resolve_state(void)
{
    struct vnu_gfx_info info;
    unsigned int table[16];
    if (syscall(VNU_SYS_gfx_getinfo, (long)&info) == 0 &&
        (info.bpp == 8 || info.bpp == 32))
        bpp = (int)info.bpp;
    else
        bpp = 8;   /* a kernel too old to answer: the 8bpp canvas it had */
    if (syscall(VNU_SYS_gfx_palette, (long)table,
                (long)sizeof(table)) == 16) {
        for (int i = 0; i < 16; ++i)
            pal[i] = table[i] | 0xFF000000u;   /* opaque, until a program says otherwise */
    } else {
        /* No palette to be had: the slots keep their VGA meaning, so a
         * program drawing still lands on the 16 colours of the default
         * DAC rather than on nothing. */
        static const unsigned char vga[16][3] = {
            {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
            {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
            {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
            {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255},
        };
        for (int i = 0; i < 16; ++i)
            pal[i] = 0xFF000000u | ((unsigned int)vga[i][2] << 16) |
                     ((unsigned int)vga[i][1] << 8) | vga[i][0];
    }
}

static int state_resolved;

static unsigned char* pixels(void)
{
    if (!fb) {
        if (!state_resolved) {
            state_resolved = 1;
            resolve_state();
        }
        long va = syscall(SYS_gfx_surface, 0UL);
        if (va) {
            fb = (unsigned char*)va;
        } else {
            /* A windowless program still has to draw into something, and
             * the buffer has to be as deep as the display is: at 32bpp
             * that is four times the bytes, so it is taken from the heap
             * rather than left as a quarter-megabyte of .bss in every
             * app that links this. */
            fb_private = (unsigned char*)malloc((unsigned long)
                                                VGFX_W * VGFX_H * (bpp / 8));
            fb = fb_private ? fb_private : (unsigned char*)0;
        }
    }
    return fb;
}

/* One colour as the *bytes* of a pixel in the current depth, and how
 * many of them there are: the palette slot itself at 8bpp, and the
 * colour the kernel's palette gives it at 32bpp - B, G, R, and a fourth
 * byte that is how a canvas says "nothing has blended here yet". Every
 * primitive goes through here, which is why a program can use the same
 * VGFX_* constants on either machine. */
static int pixel_of(int color, unsigned int out[4])
{
    if (bpp == 8) {
        out[0] = (unsigned int)(color & 0xFF);
        return 1;
    }
    const unsigned int rgb = pal[color & 0x0F];
    out[0] = rgb & 0xFFu;          /* blue: the byte order the frame is in */
    out[1] = (rgb >> 8) & 0xFFu;   /* green */
    out[2] = (rgb >> 16) & 0xFFu;  /* red */
    out[3] = 0xFFu;                /* opaque: the compositor blends 0..255 */
    return 4;
}

void vgfx_clear(int color)
{
    unsigned char* p = pixels();
    unsigned int px[4];
    int n = pixel_of(color, px);
    if (!p)
        return;
    for (int i = 0; i < VGFX_W * VGFX_H; ++i)
        for (int c = 0; c < n; ++c)
            p[i * n + c] = (unsigned char)px[c];
}

void vgfx_put_pixel(int x, int y, int color)
{
    unsigned char* p;
    unsigned int px[4];
    int n;
    if (x < 0 || y < 0 || x >= VGFX_W || y >= VGFX_H)
        return;
    p = pixels();
    n = pixel_of(color, px);
    if (!p)
        return;
    for (int c = 0; c < n; ++c)
        p[(y * VGFX_W + x) * n + c] = (unsigned char)px[c];
}

/* The 8bpp answer to a colour that is not one of the sixteen: the
 * nearest slot by squared distance, optionally dithered. Defined below,
 * next to vgfx_put_rgb_dither(). */
static void quantize(int x, int y, unsigned int rgb, int dither);

/* True colour, which is what a 32bpp canvas is for: the pixel goes in
 * as it is, 24 bits of it, and on an 8bpp display there is no such thing
 * so the colour is quantized down to the nearest of the 16 slots - the
 * one place in this file where a picture loses what it had, and only on
 * a machine whose display cannot show it. */
void vgfx_put_rgb(int x, int y, unsigned int rgb)
{
    unsigned char* p;
    if (x < 0 || y < 0 || x >= VGFX_W || y >= VGFX_H)
        return;
    p = pixels();
    if (!p)
        return;
    if (bpp == 32) {
        unsigned int* q = (unsigned int*)p + y * VGFX_W + x;
        *q = (rgb & 0x00FFFFFFu) | 0xFF000000u;
        return;
    }
    quantize(x, y, rgb, 0);
}

/* 4x4 Bayer, the standard answer to banding on a 16-colour display: the
 * error of a pixel is spread over its neighbours' choice rather than
 * its own, so a gradient that could only be 16 steps comes out as a
 * dither. On a 32bpp display there is nothing to dither - the gradient
 * is already as fine as the picture - so this is vgfx_put_rgb(). */
static void quantize(int x, int y, unsigned int rgb, int dither)
{
    static const unsigned char BAYER[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
    };
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF),
        b = (int)(rgb & 0xFF);
    int best = 0, bd = 1 << 30;
    for (int c = 0; c < 16; ++c) {
        const int pr = (int)((pal[c] >> 16) & 0xFF),
                  pg = (int)((pal[c] >> 8) & 0xFF),
                  pb = (int)(pal[c] & 0xFF);
        const int dr = r - pr, dg = g - pg, db = b - pb;
        const int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = c; }
    }
    if (dither) {
        /* The pixel's brightness decides how often the next slot up is
         * taken: the darker it is, the fewer - so a dark region keeps
         * its slot and a bright one is mostly stepped over. Both sides
         * are 0..15, which is the whole reason for the mask. */
        const int e = (r * 77 + g * 150 + b * 29) >> 8;
        if ((e & 15) > BAYER[y & 3][x & 3])
            best = (best + 1) & 15;
    }
    vgfx_put_pixel(x, y, best);
}

void vgfx_put_rgb_dither(int x, int y, unsigned int rgb)
{
    unsigned char* p = pixels();
    if (!p || bpp != 32) {
        quantize(x, y, rgb, 1);
        return;
    }
    vgfx_put_rgb(x, y, rgb);
}

/* The same, with coverage: the canvas's unused top byte is what the
 * compositor blends, so a window drawn over the desktop keeps whatever
 * was behind it. Ignored on an 8bpp display, where there is no channel
 * to carry it - the same honest answer the rest of this file gives. */
void vgfx_put_rgba(int x, int y, unsigned int rgb, unsigned int a)
{
    unsigned char* p;
    unsigned int* q;
    if (x < 0 || y < 0 || x >= VGFX_W || y >= VGFX_H)
        return;
    p = pixels();
    if (!p)
        return;
    if (bpp != 32) {
        /* No channel to carry the coverage, so the colour is quantized
         * to the nearest slot and the coverage is dropped - the same
         * answer vgfx_put_rgb gives, which is what an 8bpp display can
         * say about a half-transparent pixel. */
        quantize(x, y, rgb, 0);
        return;
    }
    if (a > 255)
        a = 255;
    q = (unsigned int*)p + y * VGFX_W + x;
    *q = (rgb & 0x00FFFFFFu) | (a << 24);
}

void vgfx_fill_rect(int x, int y, int w, int h, int color)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            vgfx_put_pixel(x + i, y + j, color);
}

void vgfx_hline(int x, int y, int w, int color)
{
    for (int i = 0; i < w; ++i)
        vgfx_put_pixel(x + i, y, color);
}

void vgfx_vline(int x, int y, int h, int color)
{
    for (int j = 0; j < h; ++j)
        vgfx_put_pixel(x, y + j, color);
}

void vgfx_rect(int x, int y, int w, int h, int color)
{
    vgfx_hline(x, y, w, color);
    vgfx_hline(x, y + h - 1, w, color);
    vgfx_vline(x, y, h, color);
    vgfx_vline(x + w - 1, y, h, color);
}

void vgfx_char(int x, int y, char ch, int color)
{
    const unsigned char* g = VGFX_FONT[(unsigned char)ch];
    for (int r = 0; r < 16; ++r)
        for (int c = 0; c < 8; ++c)
            if (g[r] & (0x80 >> c))
                vgfx_put_pixel(x + c, y + r, color);
}

/* Compact 8×8 font derived by OR-pairing adjacent rows of the 8×16
 * VGA table, giving a dense bold face that keeps the console look
 * while letting dense lists and panel text fit at half the height. */
static unsigned char font8[256][8];
static int font8_built = 0;

static void build_font8(void)
{
    if (font8_built)
        return;
    for (int ch = 0; ch < 256; ++ch) {
        const unsigned char* g = VGFX_FONT[ch];
        for (int r = 0; r < 8; ++r)
            font8[ch][r] = (unsigned char)(g[r * 2] | g[r * 2 + 1]);
    }
    font8_built = 1;
}

void vgfx_char8(int x, int y, char ch, int color)
{
    build_font8();
    const unsigned char* g = font8[(unsigned char)ch];
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            if (g[r] & (0x80 >> c))
                vgfx_put_pixel(x + c, y + r, color);
}

void vgfx_str8(int x, int y, const char* s, int color)
{
    while (s && *s) {
        vgfx_char8(x, y, *s, color);
        x += 8;
        ++s;
    }
}

int vgfx_text_width8(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n * 8;
}

void vgfx_str(int x, int y, const char* s, int color)
{
    while (s && *s) {
        vgfx_char(x, y, *s, color);
        x += 8;
        ++s;
    }
}

int vgfx_text_width(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n * 8;
}

void vgfx_flush(void)
{
    /* Kept because every app ends a frame with it, and it says what the
     * frame is now: it is already where the compositor reads it, so
     * there is nothing to push. Asking for the surface here (rather
     * than in every primitive) keeps a windowless program from claiming
     * a pixel window it cannot have. */
    (void)pixels();
}

/* --- Input: parse the ESC '[' 'M' btn xl xh yl yh stream on fd 0 --- */

/* Drop payload space: the kernel reserves up to 128 path bytes; keep a
 * little slack so a truncated path still NUL-terminates. */
#define DROP_BUF 128
static char drop_buf[DROP_BUF];

/* dnd_declare(2): hand the GUI the draggable path of the pressed item. */
int vnu_dnd_declare(const char* path)
{
    return (int)syscall(VNU_SYS_dnd_declare, (long)path);
}

void vgfx_get_resolution(int* w, int* h)
{
    /* Pointers are the caller's, and the kernel validates them like any
     * other; a null one is allowed on both sides. */
    (void)syscall(VNU_SYS_gfx_getmode, (long)w, (long)h);
}

int vgfx_get_info(struct vnu_gfx_info* info)
{
    /* The kernel fills the struct in place. Unlike vgfx_get_resolution()
     * there is nothing to leave out: a program that wants the depth
     * wants all of it, and one call is one call. */
    if (!info)
        return -1;
    return (int)syscall(VNU_SYS_gfx_getinfo, (long)info) == 0 ? 0 : -1;
}

int vgfx_set_resolution(int w, int h)
{
    return (int)syscall(VNU_SYS_gfx_setmode, (long)w, (long)h);
}

int vgfx_poll(vgfx_event_t* ev)
{
    enum { S_IDLE, S_ESC, S_BRACKET, S_M, S_BTN, S_XLO, S_XHI,
           S_YLO, S_DLEN_LO, S_DLEN_HI, S_DROP } state = S_IDLE;
    int drop_left = 0;
    int drop_pos = 0;
    ev->type = VGFX_EV_NONE;
    ev->x = ev->y = 0;
    ev->button = 0;
    ev->key = 0;
    ev->drop = 0;

    /* Because feed_mouse() enqueues all eight bytes of a message
     * atomically, a byte stream that starts "ESC [" always finishes. */
    for (;;) {
        char b;
        long n = read(0, &b, 1); /* blocks until a byte is ready */
        if (n != 1)
            continue;
        switch (state) {
        case S_IDLE:
            if (b == 0x1B)
                state = S_ESC;
            else {
                ev->type = VGFX_EV_KEY;
                ev->key = b;
                return 1;
            }
            break;
        case S_ESC:
            if (b == '[')
                state = S_BRACKET;
            else {
                /* ESC itself is a key; the byte after it belongs to a
                 * different event — return it and drop the ESC. */
                ev->type = VGFX_EV_KEY;
                ev->key = b;
                state = S_IDLE;
                return 1;
            }
            break;
        case S_BRACKET:
            if (b == 'M')
                state = S_M;
            else if (b == 'D')
                state = S_DLEN_LO; /* drop message: len_lo ... */
            else
                state = S_IDLE; /* not our protocol; resync */
            break;
        case S_M:
            ev->button = (int)(unsigned char)b;
            state = S_BTN;
            break;
        case S_BTN:
            ev->x = (int)(unsigned char)b; /* x low byte */
            state = S_XLO;
            break;
        case S_XLO:
            ev->x |= (int)(unsigned char)b << 8; /* x high byte */
            state = S_XHI;
            break;
        case S_XHI:
            ev->y = (int)(unsigned char)b; /* y low byte */
            state = S_YLO;
            break;
        case S_YLO:
            ev->y |= (int)(unsigned char)b << 8; /* y high byte */
            if (ev->button == 1)
                ev->type = VGFX_EV_PRESS;
            else if (ev->button == 2)
                ev->type = VGFX_EV_RELEASE;
            else if (ev->button == 4)
                ev->type = VGFX_EV_DRAG_CANCEL;
            else {
                /* Button 3: the GUI wraps a bare Esc keypress this way
                 * (a lone 0x1B byte would stall the parser). */
                ev->type = VGFX_EV_KEY;
                ev->key = 0x1B;
            }
            state = S_IDLE;
            return 1;
        case S_DLEN_LO:
            drop_left = (int)(unsigned char)b; /* length low byte */
            state = S_DLEN_HI;
            break;
        case S_DLEN_HI:
            drop_left |= (int)(unsigned char)b << 8; /* length high byte */
            drop_pos = 0;
            if (drop_left > DROP_BUF - 1)
                drop_left = DROP_BUF - 1; /* truncate, keep NUL room */
            state = (drop_left > 0) ? S_DROP : S_IDLE;
            break;
        case S_DROP:
            drop_buf[drop_pos] = b;
            ++drop_pos;
            if (--drop_left == 0) {
                drop_buf[drop_pos] = 0;
                ev->type = VGFX_EV_DROP;
                ev->drop = drop_buf;
                state = S_IDLE;
                return 1;
            }
            break;
        }
    }
}