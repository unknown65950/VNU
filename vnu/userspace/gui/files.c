/*
 * files — a compact file manager for VibeGraphics.
 *
 * Navigates the kernel's in-memory VFS (/, /bin, /apps, /dev, /proc,
 * /home, /tmp, ...) with a click-through directory list rendered in
 * the compact 8x8 UI face.  Each row shows a type badge, the entry
 * name and (for regular files) its size; clicking a directory enters
 * it, clicking ".." walks back up, and the path is always shown in
 * the header.
 *
 * Keys: Esc (close the window), Enter (open the highlighted entry).
 */
#include <vlibc/vgfx.h>
#include <vlibc/dirent.h>
#include <vlibc/sys/stat.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>

#define MAX_ENTS 32
#define NAME_LEN 255
#define PATH_LEN 128

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
static int hover = -1;

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

static void enter_sel(void)
{
    if (sel <= 0) { /* up */
        char parent[PATH_LEN];
        int i = (int)strlen(cwd) - 1;
        while (i > 0 && cwd[i] != '/')
            --i;
        if (i == 0)
            parent[0] = '/', parent[1] = 0;
        else {
            int j = 0;
            while (j < i) {
                parent[j] = cwd[j];
                ++j;
            }
            parent[j] = 0;
        }
        enter_path(parent);
        return;
    }
    int idx = scroll + sel - 1;
    if (idx >= list_n)
        return;
    struct ent* e = &list[idx];
    if (!e->is_dir)
        return;
    char fp[PATH_LEN];
    pcat(fp, cwd, e->name);
    enter_path(fp);
}

/* ---- layout ---- */
#define HDR_H 18
#define ROW_H 9
#define BADGE 8

static int row_y(int r)
{
    return HDR_H + r * ROW_H;
}

static void draw(void)
{
    vgfx_clear(VGFX_LGRAY);

    /* header: path pill */
    vgfx_fill_rect(0, 0, VGFX_W, HDR_H, VGFX_DGRAY);
    vgfx_hline(0, HDR_H - 1, VGFX_W, VGFX_BLACK);
    vgfx_str8(3, 3, cwd, VGFX_WHITE);

    /* up-arrow "button" as first list row */
    int up_y = row_y(0);
    vgfx_fill_rect(1, up_y, VGFX_W - 2, ROW_H, VGFX_LBLUE);
    vgfx_str8(4, up_y + 1, "..  (parent dir)", VGFX_BLACK);

    /* list rows (rom scroll..scroll+visible) */
    for (int r = 1; r < MAX_ENTS && list_n - scroll + 1 > r; ++r) {
        int idx = scroll + r - 1;
        if (idx >= list_n)
            break;
        struct ent* e = &list[idx];
        int y = row_y(r);
        int is_sel = (r == sel);
        if (is_sel) {
            vgfx_fill_rect(1, y, VGFX_W - 2, ROW_H, VGFX_LBLUE);
        } else if (hover == r) {
            vgfx_fill_rect(1, y, VGFX_W - 2, ROW_H, VGFX_LCYAN);
        }
        int text_col = VGFX_BLACK;
        vgfx_str8(2, y + 1, e->is_dir ? "d" : "-", text_col);
        vgfx_str8(BADGE, y + 1, e->name, text_col);
        if (!e->is_dir) {
            char sz[16];
            int i = 0;
            unsigned long v = e->size;
            char tmp[16];
            do {
                tmp[i++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            int j = 0;
            while (i > 0)
                sz[j++] = tmp[--i];
            sz[j] = 0;
            int tw = vgfx_text_width8(sz);
            vgfx_str8(VGFX_W - 3 - tw, y + 1, sz, text_col);
        }
    }

    /* status strip */
    int sy = VGFX_H - ROW_H;
    vgfx_fill_rect(0, sy, VGFX_W, ROW_H, VGFX_DGRAY);
    vgfx_hline(0, sy, VGFX_W, VGFX_BLACK);
    if (error_flag)
        vgfx_str8(3, sy + 1, "cannot open directory", VGFX_LRED);
    else {
        vgfx_str8(3, sy + 1, "double-click to open", VGFX_WHITE);
        char cnt[16];
        int i = 0, v = list_n;
        char tmp[8];
        do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
        int j = 0;
        while (i > 0) cnt[j++] = tmp[--i];
        cnt[j] = 0;
        vgfx_str8(3 + 5 * 8, sy + 1, cnt, VGFX_WHITE);
        vgfx_str8(3 + 5 * 8 + 1 * 8, sy + 1, "entries", VGFX_WHITE);
    }
}

static int hit_row(int py)
{
    if (py < HDR_H)
        return -1;
    int r = (py - HDR_H) / ROW_H;
    if (r < 0 || r >= list_n + 1)
        return -1;
    return r;
}

int main(void)
{
    load_dir(cwd);
    draw();
    vgfx_flush();

    int was_down = 0;
    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == 0x1B)
                exit(0);
            if (ev.key == '\n' || ev.key == '\r') {
                enter_sel();
                draw();
                vgfx_flush();
            }
        } else if (ev.type == VGFX_EV_PRESS) {
            int r = hit_row(ev.y);
            if (r >= 0)
                sel = r;
            was_down = 1;
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_RELEASE) {
            if (was_down) {
                int r = hit_row(ev.y);
                if (r >= 0 && r == sel) {
                    if (r == 0)
                        sel = r; /* ".." */
                    enter_sel();
                }
            }
            was_down = 0;
            draw();
            vgfx_flush();
        }
    }
}