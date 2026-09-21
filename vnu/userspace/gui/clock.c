/*
 * clock — analog clock, stopwatch and countdown timer for the VNU desktop.
 *
 * A from-scratch C rewrite of the classic clock-tui (analog face + a
 * stopwatch + a timer) as a VibeGraphics window app. It renders its own
 * 480x340 pixel framebuffer through the vgfx library and reads mouse and
 * keyboard events off stdin, exactly like the other desktop apps.
 *
 * Three modes, switchable with the 1/2/3 keys or the tab buttons:
 *   Clock     — analog face driven by the `time` syscall (RTC, 1 s
 *               resolution); the seconds hand jumps once per heartbeat.
 *   Stopwatch — counts up from zero, with start/pause, lap and reset.
 *   Timer     — counts down from a configurable duration, with
 *               start/pause/reset and a flashing "TIME'S UP" when done.
 *
 * The GUI kernel feeds a per-second TICK_BYTE (0x06) heartbeat to every
 * gfx window, so the hands and readings advance even while the window is
 * idle. Esc closes the window (the desktop respawns nothing: the app was
 * spawned directly from an icon, not exec'd by a shell).
 *
 * Keys: 1/2/3 mode, Space start/pause, r reset, l lap, Esc close.
 */
#include <vlibc/vgfx.h>
#include <vlibc/vgfx_font.h>
#include <vlibc/unistd.h>
#include <vlibc/time.h>
#include <vlibc/string.h>

#define TICK_BYTE 0x06 /* kernel GUI heartbeat, see vnu/wintask.h */

/* --- geometry --------------------------------------------------------- */

#define TAB_Y 8
#define TAB_H 22
#define TAB_W 150
#define TAB_GAP 6
#define TAB_X0 8

#define CX 240              /* clock face centre */
#define CY 170
#define FACE_R 88

#define BTN_Y 300           /* bottom button row */
#define BTN_H 18
#define STATUS_Y (VGFX_H - 17)  /* dark status strip, like files/prefs */

/* --- palette roles (Catppuccin Mocha lives in the 16 VGA slots) ------- */

#define C_BG     VGFX_DGRAY
#define C_SURF   VGFX_LGRAY
#define C_TXT    VGFX_WHITE
#define C_ACCENT VGFX_LBLUE
#define C_HOUR   VGFX_BLACK
#define C_MIN    VGFX_BLACK
#define C_SEC    VGFX_LRED
#define C_DONE   VGFX_RED

/* --- basic integer graphics ------------------------------------------- */

/* sin/cos lookup for 0..90 degrees, result scaled by 1000 (integer-only,
 * vlibc has no libm). */
static const short SCT[91] = {
    0, 17, 35, 52, 70, 87, 105, 122, 139, 156, 174, 191, 208, 225, 242,
    259, 276, 292, 309, 326, 342, 358, 375, 391, 407, 423, 438, 454,
    469, 485, 500, 515, 530, 545, 559, 574, 588, 602, 616, 629, 643,
    656, 669, 682, 695, 707, 719, 731, 743, 755, 766, 777, 788, 799,
    809, 819, 829, 839, 848, 857, 866, 875, 883, 891, 899, 906, 914,
    921, 927, 934, 940, 946, 951, 956, 961, 966, 970, 974, 978, 982,
    985, 988, 990, 993, 995, 996, 998, 999, 999, 1000, 1000,
};

static int trg(int deg, int want_sin)
{
    deg %= 360;
    if (deg < 0)
        deg += 360;
    int q = deg / 90, r = deg % 90;
    switch (q) {
    case 0:  return want_sin ? SCT[r]      : SCT[90 - r];
    case 1:  return want_sin ? SCT[90 - r] : -SCT[r];
    case 2:  return want_sin ? -SCT[r]     : -SCT[90 - r];
    default: return want_sin ? -SCT[90 - r] : SCT[r];
    }
}

static int positive(int v)
{
    return v < 0 ? -v : v;
}

static void line(int x0, int y0, int x1, int y1, int c)
{
    int dx = positive(x1 - x0), dy = positive(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        vgfx_put_pixel(x0, y0, c);
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

static void circle_outline(int cx0, int cy0, int r, int c)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        vgfx_put_pixel(cx0 + x, cy0 + y, c);
        vgfx_put_pixel(cx0 + y, cy0 + x, c);
        vgfx_put_pixel(cx0 - x, cy0 + y, c);
        vgfx_put_pixel(cx0 - y, cy0 + x, c);
        vgfx_put_pixel(cx0 + x, cy0 - y, c);
        vgfx_put_pixel(cx0 + y, cy0 - x, c);
        vgfx_put_pixel(cx0 - x, cy0 - y, c);
        vgfx_put_pixel(cx0 - y, cy0 - x, c);
        if (d < 0)
            d += 4 * x + 6;
        else {
            d += 4 * (x - y) + 10;
            --y;
        }
        ++x;
    }
}

static void disc(int cx0, int cy0, int r, int c)
{
    for (int yy = -r; yy <= r; ++yy)
        for (int xx = -r; xx <= r; ++xx)
            if (xx * xx + yy * yy <= r * r)
                vgfx_put_pixel(cx0 + xx, cy0 + yy, c);
}

/* --- text -------------------------------------------------------------- */

/* 2x-scaled 8x16 face: big, readable digital readout. */
static void big_char(int x, int y, char ch, int c)
{
    const unsigned char* g = VGFX_FONT[(unsigned char)ch];
    for (int r = 0; r < 16; ++r)
        for (int col = 0; col < 8; ++col)
            if (g[r] & (0x80 >> col))
                vgfx_fill_rect(x + col * 2, y + r * 2, 2, 2, c);
}

static void big_str(int x, int y, const char* s, int c)
{
    while (s && *s) {
        big_char(x, y, *s, c);
        x += 16;
        ++s;
    }
}

static int big_width(const char* s)
{
    int n = 0;
    while (s && s[n])
        ++n;
    return n * 16;
}

static void fmt2(char* out, int v)
{
    out[0] = (char)('0' + v / 10 % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

/* hh:mm:ss from a seconds value. */
static void fmt_hms(char* out, int sec)
{
    char b0[4], b1[4], b2[4];
    fmt2(b0, sec / 3600);
    fmt2(b1, sec % 3600 / 60);
    fmt2(b2, sec % 60);
    out[0] = b0[0]; out[1] = b0[1]; out[2] = ':';
    out[3] = b1[0]; out[4] = b1[1]; out[5] = ':';
    out[6] = b2[0]; out[7] = b2[1];
    out[8] = 0;
}

/* --- mode state -------------------------------------------------------- */

enum Mode { M_CLOCK, M_STOP, M_TIMER };
static enum Mode mode = M_CLOCK;

/* per-mode label + accent, for the tabs and the status strip */
static const char* MODE_NAME[3] = { "clock", "stopwatch", "timer" };
static const int MODE_ACCENT[3] = { VGFX_YELLOW, VGFX_LGREEN, VGFX_LRED };

/* stopwatch */
static int sw_running = 0;
static int sw_accum = 0;       /* whole seconds banked while paused */
static unsigned long sw_base;  /* uptime when running was last started */
static int lap_sec[8];
static int lap_n = 0;

/* timer */
static int tm_set = 60;        /* configured duration in seconds */
static int tm_remain = 60;     /* shown/remaining value */
static int tm_running = 0;
static int tm_accum = 0;
static unsigned long tm_base;
static int tm_done = 0;

/* --- buttons ----------------------------------------------------------- */

static void draw_button(int x, int y, int w, int h, const char* label, int active)
{
    vgfx_fill_rect(x, y, w, h, active ? C_ACCENT : C_SURF);
    vgfx_rect(x, y, w, h, VGFX_BLACK);
    int tw = vgfx_text_width(label);
    vgfx_str(x + (w - tw) / 2, y + (h - 16) / 2, label, active ? C_TXT : C_HOUR);
}

static void draw_tabs(void)
{
    static const char* names[3] = { "Clock", "Stopwatch", "Timer" };
    for (int i = 0; i < 3; ++i) {
        int x = TAB_X0 + i * (TAB_W + TAB_GAP);
        draw_button(x, TAB_Y, TAB_W, TAB_H, names[i], (int)mode == i);
    }
}

/* --- Clock face -------------------------------------------------------- */

static void tick_on(int deg, int in_len, int out_len, int c)
{
    int x0 = CX + (FACE_R - in_len) * trg(deg, 1) / 1000;
    int y0 = CY - (FACE_R - in_len) * trg(deg, 0) / 1000;
    int x1 = CX + (FACE_R - out_len) * trg(deg, 1) / 1000;
    int y1 = CY - (FACE_R - out_len) * trg(deg, 0) / 1000;
    line(x0, y0, x1, y1, c);
}

static void draw_clock_face(int h, int m, int s)
{
    vgfx_clear(C_BG);
    disc(CX, CY, FACE_R, C_SURF);
    circle_outline(CX, CY, FACE_R, VGFX_BLACK);

    /* 60 ticks: bare stub marks, with a longer one every 5 seconds. */
    for (int i = 0; i < 60; ++i)
        tick_on(i * 6, 6, (i % 5 == 0) ? 14 : 9, i % 5 == 0 ? VGFX_BLACK : C_BG);

    /* hour numerals 12 / 3 / 6 / 9 in the compact 8x8 face. */
    vgfx_str8(CX - 8, CY - FACE_R + 6, "12", C_TXT);
    vgfx_str8(CX + FACE_R - 12, CY - 4, "3", C_TXT);
    vgfx_str8(CX - 8, CY + FACE_R - 16, "6", C_TXT);
    vgfx_str8(CX - FACE_R + 4, CY - 4, "9", C_TXT);

    /* hands: hour (thick), minute, second. Deg from 12 o'clock. */
    int hdeg = (h % 12) * 30 + m / 2;
    int mdeg = m * 6 + s / 10;
    int sdeg = s * 6;
    int hx = CX + 44 * trg(hdeg, 1) / 1000;
    int hy = CY - 44 * trg(hdeg, 0) / 1000;
    line(CX, CY, hx, hy, C_HOUR);
    line(CX + 1, CY, hx + 1, hy, C_HOUR);
    int mx = CX + 68 * trg(mdeg, 1) / 1000;
    int my = CY - 68 * trg(mdeg, 0) / 1000;
    line(CX, CY, mx, my, C_MIN);
    int sx = CX + 78 * trg(sdeg, 1) / 1000;
    int sy = CY - 78 * trg(sdeg, 0) / 1000;
    line(CX, CY, sx, sy, C_SEC);

    disc(CX, CY, 4, VGFX_BLACK);
    disc(CX, CY, 2, C_SEC);
}

/* --- Stopwatch --------------------------------------------------------- */

static int sw_now(void)
{
    int el = sw_accum;
    if (!sw_running)
        return el;
    unsigned long d = uptime() - sw_base;
    if (d > 100000000ul) /* midnight wrap: drop this sample */
        return el;
    el += (int)d;
    return el;
}

static void draw_stopwatch(void)
{
    vgfx_clear(C_BG);
    int el = sw_now();

    char buf[16];
    fmt_hms(buf, el);
    big_str((VGFX_W - big_width(buf)) / 2, 120, buf, sw_running ? C_ACCENT : C_TXT);

    /* laps, newest first, up to 8. */
    if (lap_n > 0) {
        vgfx_str8(40, 170, "laps", VGFX_BLACK);
        int y = 186;
        for (int i = lap_n - 1; i >= 0 && y < 284; --i) {
            char lb[16] = { 'L', ' ', 0, ':', ' ', '0', '0', ':', '0', '0', 0, 0, 0, 0, 0, 0 };
            int n = lap_n - i;
            fmt2(lb + 2, n);
            fmt2(lb + 6, lap_sec[i]);
            vgfx_str8(40, y, lb, C_TXT);
            y += 16;
        }
    }

    draw_button(32,  BTN_Y, 130, BTN_H, sw_running ? "Pause" : "Start", sw_running);
    draw_button(175, BTN_Y, 130, BTN_H, "Lap",    sw_running);
    draw_button(318, BTN_Y, 130, BTN_H, "Reset",  0);
}

/* --- Timer ------------------------------------------------------------- */

static int tm_now(void)
{
    if (!tm_running)
        return tm_remain;
    unsigned long d = uptime() - tm_base;
    if (d > 100000000ul)
        d = 0;
    int left = tm_set - tm_accum - (int)d;
    return left < 0 ? 0 : left;
}

static void draw_timer(void)
{
    vgfx_clear(C_BG);

    /* duration presets: shown always, but only tweakable while idle. */
    draw_button(40,  78, 90, 18, "-1m",  0);
    draw_button(138, 78, 90, 18, "-10s", 0);
    draw_button(236, 78, 90, 18, "+10s", 0);
    draw_button(334, 78, 90, 18, "+1m",  0);

    int rem = tm_done ? 0 : tm_now();
    char buf[16];
    fmt_hms(buf, rem);
    int c = C_TXT;
    if (tm_done)
        c = C_DONE;
    else if (rem <= 10 && tm_running)
        c = VGFX_YELLOW;
    big_str((VGFX_W - big_width(buf)) / 2, 160, buf, c);
    if (tm_done)
        big_str((VGFX_W - big_width("TIME'S UP")) / 2, 120, "TIME'S UP", C_DONE);

    draw_button(105, BTN_Y, 130, BTN_H, tm_running ? "Pause" : "Start", tm_running || tm_done);
    draw_button(245, BTN_Y, 130, BTN_H, "Reset", 0);
}

/* --- drawing + hit-testing --------------------------------------------- */

/* Advance time-based state; called on every redraw (each heartbeat). */
static void tick(void)
{
    if (tm_running && tm_now() == 0) {
        tm_running = 0;
        tm_done = 1;
        tm_accum = 0;
        tm_remain = 0;
    }
}

static void draw(void)
{
    tick();
    switch (mode) {
    case M_CLOCK: {
        long t = time(NULL);
        draw_clock_face((int)(t / 3600) % 24, (int)(t % 3600 / 60), (int)(t % 60));
        char db[16];
        fmt_hms(db, (int)t);
        int xb = (VGFX_W - big_width(db)) / 2;
        vgfx_fill_rect(xb - 4, 262, big_width(db) + 8, 34, C_BG);
        big_str(xb, 262, db, C_TXT);
        break;
    }
    case M_STOP:
        draw_stopwatch();
        break;
    case M_TIMER:
        draw_timer();
        break;
    }
    draw_tabs();

    /* dark status strip, same flat house style as files/prefs */
    vgfx_fill_rect(0, STATUS_Y, VGFX_W, VGFX_H - STATUS_Y, VGFX_DGRAY);
    vgfx_hline(0, STATUS_Y, VGFX_W, VGFX_BLACK);
    vgfx_str(4, STATUS_Y + 1, MODE_NAME[(int)mode], MODE_ACCENT[(int)mode]);

    const char* hint = mode == M_CLOCK ? "1/2/3 mode   Esc close"
                     : mode == M_STOP  ? "1/2/3 mode   Space start/pause   r reset   l lap"
                                       : "1/2/3 mode   Space start/pause   r reset";
    vgfx_str(VGFX_W - 4 - vgfx_text_width(hint), STATUS_Y + 1, hint, VGFX_WHITE);
}

/* action ids for clicks (something that maps back to handle_action). */
enum { A_CLOCK = 1, A_STOP, A_TIMER,
       A_SW_START = 10, A_SW_LAP, A_SW_RESET,
       A_TM_M1 = 20, A_TM_S10, A_TM_P10, A_TM_P1, A_TM_START, A_TM_RESET };

static int hit_action(int px, int py)
{
    /* mode tabs */
    for (int i = 0; i < 3; ++i) {
        int x = TAB_X0 + i * (TAB_W + TAB_GAP);
        if (px >= x && px < x + TAB_W && py >= TAB_Y && py < TAB_Y + TAB_H)
            return A_CLOCK + i;
    }
    if (py < BTN_Y || py >= BTN_Y + BTN_H)
        return 0;
    switch (mode) {
    case M_STOP:
        if (px >= 32 && px < 162)  return A_SW_START;
        if (px >= 175 && px < 305) return A_SW_LAP;
        if (px >= 318 && px < 448) return A_SW_RESET;
        break;
    case M_TIMER:
        if (py >= 78 && py < 96) {
            if (px >= 40 && px < 130)  return A_TM_M1;
            if (px >= 138 && px < 228) return A_TM_S10;
            if (px >= 236 && px < 326) return A_TM_P10;
            if (px >= 334 && px < 424) return A_TM_P1;
        }
        if (px >= 105 && px < 235) return A_TM_START;
        if (px >= 245 && px < 375) return A_TM_RESET;
        break;
    default:
        break;
    }
    return 0;
}

/* --- actions ----------------------------------------------------------- */

static void handle_action(int a)
{
    switch (a) {
    case A_CLOCK: mode = M_CLOCK; break;
    case A_STOP:  mode = M_STOP; break;
    case A_TIMER: mode = M_TIMER; break;

    case A_SW_START:
        if (sw_running) {
            sw_accum = sw_now();
            sw_running = 0;
        } else {
            sw_accum = sw_now();
            sw_base = uptime();
            sw_running = 1;
        }
        break;
    case A_SW_LAP:
        if (sw_running && lap_n < 8)
            lap_sec[lap_n++] = sw_now();
        break;
    case A_SW_RESET:
        sw_running = 0;
        sw_accum = 0;
        lap_n = 0;
        break;

    case A_TM_M1:
        if (!tm_running && !tm_done) {
            tm_set -= 60;
            if (tm_set < 1) tm_set = 1;
            tm_remain = tm_set;
        }
        break;
    case A_TM_S10:
        if (!tm_running && !tm_done) {
            tm_set -= 10;
            if (tm_set < 1) tm_set = 1;
            tm_remain = tm_set;
        }
        break;
    case A_TM_P10:
        if (!tm_running && !tm_done) {
            tm_set += 10;
            if (tm_set > 86399) tm_set = 86399;
            tm_remain = tm_set;
        }
        break;
    case A_TM_P1:
        if (!tm_running && !tm_done) {
            tm_set += 60;
            if (tm_set > 86399) tm_set = 86399;
            tm_remain = tm_set;
        }
        break;
    case A_TM_START:
        if (tm_done) {
            tm_done = 0;
            tm_remain = 0; /* restart from zero after it fired */;
        }
        if (tm_running) {
            tm_remain = tm_now();
            tm_accum = tm_set - tm_remain;
            tm_running = 0;
        } else {
            tm_base = uptime();
            tm_running = 1;
        }
        break;
    case A_TM_RESET:
        tm_running = 0;
        tm_done = 0;
        tm_accum = 0;
        tm_remain = tm_set;
        break;
    }
}

static void key_action(char k)
{
    if (k == '1') mode = M_CLOCK;
    else if (k == '2') mode = M_STOP;
    else if (k == '3') mode = M_TIMER;
    else if (k == ' ') handle_action(mode == M_STOP ? A_SW_START : A_TM_START);
    else if (k == 'r' || k == 'R') handle_action(mode == M_STOP ? A_SW_RESET : A_TM_RESET);
    else if (k == 'l' || k == 'L') handle_action(A_SW_LAP);
}

int main(void)
{
    draw();
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == 0x1B) {
                exit(0); /* Esc closes the window */
            }
            if (TICK_BYTE == ev.key)
                ; /* just a second tick: redraw below with fresh time */
            else
                key_action(ev.key);
        } else if (ev.type == VGFX_EV_PRESS) {
            int a = hit_action(ev.x, ev.y);
            if (a)
                handle_action(a);
        }
        draw();
        vgfx_flush();
    }
}