/*
 * prefs — VNU system preferences.
 *
 * A window split into a narrow pane selector (left) and a content area
 * (right), all in the crisp 8x16 VGA face. Every pane draws as a
 * label/value table under an accent heading line; the selected pane
 * gets a full-width light-blue bar and its tile wears the pane accent
 * colour.
 *
 * Panes:
 *   About  — OS identification (utsname) + /proc/version
 *   Memory — live totals from /proc/meminfo and /proc/uptime
 *   Mounts — mounted filesystems from /proc/mounts
 *   CPU    — /proc/cpuinfo snippet
 *
 * Keys: j/k switch pane, Esc closes. Mouse: click a pane to switch.
 */
#include <vlibc/vgfx.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/sys/utsname.h>

#define SEL_W 92
#define CONTENT_X (SEL_W + 8)
#define PANE_ROWS 4
#define PANE_PITCH 30
#define PANE_H 26
#define TILE 10
#define STATUS_Y (VGFX_H - 17)

static const char* pane_names[PANE_ROWS] = {
    "About", "Memory", "Mounts", "CPU",
};
static const int pane_accent[PANE_ROWS] = {
    VGFX_LCYAN, VGFX_GREEN, VGFX_YELLOW, VGFX_MAGENTA,
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

/* ---- routing --- */

static void heading(const char* name, int accent)
{
    int l = (int)strlen(name);
    vgfx_str(CONTENT_X, 2, name, VGFX_WHITE);
    vgfx_hline(CONTENT_X, 18, l * 8, accent);
}

/* Blank-separated tokens trimmed in place. */
static char* skip_ws_p(char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        ++s;
    return s;
}

static void trim_r(char* s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' ||
                     s[n - 1] == '\r'))
        s[--n] = 0;
}

static void clip(char* dst, int cap, const char* src)
{
    int i = 0;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

/* label/value table row: label in white, value in light blue, clipped
 * to the content column so long strings never spill off the window. */
static void frow(int y, const char* label, const char* value)
{
    vgfx_str(CONTENT_X, y, label, VGFX_WHITE);
    if (!value || !*value)
        return;
    int lw = vgfx_text_width(label);
    int cap = (VGFX_W - 4 - (CONTENT_X + lw + 8)) / 8;
    if (cap < 0)
        cap = 0;
    if (cap > 44)
        cap = 44;
    char v[64];
    clip(v, cap + 1, value);
    vgfx_str(CONTENT_X + lw + 8, y, v, VGFX_LBLUE);
}

static int draw_about(void)
{
    struct utsname u;
    if (uname(&u) != 0)
        return 0;
    vgfx_str(CONTENT_X, 30, "VNU / VibeGraphics", VGFX_LCYAN);
    frow(50, "system", u.sysname);
    frow(68, "release", u.release);
    char ver[128];
    int n = read_proc("/proc/version", ver, sizeof(ver));
    if (n > 0) {
        for (int i = 0; ver[i]; ++i)
            if (ver[i] == '\n') {
                ver[i] = 0;
                break;
            }
        frow(86, "version", ver);
    }
    frow(104, "machine", u.machine);
    return 1;
}

static int draw_memory(void)
{
    char mem[256];
    int n = read_proc("/proc/meminfo", mem, sizeof(mem));
    if (n < 0)
        return 0;
    int y = 30;
    char* line = mem;
    for (;;) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        char save = *nl;
        *nl = 0;
        char* key = skip_ws_p(line);
        char* colon = key;
        while (*colon && *colon != ':')
            ++colon;
        if (*colon == ':') {
            *colon = 0;
            char* val = skip_ws_p(colon + 1);
            trim_r(val);
            const char* lab = 0;
            if (!strcmp(key, "MemTotal"))
                lab = "total";
            else if (!strcmp(key, "MemFree"))
                lab = "free";
            else if (!strcmp(key, "MemUsed"))
                lab = "used";
            if (lab && y <= 102) {
                frow(y, lab, val);
                y += 18;
            }
        }
        if (!save)
            break;
        line = nl + 1;
    }
    char up[64];
    int un = read_proc("/proc/uptime", up, sizeof(up));
    if (un > 0) {
        for (int i = 0; up[i]; ++i)
            if (up[i] == '\n' || up[i] == ' ') {
                up[i] = 0;
                break;
            }
        frow(y, "uptime", up);
    }
    return 1;
}

static int draw_mounts(void)
{
    char m[256];
    int n = read_proc("/proc/mounts", m, sizeof(m));
    if (n < 0)
        return 0;
    int y = 30;
    char* line = m;
    while (y <= 102 && *line) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        char save = *nl;
        *nl = 0;
        char* mp = skip_ws_p(line);
        char* sp = mp;
        while (*sp && *sp != ' ' && *sp != '\t')
            ++sp;
        if (*sp)
            *sp = 0;
        char* fs = skip_ws_p(sp + 1);
        sp = fs;
        while (*sp && *sp != ' ' && *sp != '\t')
            ++sp;
        if (*sp)
            *sp = 0;
        char mb[32], fb[16];
        clip(mb, sizeof(mb), mp);
        clip(fb, sizeof(fb), fs);
        frow(y, mb, fb);
        y += 18;
        if (!save)
            break;
        line = nl + 1;
    }
    return 1;
}

static int draw_cpu(void)
{
    char c[192];
    int n = read_proc("/proc/cpuinfo", c, sizeof(c));
    if (n < 0)
        return 0;
    int y = 30;
    char* line = c;
    while (y <= 102 && *line) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        char save = *nl;
        *nl = 0;
        char* key = skip_ws_p(line);
        char* colon = key;
        while (*colon && *colon != ':')
            ++colon;
        if (*colon == ':') {
            *colon = 0;
            char* val = skip_ws_p(colon + 1);
            trim_r(val);
            trim_r(key);
            char kb[24], vb[40];
            clip(kb, sizeof(kb), key);
            clip(vb, sizeof(vb), val);
            frow(y, kb, vb);
            y += 18;
        }
        if (!save)
            break;
        line = nl + 1;
    }
    return 1;
}

/* ---- layout ---- */

static void draw(void)
{
    vgfx_clear(VGFX_LGRAY);

    /* selector column: dark bar with accent tiles per pane */
    vgfx_fill_rect(0, 0, SEL_W, VGFX_H, VGFX_DGRAY);
    vgfx_vline(SEL_W - 1, 0, VGFX_H, VGFX_BLACK);
    for (int i = 0; i < PANE_ROWS; ++i) {
        int y = 4 + i * PANE_PITCH;
        int is_sel = i == cur_pane;
        int is_hov = i == hover_pane;
        if (is_sel)
            vgfx_fill_rect(4, y, SEL_W - 8, PANE_H, VGFX_LBLUE);
        else if (is_hov)
            vgfx_fill_rect(4, y, SEL_W - 8, PANE_H, VGFX_LCYAN);
        int col = (is_sel || is_hov) ? VGFX_BLACK : VGFX_WHITE;
        int ty = y + (PANE_H - TILE) / 2;
        vgfx_fill_rect(8, ty, TILE, TILE, pane_accent[i]);
        vgfx_rect(8, ty, TILE, TILE, VGFX_BLACK);
        vgfx_str(23, y + 5, pane_names[i], col);
    }

    /* content area */
    int ok = 1;
    switch (cur_pane) {
    case 0:
        heading(pane_names[0], pane_accent[0]);
        ok = draw_about();
        break;
    case 1:
        heading(pane_names[1], pane_accent[1]);
        ok = draw_memory();
        break;
    case 2:
        heading(pane_names[2], pane_accent[2]);
        ok = draw_mounts();
        break;
    default:
        heading(pane_names[3], pane_accent[3]);
        ok = draw_cpu();
        break;
    }
    if (!ok)
        vgfx_str(CONTENT_X, 30, "pane unavailable", VGFX_RED);

    /* status strip */
    vgfx_fill_rect(0, STATUS_Y, VGFX_W, VGFX_H - STATUS_Y, VGFX_DGRAY);
    vgfx_hline(0, STATUS_Y, VGFX_W, VGFX_BLACK);
    vgfx_str(4, STATUS_Y + 1, "preferences", VGFX_LBLUE);
    vgfx_str(VGFX_W - 4 - 10 * 8, STATUS_Y + 1, "j/k switch", VGFX_WHITE);
}

static int sel_hit(int px, int py)
{
    if (px < 0 || px >= SEL_W)
        return -1;
    int i = (py - 4) / PANE_PITCH;
    if (i < 0 || i >= PANE_ROWS)
        return -1;
    return py - 4 < i * PANE_PITCH + PANE_H ? i : -1;
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
            else if (ev.key == 'j' || ev.key == 'J')
                cur_pane = (cur_pane + 1) % PANE_ROWS;
            else if (ev.key == 'k' || ev.key == 'K')
                cur_pane = (cur_pane - 1 + PANE_ROWS) % PANE_ROWS;
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_PRESS) {
            hover_pane = sel_hit(ev.x, ev.y);
            if (hover_pane >= 0)
                cur_pane = hover_pane;
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_RELEASE) {
            draw();
            vgfx_flush();
        }
    }
}