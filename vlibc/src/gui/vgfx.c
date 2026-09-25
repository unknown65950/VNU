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
#include <vlibc/vgfx_font.h>
#include <vlibc/unistd.h>
#include <vlibc/sys/syscall.h>

static unsigned char fb[VGFX_W * VGFX_H];

void vgfx_clear(int color)
{
    for (int i = 0; i < VGFX_W * VGFX_H; ++i)
        fb[i] = (unsigned char)color;
}

void vgfx_put_pixel(int x, int y, int color)
{
    if (x < 0 || y < 0 || x >= VGFX_W || y >= VGFX_H)
        return;
    fb[y * VGFX_W + x] = (unsigned char)color;
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
    long rc = lseek(VGFX_FD, 0, 0);
    if (rc < 0)
        return;
    write(VGFX_FD, fb, (unsigned long)(VGFX_W * VGFX_H));
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