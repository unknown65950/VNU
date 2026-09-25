/*
 * files — graphical file manager for the VNU desktop.
 *
 * Browses the kernel's in-memory VFS (/, /bin, /apps, /dev, /proc,
 * /home, /tmp, ...). Rows are drawn in the crisp 8x16 VGA face with a
 * coloured type tile per entry; the selected row gets a full-width
 * light-blue bar, clicking a directory enters it and ".." walks back
 * up. Opening a .png/.jpg/.jpeg picture (Enter or double-click) hands
 * it to picview, which takes over this window and returns it on Esc.
 * The path is shown in the header, the item count in the status
 * strip.
 *
 * Own-house flat look: dark header/status bars, accent tiles, exactly
 * one glyph height per row — no bevels, no legacy window dressing.
 *
 * Keys: j/k select, Enter open, Esc close. Mouse: click to select,
 * release on the same row to open it. Opening a picture (png/jpg/jpeg)
 * runs it in picview inside this window; Esc returns to the manager.
 */
#include <vlibc/vgfx.h>
#include <vlibc/dirent.h>
#include <vlibc/sys/stat.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>

#define MAX_ENTS 32
#define NAME_LEN 255
#define PATH_LEN 128

/* Layout: dark header, full-height 16px rows, dark status strip. The
 * canvas is a native 8x16 grid, so VIS_ROWS is a full column of text. */
#define HEAD_H 18
#define ROW_PITCH 16
#define STATUS_Y (VGFX_H - 17)
#define VIS_ROWS ((STATUS_Y - HEAD_H) / ROW_PITCH)
#define NAME_X 15
#define TILE 8
#define TILE_Y(r) (HEAD_H + (r) * ROW_PITCH + (ROW_PITCH - TILE) / 2)

struct ent {
    char name[NAME_LEN];
    unsigned long size;
    int is_dir;
};

static char cwd[PATH_LEN] = "/";
static struct ent list[MAX_ENTS];
static int list_n = 0;
static int scroll = 0;
static int sel = 0;
static int error_flag = 0;
static int press_row = -1;

static void pcat(char* dst, const char* a, const char* b)
{
    char* p = dst;
    while (*a)
        *p++ = *a++;
    if (p > dst && p[-1] != '/')
        *p++ = '/';
    while (*b)
        *p++ = *b++;
    *p = 0;
}

static int load_dir(const char* path)
{
    list_n = 0;
    scroll = 0;
    sel = 0;
    error_flag = 0;
    DIR* d = opendir(path);
    if (!d) {
        error_flag = 1;
        return 0;
    }
    struct dirent* de;
    while ((de = readdir(d)) && list_n < MAX_ENTS) {
        if (de->d_name[0] == '.') {
            if (de->d_name[1] == '.' || de->d_name[1] == 0)
                continue; /* skip . and ..; we draw our own up-arrow */
        }
        struct ent* e = &list[list_n];
        int i = 0;
        while (de->d_name[i] && i < NAME_LEN - 1) {
            e->name[i] = de->d_name[i];
            ++i;
        }
        e->name[i] = 0;
        if (de->d_type == 4) {
            e->is_dir = 1;
            e->size = 0;
        } else {
            e->is_dir = 0;
            char fp[PATH_LEN];
            pcat(fp, path, e->name);
            struct stat st;
            if (stat(fp, &st) == 0)
                e->size = st.st_size;
            else
                e->size = 0;
        }
        ++list_n;
    }
    closedir(d);
    return 1;
}

static void enter_path(const char* p)
{
    int i = 0;
    while (p[i] && i < PATH_LEN - 1) {
        cwd[i] = p[i];
        ++i;
    }
    cwd[i] = 0;
    load_dir(cwd);
}

static void up_dir(char* out)
{
    int i = (int)strlen(cwd) - 1;
    while (i > 0 && cwd[i] != '/')
        --i;
    if (i == 0) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    int j = 0;
    while (j < i) {
        out[j] = cwd[j];
        ++j;
    }
    out[j] = 0;
}

/* Case-insensitive name suffix match ("sunset.PNG" → image too). */
static int ends_with(const char* s, const char* suf)
{
    int i = 0, j = 0;
    while (s && s[i])
        ++i;
    while (suf && suf[j])
        ++j;
    if (j > i)
        return 0;
    i -= j;
    for (int k = 0; k < j; ++k) {
        char a = s[i + k], b = suf[k];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
    }
    return 1;
}

static void enter_sel(void)
{
    if (sel == 0) {
        char parent[PATH_LEN];
        up_dir(parent);
        enter_path(parent);
        return;
    }
    int idx = scroll + sel - 1;
    if (idx < 0 || idx >= list_n)
        return;
    struct ent* e = &list[idx];
    char fp[PATH_LEN];
    pcat(fp, cwd, e->name);
    if (!e->is_dir) {
        /* Open pictures in the image viewer. picview is exec'd over this
         * task, showing the file in this very window; Esc brings the file
         * manager right back (the kernel reloads this app when it exits). */
        if (ends_with(e->name, ".png") || ends_with(e->name, ".jpg") ||
            ends_with(e->name, ".jpeg")) {
            char* av[3];
            av[0] = "/apps/picview/bin";
            av[1] = fp;
            av[2] = 0;
            execve(av[0], av, 0);
            /* not reached on success; a failed exec just keeps browsing */
        }
        return;
    }
    enter_path(fp);
}

/* ---- tiny text/flag helpers ---- */

static void ncpy_draw(int x, int y, const char* s, int maxch, int color)
{
    int i = 0;
    while (s && s[i] && i < maxch) {
        vgfx_char(x + i * 8, y, s[i], color);
        ++i;
    }
}

static void draw_tile(int x, int y, int color)
{
    vgfx_fill_rect(x, y, TILE, TILE, color);
    vgfx_rect(x, y, TILE, TILE, VGFX_BLACK);
}

static void itoa_ul(unsigned long v, char* out)
{
    char t[16];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    int j = 0;
    while (i)
        out[j++] = t[--i];
    out[j] = 0;
}

/* ---- layout ---- */

static int row_has(int r) /* visible row r has something to select? */
{
    if (r < 0 || r >= VIS_ROWS)
        return 0;
    if (r == 0)
        return 1;
    int idx = scroll + r - 1;
    return idx >= 0 && idx < list_n;
}

static int sel_max(void)
{
    int m = list_n - scroll;
    if (m < 0)
        m = 0;
    return m < VIS_ROWS - 1 ? m : VIS_ROWS - 1;
}

static int scroll_max(void)
{
    int m = list_n - (VIS_ROWS - 1);
    return m < 0 ? 0 : m;
}

static void sel_down(void)
{
    if (sel < sel_max())
        ++sel;
    else if (scroll < scroll_max())
        ++scroll;
}

static void sel_up(void)
{
    if (sel > 0)
        --sel;
    else if (scroll > 0)
        --scroll;
}

static void draw(void)
{
    vgfx_clear(VGFX_LGRAY);

    /* header: current path on a dark bar with a location tile */
    vgfx_fill_rect(0, 0, VGFX_W, HEAD_H, VGFX_DGRAY);
    vgfx_hline(0, HEAD_H - 1, VGFX_W, VGFX_BLACK);
    draw_tile(4, (HEAD_H - TILE) / 2, VGFX_LBLUE);
    ncpy_draw(NAME_X, 1, cwd, (VGFX_W - NAME_X) / 8, VGFX_WHITE);

    /* list rows: row 0 is the parent entry, then scroll..scroll+VIS */
    for (int r = 0; r < VIS_ROWS; ++r) {
        if (!row_has(r))
            continue;
        int y = HEAD_H + r * ROW_PITCH;
        int idx = scroll + r - 1;
        int is_sel = (r == sel);
        int col = is_sel ? VGFX_BLACK : VGFX_WHITE;
        if (is_sel)
            vgfx_fill_rect(0, y, VGFX_W, ROW_PITCH, VGFX_LBLUE);

        if (r == 0) {
            draw_tile(4, TILE_Y(0), VGFX_MAGENTA);
            vgfx_str(NAME_X, y, "..", col);
            vgfx_str(NAME_X + 16, y, "(parent)", col);
        } else {
            struct ent* e = &list[idx];
            draw_tile(4, TILE_Y(r), e->is_dir ? VGFX_YELLOW : VGFX_LGREEN);
            int name_cap = (VGFX_W - NAME_X - 4) / 8;
            if (!e->is_dir)
                name_cap -= 10; /* reserve space for the size column */
            ncpy_draw(NAME_X + 8, y, e->name, name_cap, col);
            if (!e->is_dir) {
                char sz[16];
                itoa_ul(e->size, sz);
                int tw = vgfx_text_width(sz);
                vgfx_str(VGFX_W - 4 - tw, y, sz, col);
            }
        }
    }

    /* status strip */
    vgfx_fill_rect(0, STATUS_Y, VGFX_W, VGFX_H - STATUS_Y, VGFX_DGRAY);
    vgfx_hline(0, STATUS_Y, VGFX_W, VGFX_BLACK);
    if (error_flag) {
        vgfx_str(4, STATUS_Y + 1, "cannot open directory", VGFX_LRED);
    } else {
        vgfx_str(4, STATUS_Y + 1, "j/k move  enter open", VGFX_WHITE);
        char cnt[16];
        itoa_ul((unsigned long)list_n, cnt);
        int cw = vgfx_text_width(cnt);
        vgfx_str(VGFX_W - 4 - cw, STATUS_Y + 1, cnt, VGFX_WHITE);
        vgfx_str(VGFX_W - 4 - cw - 2 - 5 * 8, STATUS_Y + 1, "items",
                 VGFX_LBLUE);
    }
}

static int hit_row(int py)
{
    if (py < HEAD_H || py >= STATUS_Y)
        return -1;
    int r = (py - HEAD_H) / ROW_PITCH;
    if (r < 0 || r >= VIS_ROWS)
        return -1;
    return r;
}

int main(void)
{
    load_dir(cwd);
    draw();
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == 0x1B)
                exit(0);
            else if (ev.key == 'j' || ev.key == 'J')
                sel_down();
            else if (ev.key == 'k' || ev.key == 'K')
                sel_up();
            else if (ev.key == '\n' || ev.key == '\r') {
                if (row_has(sel))
                    enter_sel();
            }
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_PRESS) {
            int r = hit_row(ev.y);
            if (r >= 0 && row_has(r)) {
                sel = r;
                press_row = r;
            }
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_RELEASE) {
            if (press_row >= 0) {
                int r = hit_row(ev.y);
                if (r == press_row && row_has(r))
                    enter_sel();
                press_row = -1;
            }
            draw();
            vgfx_flush();
        }
    }
}