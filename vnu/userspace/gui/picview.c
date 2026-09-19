/*
 * picview — tiny image viewer for the VNU desktop.
 *
 * Walks the kernel VFS directory /pics the same way the files app walks
 * its cwd (opendir/readdir, struct dirent, d_type[1] == 8 for regular
 * files), reads each picture's bytes with open/read/close (the pattern
 * prefs.c uses), decodes BMP / PNG / JPEG with the self-contained px.h
 * decoder and renders the picture cropped/scaled to the window's client
 * area (VGFX_W x VGFX_H) by colour-quantising each RGB888 pixel to the
 * nearest of the 16 Catppuccin DAC palette indices with an optional
 * ordered dither mask.
 *
 * The window is pre-created by the desktop (title = app name); we never
 * call vgfx_open — just draw, vgfx_flush and service events via
 * vgfx_poll, exactly like calc/files/prefs.
 *
 * Keys:
 *   [  previous picture    ]  next picture
 *   z  toggle zoom (fit / 1:1)   d  toggle dither
 *   Esc  close the window
 */
#include <vlibc/vgfx.h>
#include <vlibc/dirent.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>

#include "px.h"

/* Decoded RGB888 image. Allocated on the brk heap (not a static BSS
 * array): the windowed task's app image is capped at 96 KiB of code+
 * data+.bss (wintask.cpp APP_PAGES=24), so a full VGFX_W x VGFX_H RGB
 * buffer would overflow it and page-fault the GUI task to death.
 * Freed and re-allocated each time a picture is loaded. */
static uint8_t*  g_pic_rgb;
static int       g_pic_w, g_pic_h;

#define MAX_PICS 16
#define NAME_CAP 32
#define PATH_CAP 64
#define DATA_CAP 20480

static char  g_names[MAX_PICS][NAME_CAP];
static int   g_n;
static int   g_sel;
static int   g_loaded = -1;
static int   g_zoom;
static int   g_dither = 1;

/* Catppuccin Mocha DAC palette (6-bit per channel), mirrors
 * kernel/drivers/vga_gfx.cpp CATT_PAL. */
static const uint8_t PAL[16][3] = {
    {7, 7, 11}, {34, 45, 62}, {41, 56, 40}, {29, 49, 59},
    {60, 34, 42}, {50, 41, 61}, {62, 44, 33}, {12, 12, 17},
    {6, 6, 9}, {45, 47, 63}, {37, 56, 53}, {34, 55, 58},
    {58, 40, 43}, {61, 48, 57}, {62, 56, 43}, {51, 53, 61},
};

static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static void pcat(char* out, int cap, const char* a, const char* b)
{
    int i = 0;
    while (a && a[i] && i < cap - 2) {
        out[i] = a[i];
        ++i;
    }
    out[i++] = '/';
    int j = 0;
    while (b && b[j] && i < cap - 1) {
        out[i++] = b[j++];
    }
    out[i] = 0;
}

static void enumerate(void)
{
    g_n = 0;
    DIR* d = opendir("/pics");
    if (!d)
        return;
    struct dirent* de;
    while ((de = readdir(d)) && g_n < MAX_PICS) {
        if (de->d_type != 8)
            continue; /* only regular files are pictures */
        int i = 0;
        while (de->d_name[i] && i < NAME_CAP - 1) {
            g_names[g_n][i] = de->d_name[i];
            ++i;
        }
        g_names[g_n][i] = 0;
        ++g_n;
    }
    closedir(d);
}

static int load_sel(void)
{
    char path[PATH_CAP];
    pcat(path, PATH_CAP, "/pics", g_names[g_sel]);

    uint8_t data[DATA_CAP];
    int fd = open(path, 0);
    if (fd < 0)
        return -1;
    int total = 0;
    for (;;) {
        long r = read(fd, data + total, (unsigned long)(DATA_CAP - (unsigned)total));
        if (r <= 0)
            break;
        total += (int)r;
        if (total >= DATA_CAP)
            break;
    }
    close(fd);
    if (total <= 0)
        return -1;

    int w = 0, h = 0;
    int fmt = px_probe(data, (unsigned)total, &w, &h);
    if (fmt == PX_NONE || w <= 0 || h <= 0)
        return -1;
    /* decoded RGB must fit the windowed task's heap (brk region is
     * 1 MiB, 0x700000..0x800000) — about a window's worth is plenty */
    if (w * h > VGFX_W * VGFX_H)
        return -1;

    uint8_t* rgb = (uint8_t*)malloc((unsigned)(w * h * 3));
    if (!rgb)
        return -1;
    if (px_decode(fmt, data, (unsigned)total, w, h, rgb) != 0) {
        free(rgb);
        return -1;
    }

    free(g_pic_rgb);
    g_pic_rgb = rgb;
    g_pic_w = w;
    g_pic_h = h;
    g_loaded = g_sel;
    return 0;
}

/* Colour-quantise one source pixel of the decoded RGB picture to the
 * nearest Catppuccin palette index. */
static int map_color(int sx, int sy)
{
    int r = g_pic_rgb[(sy * g_pic_w + sx) * 3 + 0];
    int g = g_pic_rgb[(sy * g_pic_w + sx) * 3 + 1];
    int b = g_pic_rgb[(sy * g_pic_w + sx) * 3 + 2];

    int best = 0;
    int bd = 1 << 30;
    for (int c = 0; c < 16; ++c) {
        int dr = (r >> 2) - PAL[c][0];
        int dg = (g >> 2) - PAL[c][1];
        int db = (b >> 2) - PAL[c][2];
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) {
            bd = dd;
            best = c;
        }
    }

    if (g_dither) {
        int e = (r * 77 + g * 150 + b * 29) >> 8; /* brightness 0..254 */
        int th = BAYER[sy & 3][sx & 3];
        if ((e & 15) > th)
            best = (best + 1) & 15;
    }
    return best;
}

static void draw(void)
{
    vgfx_clear(VGFX_BLACK);
    if (g_loaded != g_sel) {
        vgfx_str8((VGFX_W - vgfx_text_width8(g_names[g_sel])) / 2,
                  VGFX_H / 2 - 4, g_names[g_sel], VGFX_WHITE);
        vgfx_flush();
        return;
    }

    int W = VGFX_W, H = VGFX_H;
    if (g_zoom) {
        W = g_pic_w;
        H = g_pic_h;
    }
    /* centre it */
    int ox = (VGFX_W - W) / 2;
    int oy = (VGFX_H - H) / 2;
    if (ox < 0)
        ox = 0;
    if (oy < 0)
        oy = 0;

    for (int y = 0; y < H; ++y) {
        if (y + oy >= VGFX_H)
            break;
        for (int x = 0; x < W; ++x) {
            if (x + ox >= VGFX_W)
                break;
            /* map client pixel back onto the decoded picture */
            int sx = (g_zoom) ? x : (x * g_pic_w) / W;
            int sy = (g_zoom) ? y : (y * g_pic_h) / H;
            if (sx >= g_pic_w)
                sx = g_pic_w - 1;
            if (sy >= g_pic_h)
                sy = g_pic_h - 1;
            vgfx_put_pixel(x + ox, y + oy, map_color(sx, sy));
        }
    }

    char name[NAME_CAP + 8];
    int i = 0;
    while (g_names[g_sel][i] && i < NAME_CAP - 1) {
        name[i] = g_names[g_sel][i];
        ++i;
    }
    name[i++] = ' ';
    name[i] = 0;
    vgfx_str8(4, VGFX_H - 12, name, VGFX_DGRAY);
    vgfx_flush();
}

static void show(void)
{
    if (g_n == 0) {
        vgfx_clear(VGFX_BLACK);
        vgfx_str8((VGFX_W - vgfx_text_width8("empty /pics")) / 2,
                  VGFX_H / 2 - 4, "empty /pics", VGFX_LRED);
        vgfx_flush();
        for (;;) {
            vgfx_event_t ev;
            vgfx_poll(&ev);
            if (ev.type == VGFX_EV_KEY && ev.key == 0x1B)
                break;
        }
        return;
    }
    draw();
    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type != VGFX_EV_KEY)
            continue;
        int k = (int)(unsigned char)ev.key;
        if (k == 0x1B)
            return;
        if (k == '[')
            g_sel = (g_sel - 1 + g_n) % g_n;
        else if (k == ']')
            g_sel = (g_sel + 1) % g_n;
        else if (k == 'z')
            g_zoom = !g_zoom;
        else if (k == 'd')
            g_dither = !g_dither;
        else
            continue;
        load_sel();
        draw();
    }
}

int main(void)
{
    enumerate();
    g_sel = 0;
    load_sel();
    show();
    return 0;
}