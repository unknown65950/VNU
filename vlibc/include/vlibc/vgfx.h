/*
 * vgfx — pixel framebuffer API for VNU gfx-windowed apps.
 *
 * The app renders into a 240x146 pixel buffer and flushes it to
 * the kernel's gfx surface (fd 3).  Mouse clicks over the client
 * area arrive as an escape stream on stdin (fd 0) which vgfx_poll
 * parses into easy-to-use event structs.
 *
 * Protocol (kernel → userspace, on fd 0):
 *   ESC '[' 'M' <button> <px> <py>
 *   <button>: 1 = left press, 2 = left released
 *   <px>     : 0..239
 *   <py>     : 0..169
 *
 * Keyboard characters arrive as a single byte (no ESC prefix), except
 * Esc (0x1B) which is wrapped as button 3 so the parser never stalls
 * waiting for a message that isn't coming.
 */
#pragma once

#define VGFX_W 240
#define VGFX_H 170
#define VGFX_FD 3

/* Standard VGA 16-color palette indices (same as text-mode attributes). */
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

typedef struct {
    int type;     /* VGFX_EV_* */
    int x, y;    /* valid for PRESS / RELEASE */
    int button;  /* always 1 (left) for now */
    char key;    /* valid for KEY */
} vgfx_event_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Draw primitives (into the in-process framebuffer). */
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

/* Flush the in-process framebuffer to the kernel's gfx surface. */
void vgfx_flush(void);

/* Blocks until a keyboard or mouse event is ready, then fills `ev`
 * and returns 1.  While blocked it cooperatively yields back to the
 * GUI (windowed tasks have no O_NONBLOCK), so the caller can simply
 * loop on it.  Internally assembles the 6-byte mouse messages
 * incrementally so partial reads never produce garbage. */
int  vgfx_poll(vgfx_event_t* ev);

#ifdef __cplusplus
}
#endif
