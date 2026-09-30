/*
 * vibecommander — a Norton Commander / Midnight Commander style file
 * manager for the text console.
 *
 * Two side-by-side panels, each browsing its own directory, Tab
 * switching which one is active. Enter (or Right) opens the entry
 * under the cursor: a directory changes the panel there, a regular
 * file hands it to the built-in viewer. The function keys run the
 * file operations, as they do on a Commander:
 *
 *   F1 Help      F3 View    F4 Edit    F5 Copy
 *   F6 Move      F7 Mkdir   F8 Delete  F10 Quit
 *
 * F4 runs the real editor (vedit) as a child process — the panel
 * waits for it and redraws when it comes back. Everything else is
 * handled in place: no external tools, so it works off a bare VFS
 * and never depends on the PATH.
 *
 * The screen is the console's small ANSI subset (CUP/ED/EL/SGR from
 * term.h): the whole surface is redrawn on every key, one cell row at
 * a time, with the attribute per entry. Nothing relies on scrolling
 * or on the console echoing, so the panels survive being full of
 * anything the VFS throws at them.
 *
 * Keys:
 *   Tab            switch the active panel
 *   Enter / Right  open the entry (directory or viewer)
 *   Left           leave to the parent directory
 *   Up/Down/Home/End move the cursor, ' ' pages the list
 *   a..z 0..9 . _ - jump to the first entry starting with that byte
 *   F1..F8, F10    help, view, edit, copy, move, mkdir, delete, quit
 *   Esc            leave the viewer or the manager
 */
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/dirent.h>
#include <vlibc/keys.h>
#include <vlibc/term.h>
#include <vlibc/sys/stat.h>
#include <vlibc/sys/wait.h>

/* --- terminal ------------------------------------------------------ */

static unsigned g_rows = 25, g_cols = 80;

static void out(const char* s)
{
    if (s)
        write(1, s, strlen(s));
}

static void wch_raw(char c)
{
    write(1, &c, 1);
}

static void goto_rc(unsigned r, unsigned c)
{
    char q[16];
    int n = tnu_goto(q, sizeof q, r, c);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static void sgr(const char* p)
{
    char q[16];
    int n = tnu_sgr(q, sizeof q, p);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

/* Erase the rest of the current cell row, so a shorter line never
 * leaves a longer one's tail behind it. */
static void erase_row(void)
{
    char q[8];
    int n = tnu_erase_eol(q, sizeof q);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static void reset_screen(void)
{
    char q[8];
    int n = tnu_clear(q, sizeof q);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

/* --- data ---------------------------------------------------------- */

#define MAX_ENT  320
#define NAME_CAP 40
#define P_CAP    160

enum { T_FILE, T_DIR, T_PARENT };

struct Ent {
    char name[NAME_CAP];
    uint32_t size;
    uint8_t type;
};

struct Panel {
    char path[P_CAP];
    struct Ent ent[MAX_ENT];
    int n;
    int cur;
    int top;
};

static struct Panel g_pan[2];
static int g_active = 0;
static char g_msg[80];

/* entry display bounds in cells */
static unsigned p_width(void)
{
    return g_cols / 2;
}
static unsigned namew(void)
{
    unsigned w = p_width();
    return w > 14 ? w - 11 : 3;
}

/* Path and text helpers -------------------------------------------- */

static void set_msg(const char* s)
{
    int i = 0;
    for (; s[i] && i < (int)sizeof(g_msg) - 1; ++i)
        g_msg[i] = s[i];
    g_msg[i] = 0;
}

static int ci_cmp(const char* a, const char* b)
{
    for (; *a || *b; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z')
            ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z')
            cb = (char)(cb - 'a' + 'A');
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    return 0;
}

static void join_path(const char* dir, const char* name, char* out,
                      unsigned outn)
{
    if (name[0] == '/') {
        strncpy(out, name, outn - 1);
        out[outn - 1] = 0;
        return;
    }
    unsigned i = 0;
    while (dir[i] && i + 1 < outn) {
        out[i] = dir[i];
        ++i;
    }
    if (i && i + 1 < outn && out[i - 1] != '/')
        out[i++] = '/';
    unsigned k = 0;
    while (name[k] && i + 1 < outn)
        out[i++] = name[k++];
    out[i] = 0;
}

static void parent_of(const char* p, char* out)
{
    if (!p[1]) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    int last = 0;
    for (int i = 0; p[i]; ++i)
        if (p[i] == '/')
            last = i;
    if (last == 0) {
        out[0] = '/';
        out[1] = 0;
    } else {
        int i = 0;
        for (; i < last && i + 1 < P_CAP; ++i)
            out[i] = p[i];
        out[i] = 0;
    }
}

static void entry_path(const struct Panel* p, const struct Ent* e,
                       char* out)
{
    join_path(p->path, e->name, out, P_CAP);
}

/* Decimal bytes into a fixed buffer, right-aligned in `field` cells. */
static void fmt_size(uint32_t v, char* o)
{
    char d[12];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 12);
    for (int i = 0; i < n; ++i)
        o[i] = d[n - 1 - i];
    o[n] = 0;
}

/* --- directory listing --------------------------------------------- */

static void add_entry(struct Panel* p, const char* name, uint8_t type,
                      uint32_t size)
{
    if (p->n >= MAX_ENT)
        return;
    struct Ent* e = &p->ent[p->n];
    strncpy(e->name, name, NAME_CAP - 1);
    e->name[NAME_CAP - 1] = 0;
    e->type = type;
    e->size = size;
    ++p->n;
}

static void sort_panel(struct Panel* p)
{
    /* ".." stays first; everything after it is dirs, then files, each
     * group alphabetical and case-insensitive. */
    int first = p->n && p->ent[0].type == T_PARENT ? 1 : 0;
    for (int i = first + 1; i < p->n; ++i) {
        struct Ent t = p->ent[i];
        int j = i - 1;
        int tdir = t.type == T_DIR ? 0 : 1;
        while (j >= first) {
            int jdir = p->ent[j].type == T_DIR ? 0 : 1;
            int worse = jdir > tdir ||
                        (jdir == tdir && ci_cmp(p->ent[j].name, t.name) > 0);
            if (!worse)
                break;
            p->ent[j + 1] = p->ent[j];
            --j;
        }
        p->ent[j + 1] = t;
    }
}

static void load_panel(struct Panel* p)
{
    p->n = 0;
    p->cur = 0;
    p->top = 0;
    if (p->path[1] != 0)
        add_entry(p, "..", T_PARENT, 0);

    DIR* d = opendir(p->path);
    if (!d) {
        set_msg("cannot open the directory");
        if (p->n == 0)
            add_entry(p, "..", T_PARENT, 0);
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if ((e->d_name[0] == '.' && !e->d_name[1]) ||
            (e->d_name[0] == '.' && e->d_name[1] == '.' && !e->d_name[2]))
            continue;
        uint8_t type = e->d_type == 4 ? T_DIR : T_FILE;
        uint32_t size = 0;
        char full[P_CAP];
        join_path(p->path, e->d_name, full, sizeof full);
        struct stat st;
        if (stat(full, &st) == 0)
            size = st.st_size;
        add_entry(p, e->d_name, type, size);
    }
    closedir(d);
    sort_panel(p);
}

/* --- drawing ------------------------------------------------------- */

static void draw_title(unsigned c0, const struct Panel* p, int active)
{
    goto_rc(1, c0 + 1);
    sgr(active ? "1;37;44" : "1;34");
    char buf[72];
    unsigned cap = p_width() > 2 ? p_width() - 2 : 0;
    unsigned i = 0;
    buf[i++] = active ? '[' : ' ';
    unsigned len = (unsigned)strlen(p->path);
    unsigned off = len > cap ? len - cap : 0;
    for (; p->path[off] && i < cap + 1; ++off, ++i)
        buf[i] = p->path[off];
    if (active && i < (unsigned)sizeof(buf))
        buf[i++] = ']';
    buf[i] = 0;
    out(buf);
    erase_row();
}

static void draw_list(unsigned c0, const struct Panel* p, int active)
{
    /* list rows are 2..(g_rows-2); the second-to-last row is the
     * status line and the last is the function-key bar */
    unsigned rows_here = g_rows > 3 ? g_rows - 1 : g_rows;
    unsigned nw = namew();
    unsigned width = p_width();
    unsigned sizefield = width - nw - 1;
    for (unsigned r = 2; r < rows_here; ++r) {
        int idx = p->top + (int)(r - 2);
        goto_rc(r, c0 + 1);
        if (idx < p->n) {
            const struct Ent* e = &p->ent[idx];
            const char* attr = (idx == p->cur && active)     ? "1;37;44"
                               : (idx == p->cur && !active)  ? "0;30;47"
                               : e->type == T_PARENT         ? "1;36"
                               : e->type == T_DIR            ? "1;34"
                                                             : "0";
            sgr(attr);
            unsigned used = 0;
            for (; e->name[used] && used < nw; ++used)
                wch_raw(e->name[used]);
            for (; used < nw + 1; ++used)
                wch_raw(' ');
            char n[12];
            if (e->type == T_DIR || e->type == T_PARENT) {
                out("DIR");
            } else {
                fmt_size(e->size, n);
                unsigned len = (unsigned)strlen(n);
                for (unsigned k = 0; k + len < sizefield; ++k)
                    wch_raw(' ');
                out(n);
            }
        }
        erase_row();
    }
}

static void draw_status_and_bar(void)
{
    unsigned rows = g_rows;
    if (rows < 3)
        return;

    goto_rc(rows - 1, 1);
    struct Panel* p = &g_pan[g_active];
    if (g_msg[0]) {
        sgr("1;33");
        out(g_msg);
    } else {
        sgr("7");
        if (p->n > 0 && p->cur < p->n) {
            const struct Ent* e = &p->ent[p->cur];
            out(e->name);
            out("  ");
            if (e->type == T_DIR)
                out("<DIR>");
            else {
                char n[12];
                fmt_size(e->size, n);
                out(n);
                out(" bytes");
            }
            out("   Enter=open  F3=view  F4=edit");
        } else {
            out("(empty)   F7=mkdir  F8=delete  Tab=panel  Esc=quit");
        }
    }
    erase_row();

    goto_rc(rows, 1);
    sgr("30;47");
    out(" 1Help  3View  4Edit  5Copy  6Move  7Mkdir  8Delete  10Quit");
    erase_row();
    goto_rc(rows, g_cols);
    sgr("0");
}

static void render(void)
{
    draw_title(0, &g_pan[0], g_active == 0);
    draw_title(g_cols / 2, &g_pan[1], g_active == 1);
    draw_list(0, &g_pan[0], g_active == 0);
    draw_list(g_cols / 2, &g_pan[1], g_active == 1);
    draw_status_and_bar();
}

/* --- one-line input ------------------------------------------------ */

/* A tiny prompt on the status line: letters typed, Backspace edits,
 * Enter returns 1 (the caller reads `buf`), Esc or EOF cancels with 0. */
static int prompt_line(const char* label, char* buf, unsigned cap)
{
    unsigned len = 0, rows = g_rows;
    buf[0] = 0;
    for (;;) {
        goto_rc(rows - 1, 1);
        sgr("1;37;44");
        out(label);
        out(buf);
        erase_row();
        goto_rc(rows, 1);
        sgr("0");
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            return 0;
        unsigned char ch = (unsigned char)raw;
        if (ch == '\n')
            return 1;
        if (ch == VNU_KEY_ESC)
            return 0;
        if (ch == '\b') {
            if (len > 0)
                buf[--len] = 0;
        } else if (ch >= 32 && ch < 127 && len + 1 < cap) {
            buf[len++] = (char)ch;
            buf[len] = 0;
        }
    }
}

static int confirm_line(const char* label)
{
    char b[2] = {0, 0};
    if (!prompt_line(label, b, sizeof b))
        return 0;
    return b[0] == 'y' || b[0] == 'Y';
}

/* --- viewer / pager ------------------------------------------------ */

#define VIEW_CAP 49152
#define LAY_MAX  1600

static char g_view[VIEW_CAP];
struct LRow {
    uint32_t off;   /* byte offset of the first character of the row */
    uint16_t len;   /* cells, before the newline or the wrap */
};

static struct LRow g_lay[LAY_MAX];
static int g_lay_n = 0;

static uint32_t load_file_into(const char* path, char* buf, uint32_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint32_t total = 0;
    for (;;) {
        long n = read(fd, buf + total, cap - total);
        if (n <= 0)
            break;
        total += (uint32_t)n;
        if (total >= cap)
            break;
    }
    close(fd);
    return total;
}

/* Split the text into display rows, wrapping long lines at the width
 * of the content area. */
static void layout_text(const char* data, uint32_t n, unsigned width)
{
    g_lay_n = 0;
    uint32_t i = 0;
    while (i < n && g_lay_n < LAY_MAX) {
        struct LRow* lr = &g_lay[g_lay_n];
        lr->off = i;
        lr->len = 0;
        uint16_t col = 0;
        while (i < n && col < width) {
            char c = data[i];
            if (c == '\n')
                break;
            if (c == '\r') {
                ++i;
                continue;
            }
            ++col;
            ++i;
        }
        lr->len = col;
        ++g_lay_n;
        if (i >= n)
            continue;
        if (data[i] == '\n')
            ++i;
        else if (col == width)
            continue;   /* the wrapped tail is its own row */
    }
}

static void run_pager(const char* title, const char* data, uint32_t n)
{
    unsigned rows = g_rows, cols = g_cols;
    unsigned content_w = cols > 2 ? cols - 2 : cols;
    layout_text(data, n, content_w);
    unsigned top = 0;
    for (;;) {
        reset_screen();
        goto_rc(1, 1);
        sgr("1;37;44");
        out(title);
        erase_row();
        /* content rows are 2..(rows-1); the last row is the hint bar */
        for (unsigned r = 2; r < rows; ++r) {
            int idx = (int)top + (int)(r - 2);
            goto_rc(r, 1);
            sgr("0");
            if (idx < g_lay_n) {
                const struct LRow* lr = &g_lay[idx];
                uint32_t i = lr->off;
                for (uint16_t k = 0; k < lr->len; ++k) {
                    unsigned char cc = (unsigned char)data[i++];
                    /* binary in a file must not side with the console's
                     * CSI parser: anything but printing text is a cell */
                    if (cc < 32 || cc == 127)
                        cc = ' ';
                    wch_raw((char)cc);
                }
            }
            erase_row();
        }
        goto_rc(rows, 1);
        sgr("0;30;47");
        out("  q/Esc back   Up/Down line   Space page   b back   Home/End");
        erase_row();
        goto_rc(rows, cols);
        sgr("0");

        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            break;
        unsigned ch = (unsigned char)raw;
        unsigned view = rows - 2;   /* content rows on one screen */
        unsigned max_top = g_lay_n > (int)view ? (unsigned)g_lay_n - view : 0;
        if (ch == 'q' || ch == VNU_KEY_ESC) {
            break;
        } else if (ch == VNU_KEY_UP || ch == 'k') {
            if (top > 0)
                --top;
        } else if (ch == VNU_KEY_DOWN || ch == 'j') {
            if (top < max_top)
                ++top;
        } else if (ch == ' ') {
            top += view;
            if (top > max_top)
                top = max_top;
        } else if (ch == 'b') {
            top = top > view ? top - view : 0;
        } else if (ch == VNU_KEY_HOME) {
            top = 0;
        } else if (ch == VNU_KEY_END) {
            top = max_top;
        }
    }
}

static void help_screen(void)
{
    static const char help[] =
        "vibecommander — two-panel file manager\n"
        "\n"
        "  Tab            switch the active panel\n"
        "  Enter / Right  open the entry: a directory changes the\n"
        "                 panel there, a file opens in the viewer\n"
        "  Left           leave to the parent directory\n"
        "  Up / Down      move the cursor\n"
        "  Home / End     jump to the first / last entry\n"
        "  Space          page the list down\n"
        "  a..z 0..9 _ . - jump to the next entry starting with it\n"
        "\n"
        "  F1  this help\n"
        "  F3  view the file under the cursor\n"
        "  F4  edit the file under the cursor (vedit)\n"
        "  F5  copy the file to the other panel's directory\n"
        "  F6  move the file to the other panel's directory\n"
        "  F7  create a directory in the current panel\n"
        "  F8  delete the entry (asks first)\n"
        "  F10 / Esc  quit\n";
    run_pager(" vibecommander — help", help, sizeof help - 1);
    set_msg("");
}

static void view_file(const struct Panel* p, const struct Ent* e)
{
    char full[P_CAP];
    entry_path(p, e, full);
    uint32_t n = load_file_into(full, g_view, VIEW_CAP);
    static char t[72];
    int i = 0;
    t[i++] = ' ';
    t[i++] = ' ';
    for (const char* s = e->name; *s && i < 52; ++s)
        t[i++] = *s;
    t[i++] = ' ';
    t[i] = 0;
    run_pager(t, g_view, n);
    set_msg("");
}

/* --- file operations ------------------------------------------------ */

static int copy_bytes(const char* src, const char* dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[256];
    for (;;) {
        long got = read(in, buf, sizeof buf);
        if (got <= 0)
            break;
        write(out, buf, (unsigned long)got);
    }
    close(in);
    close(out);
    return 0;
}

static const struct Ent* sel_entry(const struct Panel* p)
{
    if (p->n > 0 && p->cur < p->n && p->ent[p->cur].type != T_PARENT)
        return &p->ent[p->cur];
    return 0;
}

static void op_copy(int move)
{
    struct Panel* a = &g_pan[g_active];
    struct Panel* b = &g_pan[1 - g_active];
    const struct Ent* e = sel_entry(a);
    if (!e) {
        set_msg("nothing selected to copy");
        return;
    }
    if (e->type == T_DIR) {
        set_msg("copying a directory is not implemented");
        return;
    }
    if (ci_cmp(a->path, b->path) == 0) {
        set_msg("the panels are in the same directory");
        return;
    }
    char src[P_CAP], dst[P_CAP];
    entry_path(a, e, src);
    join_path(b->path, e->name, dst, sizeof dst);
    if (copy_bytes(src, dst) == 0) {
        if (move) {
            unlink(src);
            load_panel(a);
            set_msg("moved");
        } else {
            load_panel(b);
            set_msg("copied");
        }
    } else {
        set_msg("could not copy the file");
    }
}

static void op_mkdir(void)
{
    struct Panel* a = &g_pan[g_active];
    char name[80];
    if (!prompt_line("Make directory: ", name, sizeof name))
        return;
    if (!name[0]) {
        set_msg("");
        return;
    }
    char full[P_CAP];
    join_path(a->path, name, full, sizeof full);
    if (mkdir(full, 0755) == 0) {
        set_msg("created");
        load_panel(a);
    } else {
        set_msg("could not create the directory");
    }
}

static void op_delete(void)
{
    struct Panel* a = &g_pan[g_active];
    if (a->n <= 0 || a->cur >= a->n || a->ent[a->cur].type == T_PARENT) {
        set_msg("nothing to delete here");
        return;
    }
    const struct Ent* e = &a->ent[a->cur];
    char ask[96];
    int i = 0;
    const char* pre = "Delete ";
    while (*pre)
        ask[i++] = *pre++;
    for (const char* s = e->name; *s && i < 78; ++s)
        ask[i++] = *s;
    ask[i++] = ' ';
    ask[i++] = '?';
    ask[i++] = 0;
    if (!confirm_line(ask))
        return;
    char full[P_CAP];
    entry_path(a, e, full);
    int ok = e->type == T_DIR ? rmdir(full) : unlink(full);
    if (ok == 0) {
        set_msg("deleted");
        load_panel(a);
    } else {
        set_msg("could not delete it (is the directory empty?)");
    }
}

/* --- opening ------------------------------------------------------- */

static void enter_parent(struct Panel* p)
{
    char up[P_CAP];
    parent_of(p->path, up);
    strncpy(p->path, up, P_CAP - 1);
    p->path[P_CAP - 1] = 0;
    load_panel(p);
}

static void enter_dir_path(struct Panel* p, const char* full)
{
    strncpy(p->path, full, P_CAP - 1);
    p->path[P_CAP - 1] = 0;
    load_panel(p);
}

static void enter_entry(struct Panel* p)
{
    const struct Ent* e = &p->ent[p->cur];
    if (e->type == T_PARENT) {
        enter_parent(p);
        return;
    }
    if (e->type == T_DIR) {
        char full[P_CAP];
        entry_path(p, e, full);
        enter_dir_path(p, full);
        return;
    }
    view_file(p, e);
}

/* --- editor -------------------------------------------------------- */

static void run_editor(const struct Panel* p, const struct Ent* e)
{
    char full[P_CAP];
    entry_path(p, e, full);
    reset_screen();
    int pid = fork();
    if (pid == 0) {
        char* av[] = {(char*)"/bin/vedit", full, 0};
        execve("/bin/vedit", av, 0);
        exit(1);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
    set_msg("back from the editor");
}

/* --- main loop ----------------------------------------------------- */

static void next_row(int delta)
{
    struct Panel* p = &g_pan[g_active];
    int nx = p->cur + delta;
    if (nx < 0)
        nx = 0;
    if (nx >= p->n)
        nx = p->n - 1;
    p->cur = nx;
    unsigned page = g_rows > 4 ? g_rows - 3 : 2;   /* visible list rows */
    if (p->cur < p->top)
        p->top = p->cur;
    else if (p->cur >= p->top + (int)page)
        p->top = p->cur - (int)page + 1;
}

static void page_list(int down)
{
    unsigned page = g_rows > 4 ? g_rows - 3 : 2;
    next_row(down ? (int)page : -(int)page);
}

static void jump_letter(unsigned char ch)
{
    struct Panel* p = &g_pan[g_active];
    if (ch >= 'a' && ch <= 'z')
        ch = (unsigned char)(ch - 'a' + 'A');
    if (p->n <= 0)
        return;
    int from = (p->cur + 1) % p->n;
    for (int k = 0; k < p->n; ++k) {
        int i = (from + k) % p->n;
        const struct Ent* e = &p->ent[i];
        unsigned char fc = (unsigned char)e->name[0];
        if (fc >= 'a' && fc <= 'z')
            fc = (unsigned char)(fc - 'a' + 'A');
        if (fc == ch) {
            p->cur = i;
            next_row(0);
            return;
        }
    }
    set_msg("no entry starts with that letter");
}

int main(int argc, char** argv)
{
    tnu_size(&g_rows, &g_cols);
    if (g_rows < 8)
        g_rows = 8;
    if (g_cols < 40)
        g_cols = 40;

    const char* start = "/root";
    if (argc > 1 && argv[1] && argv[1][0])
        start = argv[1];
    for (int i = 0; i < 2; ++i) {
        enter_dir_path(&g_pan[i], start);
    }
    set_msg("");

    for (;;) {
        render();
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            break;
        unsigned ch = (unsigned char)raw;

        if (ch == '\t') {   /* Tab: the kbd maps the Tab key to a tab */
            g_active = 1 - g_active;
            set_msg("");
            continue;
        }
        if (ch == '\n' || ch == VNU_KEY_RIGHT) {
            if (g_pan[g_active].n > 0)
                enter_entry(&g_pan[g_active]);
            set_msg("");
            continue;
        }
        if (ch == VNU_KEY_LEFT) {
            enter_parent(&g_pan[g_active]);
            continue;
        }
        if (ch == VNU_KEY_UP) {
            next_row(-1);
            continue;
        }
        if (ch == VNU_KEY_DOWN) {
            next_row(1);
            continue;
        }
        if (ch == VNU_KEY_HOME) {
            g_pan[g_active].cur = 0;
            g_pan[g_active].top = 0;
            continue;
        }
        if (ch == VNU_KEY_END) {
            struct Panel* p = &g_pan[g_active];
            p->cur = p->n - 1;
            next_row(0);
            continue;
        }
        if (ch == ' ') {
            page_list(1);
            continue;
        }

        if (ch == VNU_KEY_F1) {
            help_screen();
            continue;
        }
        if (ch == VNU_KEY_F3) {
            const struct Ent* e = sel_entry(&g_pan[g_active]);
            if (e && e->type == T_FILE)
                view_file(&g_pan[g_active], e);
            else
                set_msg("F3 views a file");
            continue;
        }
        if (ch == VNU_KEY_F4) {
            const struct Ent* e = sel_entry(&g_pan[g_active]);
            if (e && e->type == T_FILE)
                run_editor(&g_pan[g_active], e);
            else
                set_msg("F4 edits a file");
            continue;
        }
        if (ch == VNU_KEY_F5) {
            op_copy(0);
            continue;
        }
        if (ch == VNU_KEY_F6) {
            op_copy(1);
            continue;
        }
        if (ch == VNU_KEY_F7) {
            op_mkdir();
            continue;
        }
        if (ch == VNU_KEY_F8) {
            op_delete();
            continue;
        }
        if (ch == VNU_KEY_F10 || ch == VNU_KEY_ESC) {
            break;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
            ch == '-') {
            set_msg("");
            jump_letter(ch);
            continue;
        }
    }

    reset_screen();
    sgr("0");
    write(1, "\n", 1);
    return 0;
}