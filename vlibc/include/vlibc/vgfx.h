/*
 * vgfx — pixel framebuffer API for VNU gfx-windowed apps.
 *
 * The app draws straight into its window's canvas: 480x340 pixels of
 * memory the kernel shares with it (gfx_surface(2)), which the
 * compositor blits on its next pass. A pixel is one byte on an 8bpp
 * display (a palette slot) and four on a 32bpp one (0xXXRRGGBB, the
 * frame's own B8G8R8X8 words), and the canvas is in the depth of the
 * display the program is on - vgfx_get_info() says which, and vgfx
 * asks the kernel for it, so a program does not have to know.  Mouse clicks over the client
 * area arrive as an escape stream on stdin (fd 0) which vgfx_poll
 * parses into easy-to-use event structs.
 *
 * Protocol (kernel → userspace, on fd 0):
 *   ESC '[' 'M' <button> <xl> <xh> <yl> <yh>
 *   <button>: 1 = left press, 2 = left released, 3 = Esc key,
 *             4 = drag cancelled (the click turned into a drag and no
 *                 release will ever come for it)
 *   <xl>/<xh>, <yl>/<yh>: little-endian 16-bit client-area pixel
 *   coordinates (0..479 / 0..339).
 *   ESC '[' 'D' <len_lo> <len_hi> <path...> — drag-and-drop: the GUI
 *   dropped the file at <path> (full VFS path, little-endian 16-bit
 *   length) onto this window.
 *
 * Keyboard characters arrive as a single byte (no ESC prefix), except
 * Esc (0x1B) which is wrapped as button 3 so the parser never stalls
 * waiting for a message that isn't coming.
 */
#pragma once

/* 480x340 is a native 8x16 text grid (60 cols x 21 rows), so a gfx
 * app's text lands at the same physical size as every console window —
 * the GUI displays this canvas 1:1. The *size* of a pixel in bytes is
 * the display's, not this header's: see the note at the top. */
#define VGFX_W 480
#define VGFX_H 340

#include <vnu/abi.h> /* struct vnu_gfx_info, VNU_GFX_DRIVER_* */

/* Palette indices (VGA layout: 0..7 dark-slot names, 8..15 bright-slot
 * names). The actual RGB values are not fixed: the kernel programs these
 * 16 DAC entries to the Catppuccin Mocha palette on entering graphics
 * mode, so apps should pick by role (surfaces = LGRAY/DGRAY, highlights
 * = LBLUE/LCYAN, text = WHITE/BLACK, accents = BLUE/GREEN/YELLOW/...) */
#define VGFX_BLACK   0
#define VGFX_BLUE    1
#define VGFX_GREEN   2
#define VGFX_CYAN    3
#define VGFX_RED     4
#define VGFX_MAGENTA 5
#define VGFX_BROWN   6
#define VGFX_LGRAY   7
#define VGFX_DGRAY   8
#define VGFX_LBLUE   9
#define VGFX_LGREEN 10
#define VGFX_LCYAN  11
#define VGFX_LRED   12
#define VGFX_LMAGENTA 13
#define VGFX_YELLOW 14
#define VGFX_WHITE  15

#define VGFX_EV_NONE    0
#define VGFX_EV_KEY     1
#define VGFX_EV_PRESS   2
#define VGFX_EV_RELEASE 3
#define VGFX_EV_DROP    4
#define VGFX_EV_DRAG_CANCEL 5

typedef struct {
    int type;     /* VGFX_EV_* */
    int x, y;    /* valid for PRESS / RELEASE */
    int button;  /* 1 = left press, 2 = release, 4 = drag cancelled */
    /* Unsigned on purpose: the kernel normalises the PS/2 scancodes for
     * the arrows into 0x81..0x87 (see keys.h), and a signed char would
     * make every comparison against them false. */
    unsigned char key; /* valid for KEY */
    const char* drop; /* valid for DROP: full VFS path delivered onto us */
} vgfx_event_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Draw primitives (into the in-process framebuffer). Colours are the
 * VGFX_* palette slots above, and they mean the same thing on either
 * depth: the kernel owns the palette (Catppuccin Mocha, programmed when
 * the desktop started) and vgfx asks it for the table, so a program
 * never hardcodes an RGB value to get a colour it wants. For a colour
 * that is not one of the sixteen, see vgfx_put_rgb below. */
void vgfx_clear(int color);
void vgfx_put_pixel(int x, int y, int color);
void vgfx_fill_rect(int x, int y, int w, int h, int color);
void vgfx_rect(int x, int y, int w, int h, int color);
void vgfx_hline(int x, int y, int w, int color);
void vgfx_vline(int x, int y, int h, int color);
void vgfx_char(int x, int y, char ch, int color);
void vgfx_str(int x, int y, const char* s, int color);
int  vgfx_text_width(const char* s);

/* compact 8x8 face for dense UI (panel, lists, prefs) */
void vgfx_char8(int x, int y, char ch, int color);
void vgfx_str8(int x, int y, const char* s, int color);
int  vgfx_text_width8(const char* s);

/* True colour, and the reason the canvas is in the display's own depth.
 * `rgb` is 0x00RRGGBB, `a` is coverage 0..255.
 *
 * On a 32bpp display both go into the pixel as they are and the
 * compositor blends the alpha over the desktop, so a decoded picture
 * with a transparent background is that background. On an 8bpp display
 * there is no such channel: the colour is quantized down to the nearest
 * of the sixteen slots and the alpha is dropped, which is the whole of
 * what the palette costs. */
void vgfx_put_rgb(int x, int y, unsigned int rgb);
void vgfx_put_rgba(int x, int y, unsigned int rgb, unsigned int a);
/* The same, dithered: a 4x4 Bayer against the palette, so a gradient
 * that an 8bpp display could only show in 16 steps comes out as a
 * dither instead of as bands. A no-op on a 32bpp display, where there is
 * nothing left to dither, so a program can ask for it unconditionally
 * rather than having to know the depth. */
void vgfx_put_rgb_dither(int x, int y, unsigned int rgb);

/* Ends a frame. Nothing has to be pushed - the pixels are already in
 * the shared canvas the compositor reads - so this only exists because
 * it reads well at the end of a draw call. */
void vgfx_flush(void);

/* Blocks until a keyboard or mouse event is ready, then fills `ev`
 * and returns 1.  While blocked it cooperatively yields back to the
 * GUI (windowed tasks have no O_NONBLOCK), so the caller can simply
 * loop on it.  Internally assembles the 6-byte mouse messages
 * incrementally so partial reads never produce garbage. */
int  vgfx_poll(vgfx_event_t* ev);

/* Announces to the GUI that the item under the current press is a
 * draggable file at the given full VFS path. The window manager uses
 * this to decide whether the press turns into a drag-and-drop (files.c
 * calls it with the pressed entry on every PRESS over a file). Safe to
 * call from any task; console tasks are a no-op. */
int  vnu_dnd_declare(const char* path);

/* The display mode, in pixels. Both drivers answer the same, whether the
 * mode was reached through vgfx_set_resolution(), the desktop's F12 key
 * or /etc/vnuconfig/gfx.conf on the next boot - so a program can ask
 * where it stands instead of assuming the compile-time default. */
void vgfx_get_resolution(int* w, int* h);

/* The whole answer in one read: the mode in use, its depth in bits per
 * pixel and the driver that owns the display (VNU_GFX_DRIVER_VGA or
 * VNU_GFX_DRIVER_VIRTIO_GPU). Answers the same way whether the mode was
 * reached through vgfx_set_resolution(), the desktop's F12 key or
 * /etc/vnuconfig/gfx.conf, so a program that needs the depth does not
 * have to parse /proc/gfx to get it. Returns 0, or -1 with `info`
 * untouched if the kernel is too old to answer. */
int  vgfx_get_info(struct vnu_gfx_info* info);

/* Put the display into w x h. Only the sizes this build supports are
 * accepted (the driver's ladder: 640x480, 800x600, 1024x768, 1280x1024);
 * anything else returns 0. Windows are moved back inside the new screen
 * rather than closed, the wallpaper is re-stretched for it, and the
 * choice is recorded in /etc/vnuconfig/gfx.conf. Either pointer of
 * vgfx_get_resolution() may be null. */
int  vgfx_set_resolution(int w, int h);

#ifdef __cplusplus
}
#endif
