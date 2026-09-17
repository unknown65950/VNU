/*
 * prefs — VibeGraphics "System Preferences", styled after the
 * NeXTSTEP / GNUstep preferences panel.
 *
 * A window split into a narrow selector column on the left (a vertical
 * list of preference panes, the selected one highlighted) and a
 * content area on the right where the chosen pane renders its fields.
 * All text uses the compact 8x8 UI face so the 240x170 client fits a
 * real settings page.
 *
 * Panes:
 *   About  — OS identification (utsname) + /proc/version
 *   Memory — live totals read from /proc/meminfo and /proc/uptime
 *   Mounts — mounted filesystems from /proc/mounts
 *   Info   — /proc/cpuinfo snippet
 *
 * Keys: Esc (close the window), Up/Down (change pane), Enter (activate
 * the pane).
 */
#include <vlibc/vgfx.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/sys/utsname.h>

#define SELECTOR_W 66
#define PANE_ROWS 4

static const char* pane_names[PANE_ROWS] = {
    "About", "Memory", "Mounts", "Info",
};
static int cur_pane = 0;
static int hover_pane = -1;

/* Read a whole /proc file (its size is known up front) into buf.
 * Returns length or -1. */
static int read_proc(const char* path, char* buf, int cap)
{
    int fd = open(path, 0);
    if (fd < 0)
        return -1;
    int n = (int)read(fd, buf, (unsigned long)(cap - 1));
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

#define CONTENT_X (SELECTOR_W + 6)

static int draw_about(void)
{
    struct utsname u;
    if (uname(&u) != 0)
        return 0;
    vgfx_str8(CONTENT_X, 4, "VNU/VibeGraphics", VGFX_BLACK);
    vgfx_str8(CONTENT_X, 14, u.sysname, VGFX_DGRAY);
    vgfx_str8(CONTENT_X, 24, u.release, VGFX_DGRAY);
    char v[128];
    int n = read_proc("/proc/version", v, sizeof(v));
    if (n > 0) {
        /* keep just the first line if it wraps */
        for (int i = 0; v[i]; ++i)
            if (v[i] == '\n') {
                v[i] = 0;
                break;
            }
        vgfx_str8(CONTENT_X, 40, "version:", VGFX_BLACK);
        vgfx_str8(CONTENT_X, 50, v, VGFX_BLUE);
    }
    vgfx_str8(CONTENT_X, 66, "machine:", VGFX_BLACK);
    vgfx_str8(CONTENT_X, 76, u.machine, VGFX_BLUE);
    return 1;
}

static int draw_memory(void)
{
    char mem[256];
    int n = read_proc("/proc/meminfo", mem, sizeof(mem));
    if (n < 0)
        return 0;
    vgfx_str8(CONTENT_X, 4, "Memory", VGFX_BLACK);
    int y = 18, line = 0;
    for (int i = 0; mem[i] && y < VGFX_H - 4; ++i) {
        if (mem[i] == '\n') {
            mem[i] = 0;
            vgfx_str8(CONTENT_X + 2, y, mem + line, VGFX_DGRAY);
            y += 12;
            line = i + 1;
        }
    }
    if (line && mem[line] == 0) {
        vgfx_str8(CONTENT_X + 2, y, mem + line, VGFX_DGRAY);
        y += 12;
    }
    char up[128];
    int un = read_proc("/proc/uptime", up, sizeof(up));
    if (un > 0) {
        for (int i = 0; up[i]; ++i)
            if (up[i] == '\n') {
                up[i] = ' ';
                break;
            }
        vgfx_str8(CONTENT_X, y + 2, "uptime:", VGFX_BLACK);
        vgfx_str8(CONTENT_X, y + 14, up, VGFX_BLUE);
    }
    return 1;
}

static int draw_mounts(void)
{
    char m[256];
    int n = read_proc("/proc/mounts", m, sizeof(m));
    if (n < 0)
        return 0;
    vgfx_str8(CONTENT_X, 4, "Mount points", VGFX_BLACK);
    int y = 18, line = 0;
    for (int i = 0; m[i] && y < VGFX_H - 4; ++i) {
        if (m[i] == '\n') {
            m[i] = 0;
            vgfx_str8(CONTENT_X, y, m + line, VGFX_DGRAY);
            y += 10;
            line = i + 1;
        }
    }
    if (line && m[line] == 0)
        vgfx_str8(CONTENT_X, y, m + line, VGFX_DGRAY);
    return 1;
}

static int draw_info(void)
{
    char c[192];
    int n = read_proc("/proc/cpuinfo", c, sizeof(c));
    if (n < 0)
        return 0;
    vgfx_str8(CONTENT_X, 4, "CPU", VGFX_BLACK);
    int y = 18, line = 0;
    for (int i = 0; c[i] && y < VGFX_H - 4; ++i) {
        if (c[i] == '\n') {
            c[i] = 0;
            vgfx_str8(CONTENT_X, y, c + line, VGFX_DGRAY);
            y += 9;
            line = i + 1;
        }
    }
    if (line && c[line] == 0)
        vgfx_str8(CONTENT_X, y, c + line, VGFX_DGRAY);
    return 1;
}

/* Pancake left selector + right content */
static void draw(void)
{
    vgfx_clear(VGFX_LGRAY);

    /* selector column */
    vgfx_fill_rect(0, 0, SELECTOR_W, VGFX_H, VGFX_LBLUE);
    vgfx_vline(SELECTOR_W - 1, 0, VGFX_H, VGFX_BLACK);
    for (int i = 0; i < PANE_ROWS; ++i) {
        int y = 2 + i * 26;
        int is_sel = i == cur_pane;
        int is_hov = i == hover_pane;
        if (is_sel)
            vgfx_fill_rect(1, y, SELECTOR_W - 2, 24, VGFX_BLUE);
        else if (is_hov)
            vgfx_fill_rect(1, y, SELECTOR_W - 2, 24, VGFX_CYAN);
        int col = is_sel ? VGFX_WHITE : VGFX_BLACK;
        /* small pane glyph: a rounded square emulating the NeXT tile */
        vgfx_fill_rect(5, y + 3, 8, 8, is_sel ? VGFX_WHITE : VGFX_DGRAY);
        vgfx_rect(5, y + 3, 8, 8, VGFX_BLACK);
        vgfx_str8(17, y + 4, pane_names[i], col);
    }

    /* content area */
    int ok = 1;
    switch (cur_pane) {
    case 0: ok = draw_about(); break;
    case 1: ok = draw_memory(); break;
    case 2: ok = draw_mounts(); break;
    default: ok = draw_info(); break;
    }
    if (!ok) {
        vgfx_str8(SELECTOR_W + 6, 4, "pane unavailable", VGFX_RED);
    }

    /* status strip */
    int sy = VGFX_H - 10;
    vgfx_fill_rect(0, sy, VGFX_W, 10, VGFX_DGRAY);
    vgfx_hline(0, sy, VGFX_W, VGFX_BLACK);
    vgfx_str8(4, sy + 1, "System Preferences", VGFX_WHITE);
}

static int sel_hit(int px, int py)
{
    if (px < 0 || px >= SELECTOR_W)
        return -1;
    int i = (py - 2) / 26;
    if (i < 0 || i >= PANE_ROWS)
        return -1;
    return py - 2 < i * 26 + 24 ? i : -1;
}

int main(void)
{
    draw();
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == 0x1B)
                exit(0);
            if (ev.key == 'k' || ev.key == 'K')
                cur_pane = (cur_pane - 1 + PANE_ROWS) % PANE_ROWS;
            else if (ev.key == 'j' || ev.key == 'J')
                cur_pane = (cur_pane + 1) % PANE_ROWS;
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_PRESS) {
            hover_pane = sel_hit(ev.x, ev.y);
            if (hover_pane >= 0)
                cur_pane = hover_pane;
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_RELEASE) {
            hover_pane = -1;
            draw();
            vgfx_flush();
        }
    }
}