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
 *   About     — OS identification (utsname) + /proc/version
 *   Memory    — live totals from /proc/meminfo and /proc/uptime
 *   Mounts    — mounted filesystems from /proc/mounts
 *   CPU       — /proc/cpuinfo snippet
 *   Wallpaper — the pictures in /etc/vnu/pics plus the shipped desktop;
 *               Enter hands the chosen one to the kernel, which decodes
 *               it and the desktop redraws at once
 *
 * Keys: j/k switch pane, Esc closes, and in the Wallpaper pane the
 * arrows move the cursor while Enter applies. Mouse: click a pane to
 * switch, click a picture to put the cursor on it.
 */
#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/dirent.h>
#include <vlibc/sys/syscall.h>
#include <vlibc/sys/utsname.h>
#include <vnu/abi.h>

#define SEL_W 92
#define CONTENT_X (SEL_W + 8)
#define PANE_ROWS 5
#define PANE_PITCH 30
#define PANE_H 26
#define TILE 10
#define STATUS_Y (VGFX_H - 17)
#define LIST_Y 30
#define ROW_H 18
#define LIST_MAX 12
#define PICS_DIR "/etc/vnu/pics"
#define SHIPPED "/etc/vnu/wallpaper.default"
#define NAME_CAP 32
#define PATH_CAP 96

static const char* pane_names[PANE_ROWS] = {
    "About", "Memory", "Mounts", "CPU", "Wallpaper",
};
static const int pane_accent[PANE_ROWS] = {
    VGFX_LCYAN, VGFX_GREEN, VGFX_YELLOW, VGFX_MAGENTA, VGFX_LGREEN,
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

/* Wallpaper pane state: the candidates, the cursor, and the last
 * verdict so the user can see why a pick was refused. */
static char wp_names[LIST_MAX][NAME_CAP];
static int wp_n = 0;
static int wp_sel = 0;
static const char* wp_status = "Enter applies the picture";
static int wp_status_col = VGFX_LBLUE;

/* /etc/vnu/pics, then the shipped desktop. A JPEG is listed too — the
 * kernel will refuse it, and the status line then says so. */
static void scan_walls(void)
{
    wp_n = 0;
    DIR* d = opendir(PICS_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != 0 && wp_n < LIST_MAX) {
            if (e->d_type != 8) /* regular file */
                continue;
            clip(wp_names[wp_n], NAME_CAP, e->d_name);
            if (*wp_names[wp_n])
                ++wp_n;
        }
        closedir(d);
    }
    if (wp_n < LIST_MAX)
        clip(wp_names[wp_n++], NAME_CAP, "wallpaper.default");
}

static void scan_walls_refresh(void)
{
    char keep[NAME_CAP];
    clip(keep, sizeof(keep), wp_names[wp_sel]);
    scan_walls();
    for (int i = 0; i < wp_n; ++i) {
        if (!strcmp(wp_names[i], keep)) {
            wp_sel = i;
            return;
        }
    }
    if (wp_sel >= wp_n)
        wp_sel = 0;
}

static void wp_path(char* dst, int cap)
{
    /* The shipped desktop keeps its own name in the list, so it needs
     * the full path rather than a join with the pack directory. */
    if (!strcmp(wp_names[wp_sel], "wallpaper.default")) {
        clip(dst, cap, SHIPPED);
        return;
    }
    int i = 0;
    while (PICS_DIR[i] && i < cap - 2) {
        dst[i] = PICS_DIR[i];
        ++i;
    }
    dst[i++] = '/';
    int j = 0;
    while (wp_names[wp_sel][j] && i < cap - 1) {
        dst[i] = wp_names[wp_sel][j];
        ++i;
        ++j;
    }
    dst[i] = 0;
}

/* The kernel is the judge: it decodes the candidate, and only swaps it
 * in when the picture survives. */
static void wp_apply(void)
{
    char path[PATH_CAP];
    wp_path(path, sizeof(path));
    long rc = syscall(SYS_wallpaper, (unsigned long)path);
    if (rc == 0) {
        wp_status = "applied — the desktop redrew";
        wp_status_col = VGFX_LGREEN;
    } else if (rc == -VNU_EINVAL) {
        wp_status = "not a usable wallpaper (bmp or png, at most 512x384 "
                    "and 64 KiB)";
        wp_status_col = VGFX_LRED;
    } else if (rc == -VNU_ENOENT) {
        wp_status = "the file is gone";
        wp_status_col = VGFX_LRED;
    } else {
        wp_status = "the kernel refused it";
        wp_status_col = VGFX_LRED;
    }
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
            else if (!strcmp(key, "PoolTotal"))
                lab = "pool";
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

static int draw_wallpaper(void)
{
    /* The one pane that is not a /proc table: a list with a cursor. */
    vgfx_str(CONTENT_X, LIST_Y, "desktop background", VGFX_LGREEN);
    if (wp_n == 0) {
        vgfx_str(CONTENT_X, LIST_Y + ROW_H + 4, "no pictures in "
                 PICS_DIR, VGFX_LRED);
        return 1;
    }
    for (int i = 0; i < wp_n && i < LIST_MAX; ++i) {
        int y = LIST_Y + 26 + i * ROW_H;
        if (i == wp_sel) {
            vgfx_fill_rect(CONTENT_X - 4, y - 2, VGFX_W - CONTENT_X - 4,
                           ROW_H - 2, VGFX_LBLUE);
            vgfx_str(CONTENT_X, y, wp_names[i], VGFX_BLACK);
        } else {
            vgfx_str(CONTENT_X, y, wp_names[i], VGFX_WHITE);
        }
    }
    /* One selected, up to twelve shown: say so rather than pretend. */
    if (wp_n > LIST_MAX) {
        vgfx_str(CONTENT_X, LIST_Y + 26 + LIST_MAX * ROW_H,
                 "… more in the pack", VGFX_LGRAY);
    }
    frow(STATUS_Y - ROW_H - 4, "", wp_status);
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
    case 3:
        heading(pane_names[3], pane_accent[3]);
        ok = draw_cpu();
        break;
    default:
        heading(pane_names[4], pane_accent[4]);
        ok = draw_wallpaper();
        break;
    }
    if (!ok)
        vgfx_str(CONTENT_X, 30, "pane unavailable", VGFX_RED);

    /* status strip */
    vgfx_fill_rect(0, STATUS_Y, VGFX_W, VGFX_H - STATUS_Y, VGFX_DGRAY);
    vgfx_hline(0, STATUS_Y, VGFX_W, VGFX_BLACK);
    vgfx_str(4, STATUS_Y + 1, "preferences", VGFX_LBLUE);
    vgfx_str(VGFX_W - 4 - 10 * 8, STATUS_Y + 1,
             cur_pane == 4 ? "arrows/enter" : "j/k switch", VGFX_WHITE);
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

/* Which picture row the mouse is over, or -1. */
static int list_hit(int px, int py)
{
    if (px < CONTENT_X - 4)
        return -1;
    int i = (py - (LIST_Y + 26)) / ROW_H;
    if (i < 0 || i >= wp_n)
        return -1;
    if (py - (LIST_Y + 26) >= wp_n * ROW_H)
        return -1;
    return i;
}

int main(void)
{
    scan_walls();
    draw();
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == VNU_KEY_ESC)
                exit(0);
            else if (ev.key == 'j' || ev.key == 'J')
                cur_pane = (cur_pane + 1) % PANE_ROWS;
            else if (ev.key == 'k' || ev.key == 'K')
                cur_pane = (cur_pane - 1 + PANE_ROWS) % PANE_ROWS;
            else if (cur_pane == 4 && ev.key == VNU_KEY_UP && wp_sel > 0)
                --wp_sel;
            else if (cur_pane == 4 && ev.key == VNU_KEY_DOWN &&
                     wp_sel < wp_n - 1)
                ++wp_sel;
            else if (cur_pane == 4 && (ev.key == '\n' || ev.key == '\r')) {
                wp_apply();
                scan_walls_refresh();
            }
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_PRESS) {
            hover_pane = sel_hit(ev.x, ev.y);
            if (hover_pane >= 0) {
                cur_pane = hover_pane;
            } else if (cur_pane == 4) {
                int row = list_hit(ev.x, ev.y);
                if (row >= 0)
                    wp_sel = row;
            }
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_RELEASE) {
            draw();
            vgfx_flush();
        }
    }
}