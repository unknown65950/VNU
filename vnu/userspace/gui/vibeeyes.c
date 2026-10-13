/*
 * vibeeyes - two eyes that follow the pointer.
 *
 * The old xeyes joke: a pair of eyes that watch the cursor around the
 * screen. What they need is the pointer's position, and that is what
 * the gfx input protocol hands a pixel app as the pointer moves: a
 * move message (vgfx_poll returns VGFX_EV_MOVE) carries the client-area
 * pixel under the cursor. The kernel sends it to the window under the
 * pointer, and - when the pointer is over the desktop or the panel -
 * to the focused window, so the eyes keep tracking even after the
 * cursor has left their window. A position outside the canvas is still
 * a direction, and that is all the eyes look at.
 *
 * The pupils are clamped inside the white so they never leave the eye,
 * and the eyes blink once in a while on the one-second heartbeat every
 * pixel app is given. Esc closes the window.
 */
#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/unistd.h>

static int W = VGFX_W, H = VGFX_H;

/* Where the pointer was last seen, in canvas pixels. Until the first
 * move message the eyes look straight ahead. */
static int look_x, look_y;
static int have_look;

/* The blink: the heartbeat advances the count, and a tick that lands
 * on the mark closes the eyes for that one frame. */
static int tick_count;
static int closed;

/* --- drawing -------------------------------------------------------- */

/* Integer square root: the eyes want a circle, and a freestanding app
 * has neither floating point nor libm. Newton's method on a small
 * positive integer converges in a handful of steps. */
static int isqrt(int v)
{
    if (v <= 0)
        return 0;
    long x = v;
    long y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (int)x;
}

/* One filled ellipse, drawn a scanline at a time; rows with no half
 * width (the very top and bottom) still get their single pixel. */
static void fill_ellipse(int cx, int cy, int rx, int ry, int color)
{
    if (rx < 0)
        rx = 0;
    if (ry <= 0) {
        vgfx_hline(cx, cy, 2 * rx + 1, color);
        return;
    }
    for (int dy = -ry; dy <= ry; ++dy) {
        int hw = rx * isqrt(ry * ry - dy * dy) / ry;
        vgfx_hline(cx - hw, cy + dy, 2 * hw + 1, color);
    }
}

static void draw(void)
{
    vgfx_clear(VGFX_BROWN);

    int rw = W / 5;              /* eye half-width */
    int rh = H / 4;              /* eye half-height */
    if (rw < 10)
        rw = 10;
    if (rh < 10)
        rh = 10;
    int gap = W / 16;
    int left_cx = W / 2 - rw - gap;
    int right_cx = W / 2 + rw + gap;
    int cy = H / 2;

    int pr = rw / 3;             /* pupil radius */
    if (pr < 3)
        pr = 3;
    int ir = pr + 3;             /* iris radius */

    int room_x = rw - 1 - ir;    /* how far the pupil may leave centre */
    int room_y = rh - 1 - ir;
    if (room_x < 0)
        room_x = 0;
    if (room_y < 0)
        room_y = 0;

    for (int e = 0; e < 2; ++e) {
        int cx = e == 0 ? left_cx : right_cx;
        fill_ellipse(cx, cy, rw, rh, VGFX_BLACK);      /* outline */
        fill_ellipse(cx, cy, rw - 1, rh - 1, VGFX_WHITE);

        if (closed) {
            /* A closed eye is the lid coming down over it. */
            vgfx_hline(cx - rw + 1, cy, 2 * rw - 1, VGFX_BLACK);
            continue;
        }

        /* Aim the pupil at the pointer. The direction is normalized
         * and then scaled by the room the pupil has inside the eye, so
         * a pointer beyond the window still reads as a direction. */
        int ox = 0, oy = 0;
        if (have_look) {
            int vx = look_x - cx;
            int vy = look_y - cy;
            int dist = isqrt(vx * vx + vy * vy);
            if (dist > 0) {
                ox = vx * room_x / dist;
                oy = vy * room_y / dist;
            }
        }
        fill_ellipse(cx + ox, cy + oy, ir, ir, VGFX_LBLUE);
        fill_ellipse(cx + ox, cy + oy, pr, pr, VGFX_BLACK);
    }
}

int main(void)
{
    (void)vgfx_canvas(VGFX_W, VGFX_H);
    vgfx_size(&W, &H);
    look_x = W / 2;
    look_y = H / 2;
    vgfx_clear(VGFX_BROWN);
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == VNU_KEY_ESC)
                exit(0);
            if (ev.key == 0x06) { /* the one-second heartbeat */
                ++tick_count;
                closed = (tick_count % 7 == 0);
            } else {
                closed = 0;
            }
        } else if (ev.type == VGFX_EV_MOVE) {
            look_x = ev.x;
            look_y = ev.y;
            have_look = 1;
            closed = 0;
        } else if (ev.type == VGFX_EV_RESIZE) {
            vgfx_size(&W, &H);
        }
        draw();
        vgfx_flush();
    }
}
