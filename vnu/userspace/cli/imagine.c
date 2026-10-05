/*
 * imagine — put a picture on the screen, with nothing between it and the
 * display but a decoder.
 *
 * The point of this program is to be the plainest thing that can draw:
 * no window, no canvas, no compositor, no palette. It opens /dev/fb0,
 * which hands back the display's own frame, decodes the file with the
 * shared px.h and writes pixels into that memory. What the monitor shows
 * afterwards is the decoder's output and the driver's - nothing of this
 * program's own idea of what a picture should look like. That is what
 * makes it a test: a vgfx app goes through a 16-colour palette and a
 * window, so when the colours are wrong there, the two steps that could
 * have done it are too many to tell apart.
 *
 * Only px.h and the device are used. No vgfx.
 */
#include <vnu/abi.h>
#include <vlibc/unistd.h>
#include <vlibc/fcntl.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include "../gui/px.h"

/* A file bigger than this is refused rather than decoded: the frame is
 * drawn into memory the size of the *display*, and a 40-megapixel JPEG
 * costs hundreds of megabytes to hold as RGB whatever the display can
 * show. */
#define MAX_FILE (4u * 1024u * 1024u)

/* What px.h hands back, in 8 bits per channel. */
#define SRC_BYTES_PER_PIXEL 3

static void say(const char* s)
{
    write(2, s, strlen(s));
}

/* Every refusal says what it was, because a program whose whole output is
 * a picture has no other way to be told it failed. */
static int fail(const char* what)
{
    say("imagine: ");
    say(what);
    say("\n");
    return 1;
}

/* Read the whole file into a fresh buffer, its length in *len. Null on any
 * failure, including a short read - a decoder handed the uninitialized
 * tail of an allocation would read whatever the pool left there, which is
 * a bug report about the wrong program. */
static uint8_t* slurp(const char* path, unsigned* len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || (unsigned long)size > MAX_FILE) {
        close(fd);
        return 0;
    }
    uint8_t* buf = malloc((unsigned long)size);
    if (!buf) {
        close(fd);
        return 0;
    }
    unsigned got = 0;
    while (got < (unsigned)size) {
        long n = read(fd, buf + got, (unsigned long)((unsigned)size - got));
        if (n <= 0)
            break;
        got += (unsigned)n;
    }
    close(fd);
    if (got != (unsigned)size) {
        free(buf);
        return 0;
    }
    *len = got;
    return buf;
}

/* How many bytes a width x height picture takes as RGB, refusing the ones
 * that cannot: the multiplication below is in int, and a picture whose
 * header lies about its size would otherwise wrap to a small number and
 * be written past the end of its own buffer. px.h caps BMP and PNG at
 * 4096, but a JPEG is bounded by its own SOF marker and nothing else, so
 * this is the check that covers it. */
static int rgb_bytes(int w, int h, unsigned* out)
{
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return 0;
    unsigned long px = (unsigned long)w * (unsigned long)h;
    unsigned long bytes = px * SRC_BYTES_PER_PIXEL;
    if (bytes == 0 || bytes > 64u * 1024u * 1024u)
        return 0;
    *out = (unsigned)bytes;
    return 1;
}

/* The display's 16 DAC entries, as 0xXXRRGGBB words - the same words a
 * 32bpp pixel is made of. Read once, because it is the kernel's palette
 * and a program that hardcoded those bytes would be right until the
 * palette changed under it (see VNU_SYS_gfx_palette in abi.md). */
static uint32_t g_dac[VNU_GFX_PALETTE_SLOTS];
static int g_dac_have;

static void load_dac(void)
{
    if (g_dac_have)
        return;
    /* 8bpp is the only depth that needs it, so a 32bpp display never
     * makes this syscall. */
    if (syscall(VNU_SYS_gfx_palette, g_dac, VNU_GFX_PALETTE_BYTES) < 0)
        return;
    g_dac_have = 1;
}

/* One pixel in the frame's own format. A 32bpp frame is a B8G8R8X8 word
 * - blue, green, red, then the byte the compositor reads as coverage - so
 * the colour goes in whole and nothing is lost. An 8bpp frame has a DAC
 * in front of it and can only hold one of sixteen slots, so a true-colour
 * picture is reduced to the nearest entry: on that display there is no
 * other answer, and picking the nearest is what makes the reduction the
 * display's business rather than this program's. */
static void put_rgb(uint8_t* frame, int stride_bytes, int x, int y,
                    int r, int g, int b, int bpp)
{
    if (x < 0 || y < 0)
        return;
    uint8_t* p = frame + (long)y * stride_bytes + (long)x * (bpp / 8);
    if (bpp == 32) {
        p[0] = (uint8_t)b;
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)r;
        p[3] = 0xFF; /* opaque: the compositor blends coverage */
        return;
    }
    int best = 0;
    long best_d = -1;
    for (int i = 0; i < VNU_GFX_PALETTE_SLOTS; ++i) {
        uint32_t c = g_dac[i];
        int cr = (int)((c >> 16) & 0xFF);
        int cg = (int)((c >> 8) & 0xFF);
        int cb = (int)(c & 0xFF);
        long dr = cr - r;
        long dg = cg - g;
        long db = cb - b;
        long d = dr * dr + dg * dg + db * db;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    *p = (uint8_t)best;
}

static void clear_row(uint8_t* frame, int stride_bytes, int w, int y, int bpp)
{
    uint8_t* p = frame + (long)y * stride_bytes;
    if (bpp == 32) {
        for (int x = 0; x < w; ++x) {
            p[x * 4 + 0] = 0;
            p[x * 4 + 1] = 0;
            p[x * 4 + 2] = 0;
            p[x * 4 + 3] = 0xFF;
        }
        return;
    }
    for (int x = 0; x < w; ++x)
        p[x] = 0; /* the DAC's black is slot 0 on both paths */
}

/* Nearest-neighbour fit into the frame, centred, keeping the picture's
 * own shape (letterboxed rather than stretched). Integer arithmetic
 * throughout: a float scale of a 4000-pixel picture into a 480-pixel
 * window is a rounding question, and the answer should not depend on the
 * FPU being in a mood. */
static void fit_center(const uint8_t* rgb, int rw, int rh,
                       uint8_t* frame, int fw, int fh,
                       int stride_bytes, int bpp)
{
    if (rw <= 0 || rh <= 0 || fw <= 0 || fh <= 0)
        return;
    if ((long)rw * fh <= (long)rh * fw) {
        /* Height decides: the picture is as wide as fits, rows and all. */
        long out_w = ((long)rw * fh + rh / 2) / rh;
        if (out_w < 1)
            out_w = 1;
        if (out_w > fw)
            out_w = fw;
        long x0 = (fw - out_w) / 2;
        for (int y = 0; y < fh; ++y) {
            int sy = (int)(((long)y * rh + fh / 2) / fh);
            if (sy >= rh)
                sy = rh - 1;
            for (long x = 0; x < out_w; ++x) {
                int sx = (int)(((long)x * rw + out_w / 2) / out_w);
                if (sx >= rw)
                    sx = rw - 1;
                const uint8_t* s = rgb + ((long)sy * rw + sx) * SRC_BYTES_PER_PIXEL;
                put_rgb(frame, stride_bytes, (int)(x0 + x), y, s[0], s[1], s[2], bpp);
            }
        }
        return;
    }
    /* Width decides: as many columns as fit, rows follow. */
    long out_h = ((long)rh * fw + rw / 2) / rw;
    if (out_h < 1)
        out_h = 1;
    if (out_h > fh)
        out_h = fh;
    long y0 = (fh - out_h) / 2;
    for (long y = 0; y < out_h; ++y) {
        int sy = (int)((y * rh + out_h / 2) / out_h);
        if (sy >= rh)
            sy = rh - 1;
        for (int x = 0; x < fw; ++x) {
            int sx = (int)(((long)x * rw + fw / 2) / fw);
            if (sx >= rw)
                sx = rw - 1;
            const uint8_t* s = rgb + ((long)sy * rw + sx) * SRC_BYTES_PER_PIXEL;
            put_rgb(frame, stride_bytes, x, (int)(y0 + y), s[0], s[1], s[2], bpp);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return fail("usage: imagine FILE");

    unsigned len = 0;
    uint8_t* file = slurp(argv[1], &len);
    if (!file)
        return fail("cannot read the file");

    int w = 0;
    int h = 0;
    int fmt = px_probe(file, len, &w, &h);
    unsigned rgb_size = 0;
    if (fmt == PX_NONE || !rgb_bytes(w, h, &rgb_size)) {
        free(file);
        return fail("not an image this can show");
    }
    uint8_t* rgb = malloc(rgb_size);
    if (!rgb) {
        free(file);
        return fail("out of memory");
    }
    int rc = px_decode(fmt, file, len, w, h, rgb);
    free(file);
    if (rc != 0) {
        free(rgb);
        return fail("cannot decode it");
    }

    /* Before the display is taken, and this is not a matter of taste:
     * anything printed while the picture is up takes it down again. The
     * console draws itself as soon as it has a row of new text, and in a
     * graphics mode that means putting the card back into the console's
     * own geometry - which hands back the frame the picture is in. So
     * the line is printed now, where it waits in the console's buffer
     * until the picture has been looked at, and turns up in the
     * scrollback just above the prompt this returns to. */
    say("imagine: any key brings the shell back\n");

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        free(rgb);
        return fail("no display to draw on");
    }
    struct vnu_fb_info fb;
    if (read(fd, &fb, sizeof(fb)) != (long)sizeof(fb)) {
        close(fd);
        free(rgb);
        return fail("the display has no frame");
    }

    int bpp = (int)fb.bpp;
    int fw = (int)fb.width;
    int fh = (int)fb.height;
    int stride = (int)fb.stride;
    uint8_t* frame = (uint8_t*)fb.va;

    if (bpp == 8)
        load_dac();
    for (int y = 0; y < fh; ++y)
        clear_row(frame, stride, fw, y, bpp);
    fit_center(rgb, w, h, frame, fw, fh, stride, bpp);
    free(rgb);

    /* Nothing was copied through the kernel: the pixels are already in
     * the frame. The write is the handshake that puts them on the
     * screen. */
    write(fd, 0, 0);

    /* And then wait, which is the whole of being a full-screen program:
     * the frame this process was given is the display's until it closes
     * it, so the picture stays up for as long as it takes to look at it.
     * One key of any kind closes the device and gives the console back,
     * the way `man' does it. */
    for (;;) {
        char c = 0;
        if (read(0, &c, 1) <= 0)
            break;
        break;
    }
    close(fd);
    return 0;
}