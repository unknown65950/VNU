/*
 * files — graphical file manager for the VNU desktop.
 *
 * Browses the kernel's in-memory VFS (/, /bin, /apps, /dev, /proc,
 * /home, /tmp, ...). Two views, toggled with 'v' like a desktop file
 * manager:
 *
 *   list view   — rows in the crisp 8x16 VGA face, each with a
 *                 coloured type tile and the entry size; the selected
 *                 row gets a full-width light-blue bar.
 *
 *   icon view   — a grid of flat 32px icons (yellow folder / white
 *                 document / picture-embossed document for images),
 *                 name underneath each, selection as a light-blue
 *                 cell highlight. The parent entry is a magenta
 *                 folder in both views.
 *
 * Clicking a directory enters it and ".." walks back up. Opening a
 * .png/.jpg/.jpeg picture (Enter, double-click or click-and-release)
 * hands it to picview, which takes over this window and returns it
 * on Esc. The path is shown in the header, the item count in the
 * status strip.
 *
 * Own-house flat look: dark header/status bars, accent tiles, no
 * bevels, no legacy window dressing.
 *
 * Keys: j/k select (h/l too in icon view), Enter open, v toggle view,
 * Esc close. Mouse: click to select, release on the same entry to
 * open it. Opening a picture (png/jpg/jpeg) runs it in picview inside
 * this window; Esc returns to the manager.
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

/* Icon-view grid: 32px icons on a PITCH_X x PITCH_Y lattice, label in
 * the 8x16 face below each icon, light-blue selection behind the cell. */
#define GRID_MX 24
#define GRID_MY 22
#define PITCH_X 72
#define PITCH_Y 56
#define GRID_C ((VGFX_W - 2 * GRID_MX) / PITCH_X)
#define GRID_R ((STATUS_Y - HEAD_H - GRID_MY) / PITCH_Y)
#define GRID_SLOTS (GRID_C * GRID_R)

struct ent {
    char name[NAME_LEN];
    unsigned long size;
    int is_dir;
};

static char cwd[PATH_LEN] = "/";
static struct ent list[MAX_ENTS];
static int list_n = 0;
static int list_scroll = 0;   /* list view: first entry under ".." */
static int iscroll = 0;       /* icon view: first item (0 = "..") */
static int sel = 0;           /* selected item: 0 = "..", n = n-th entry */
static int error_flag = 0;
static int press_item = -1;
static int view_icons = 0;

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
    list_scroll = 0;
    iscroll = 0;
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

static int is_image(const char* name)
{
    return ends_with(name, ".png") || ends_with(name, ".jpg") ||
           ends_with(name, ".jpeg") || ends_with(name, ".bmp");
}

static void enter_sel(void)
{
    if (sel == 0) {
        char parent[PATH_LEN];
        up_dir(parent);
        enter_path(parent);
        return;
    }
    int idx = sel - 1;
    if (idx < 0 || idx >= list_n)
        return;
    struct ent* e = &list[idx];
    char fp[PATH_LEN];
    pcat(fp, cwd, e->name);
    if (!e->is_dir) {
        /* Open pictures in the image viewer. picview is exec'd over this
         * task, showing the file in this very window; Esc brings the file
         * manager right back (the kernel reloads this app when it exits). */
        if (is_image(e->name)) {
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

/* ---- selection / scrolling (item 0 = "..", N = N-th entry) ---- */

static int scroll_max_list(void)
{
    int m = list_n - (VIS_ROWS - 1);
    return m < 0 ? 0 : m;
}

/* Keep `sel` on screen in the list view (adjusts list_scroll). */
static void list_vis(void)
{
    if (sel <= 0)
        return;
    if (sel - 1 < list_scroll)
        list_scroll = sel - 1;
    int row = sel - list_scroll;
    if (row >= VIS_ROWS)
        list_scroll = sel - (VIS_ROWS - 1);
    int mx = scroll_max_list();
    if (list_scroll > mx)
        list_scroll = mx;
    if (list_scroll < 0)
        list_scroll = 0;
}

/* Keep `sel` on screen in the icon view (adjusts iscroll). */
static void icon_vis(void)
{
    if (sel < iscroll)
        iscroll = sel;
    int s = sel - iscroll;
    if (s >= GRID_SLOTS)
        iscroll = sel - (GRID_SLOTS - 1);
    int mx = list_n + 1 - GRID_SLOTS;
    if (mx < 0)
        mx = 0;
    if (iscroll > mx)
        iscroll = mx;
    if (iscroll < 0)
        iscroll = 0;
    if (sel < iscroll)
        iscroll = sel;
}

/* Step the icon-grid selection by (dr, dc) rows/columns. Moving off the
 * bottom of a page scrolls the grid down a page (top row, same column),
 * moving off the top scrolls back up; sides do not wrap. */
static void icon_step(int dr, int dc)
{
    int s = sel - iscroll;
    int r = s / GRID_C;
    int c = s - r * GRID_C;
    if (dc != 0) {
        int nc = c + dc;
        if (nc < 0 || nc >= GRID_C)
            return;
        int nit = iscroll + r * GRID_C + nc;
        if (nit > list_n)
            return;
        sel = nit;
        return;
    }
    if (dr < 0) {
        if (r > 0) {
            sel = iscroll + (r - 1) * GRID_C + c;
        } else if (iscroll > 0) {
            iscroll -= GRID_C;
            sel = iscroll + c;
        } else {
            return; /* already at the very top */
        }
        if (sel > list_n)
            sel = list_n;
        return;
    }
    /* moving down */
    if (r >= GRID_R - 1) {
        int mx = list_n + 1 - GRID_SLOTS;
        if (mx < 0)
            mx = 0;
        if (iscroll < mx) {
            iscroll += GRID_C;
            if (iscroll > mx)
                iscroll = mx;
        }
        sel = iscroll + c;
        if (sel > list_n)
            sel = list_n;
    } else {
        int nit = iscroll + (r + 1) * GRID_C + c;
        if (nit > list_n)
            return;
        sel = nit;
    }
}

static void sel_j(void)
{
    if (view_icons) {
        icon_step(1, 0);
        return;
    }
    if (sel == 0) {
        if (list_n > 0)
            sel = 1;
    } else if (sel < list_n) {
        ++sel;
    }
    list_vis();
}

static void sel_k(void)
{
    if (view_icons) {
        icon_step(-1, 0);
        return;
    }
    if (sel > 0)
        --sel;
    list_vis();
}

/* Toggle the view, carrying the selected entry across. */
static void toggle_view(void)
{
    if (view_icons) {
        view_icons = 0;
        if (sel > 0)
            list_scroll = sel - 1;
        list_vis();
    } else {
        view_icons = 1;
        iscroll = 0;
        icon_vis();
    }
}

/* ---- drawing ---- */

/* Flat folder icon (32px box): tab + body, outlined dark. The parent
 * entry reuses it in magenta. */
static void draw_folder_icon(int x, int y, int color)
{
    vgfx_fill_rect(x + 7, y + 5, 11, 8, color);
    vgfx_rect(x + 7, y + 5, 11, 8, VGFX_BLACK);
    vgfx_fill_rect(x + 2, y + 12, 28, 17, color);
    vgfx_rect(x + 2, y + 12, 28, 17, VGFX_BLACK);
}

/* Flat document icon: white page with a folded top-right corner and a
 * few ruled lines; images get a little landscape instead of the lines. */
static void draw_page_icon(int x, int y, int img)
{
    vgfx_fill_rect(x + 8, y + 2, 16, 29, VGFX_WHITE);
    vgfx_rect(x + 8, y + 2, 16, 29, VGFX_BLACK);
    /* folded corner */
    int i;
    for (i = 0; i < 5; ++i)
        vgfx_put_pixel(x + 24 - i, y + 2 + i, VGFX_BLACK);
    if (img) {
        /* a miniature picture: sky, sun, ground */
        vgfx_fill_rect(x + 11, y + 10, 10, 9, VGFX_LBLUE);
        vgfx_put_pixel(x + 14, y + 12, VGFX_YELLOW);
        vgfx_hline(x + 11, y + 16, 10, VGFX_LGREEN);
        vgfx_rect(x + 11, y + 10, 10, 9, VGFX_BLACK);
    } else {
        vgfx_hline(x + 11, y + 8, 10, VGFX_LGRAY);
        vgfx_hline(x + 11, y + 13, 10, VGFX_LGRAY);
        vgfx_hline(x + 11, y + 18, 6, VGFX_LGRAY);
    }
}

static void draw_list(void)
{
    /* header: current path on a dark bar with a location tile */
    vgfx_fill_rect(0, 0, VGFX_W, HEAD_H, VGFX_DGRAY);
    vgfx_hline(0, HEAD_H - 1, VGFX_W, VGFX_BLACK);
    draw_tile(4, (HEAD_H - TILE) / 2, VGFX_LBLUE);
    ncpy_draw(NAME_X, 1, cwd, (VGFX_W - NAME_X) / 8, VGFX_WHITE);

    /* rows: row 0 is the parent entry, then scroll..scroll+VIS entries */
    for (int r = 0; r < VIS_ROWS; ++r) {
        int it = (r == 0) ? 0 : list_scroll + r;
        if (it > list_n)
            continue;
        int y = HEAD_H + r * ROW_PITCH;
        int is_sel = (it == sel);
        int col = is_sel ? VGFX_BLACK : VGFX_WHITE;
        if (is_sel)
            vgfx_fill_rect(0, y, VGFX_W, ROW_PITCH, VGFX_LBLUE);

        if (r == 0) {
            draw_tile(4, TILE_Y(0), VGFX_MAGENTA);
            vgfx_str(NAME_X, y, "..", col);
            vgfx_str(NAME_X + 16, y, "(parent)", col);
        } else {
            struct ent* e = &list[it - 1];
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
}

static void draw_icons(void)
{
    vgfx_fill_rect(0, 0, VGFX_W, HEAD_H, VGFX_DGRAY);
    vgfx_hline(0, HEAD_H - 1, VGFX_W, VGFX_BLACK);
    draw_tile(4, (HEAD_H - TILE) / 2, VGFX_LBLUE);
    ncpy_draw(NAME_X, 1, cwd, (VGFX_W - NAME_X) / 8, VGFX_WHITE);

    for (int s = 0; s < GRID_SLOTS; ++s) {
        int it = iscroll + s;
        if (it < 0 || it > list_n)
            continue;
        int c = s % GRID_C;
        int r = s / GRID_C;
        int x = GRID_MX + c * PITCH_X;
        int y = HEAD_H + GRID_MY + r * PITCH_Y;
        int is_sel = (it == sel);
        int col = is_sel ? VGFX_BLACK : VGFX_WHITE;

        if (is_sel)
            vgfx_fill_rect(x - 4, y - 4, PITCH_X - 8, PITCH_Y - 4, VGFX_LBLUE);

        if (it == 0) {
            draw_folder_icon(x, y, VGFX_MAGENTA); /* parent */
            ncpy_draw(x, y + 36, "..", PITCH_X / 8, col);
        } else {
            struct ent* e = &list[it - 1];
            if (e->is_dir)
                draw_folder_icon(x, y, VGFX_YELLOW);
            else
                draw_page_icon(x, y, is_image(e->name));
            ncpy_draw(x, y + 36, e->name, PITCH_X / 8, col);
        }
    }
}

static void draw(void)
{
    vgfx_clear(VGFX_LGRAY);
    if (view_icons)
        draw_icons();
    else
        draw_list();

    /* status strip */
    vgfx_fill_rect(0, STATUS_Y, VGFX_W, VGFX_H - STATUS_Y, VGFX_DGRAY);
    vgfx_hline(0, STATUS_Y, VGFX_W, VGFX_BLACK);
    if (error_flag) {
        vgfx_str(4, STATUS_Y + 1, "cannot open directory", VGFX_LRED);
    } else {
        if (view_icons)
            vgfx_str(4, STATUS_Y + 1, "h/j/k/l move  enter open  v:list",
                     VGFX_WHITE);
        else
            vgfx_str(4, STATUS_Y + 1, "j/k move  enter open  v:icons",
                     VGFX_WHITE);
        char cnt[16];
        itoa_ul((unsigned long)list_n, cnt);
        int cw = vgfx_text_width(cnt);
        vgfx_str(VGFX_W - 4 - cw, STATUS_Y + 1, cnt, VGFX_WHITE);
        vgfx_str(VGFX_W - 4 - cw - 2 - 5 * 8, STATUS_Y + 1, "items",
                 VGFX_LBLUE);
    }
}

/* Map a client-area point to an item, or -1 for the empty gutter. */
static int hit_item(int x, int y)
{
    if (y < HEAD_H || y >= STATUS_Y)
        return -1;
    if (view_icons) {
        int c = (x - GRID_MX) / PITCH_X;
        int r = (y - HEAD_H - GRID_MY) / PITCH_Y;
        if (c < 0 || c >= GRID_C || r < 0 || r >= GRID_R)
            return -1;
        int it = iscroll + r * GRID_C + c;
        return (it >= 0 && it <= list_n) ? it : -1;
    }
    int r = (y - HEAD_H) / ROW_PITCH;
    if (r < 0 || r >= VIS_ROWS)
        return -1;
    if (r == 0)
        return 0;
    int it = list_scroll + r;
    return (it <= list_n) ? it : -1;
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
                sel_j();
            else if (ev.key == 'k' || ev.key == 'K')
                sel_k();
            else if (ev.key == 'h' || ev.key == 'H') {
                if (view_icons)
                    icon_step(0, -1);
            } else if (ev.key == 'l' || ev.key == 'L') {
                if (view_icons)
                    icon_step(0, 1);
            } else if (ev.key == 'v' || ev.key == 'V') {
                toggle_view();
            } else if (ev.key == '\n' || ev.key == '\r') {
                if (!error_flag)
                    enter_sel();
            }
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_PRESS) {
            int it = hit_item(ev.x, ev.y);
            if (it >= 0) {
                sel = it;
                if (view_icons)
                    icon_vis();
                else
                    list_vis();
                press_item = it;
            }
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_RELEASE) {
            if (press_item >= 0) {
                if (hit_item(ev.x, ev.y) == press_item)
                    enter_sel();
                press_item = -1;
            }
            draw();
            vgfx_flush();
        }
    }
}