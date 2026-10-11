/*
 * man — VNU manual pager.
 *
 * Displays the reference manual page for a command. A page is a file:
 * the system's own live under /etc/vnu/manpages/<name> (the sources are
 * in vnu/userspace/man/pages/, installed there at build time) and a
 * third-party module keeps its page at /apps/<name>/man/<name> (see
 * thams/README.md). This binary reads the one it is asked for off the
 * VFS, and reads the directory itself to list what is documented, so a
 * page can be added without rebuilding it.
 *
 * On a terminal the page is paged: the arrow keys scroll it a line at a
 * time, other keys scroll a screen or jump to an end, `h` lists them all
 * and `q` goes back to the shell. A white status bar on the last row says
 * which page and line you are on. When stdout is not a terminal (a
 * redirect, a test) the page is simply printed, so `man ls > file` and
 * the test harness keep working.
 *
 * Usage:
 *     man              list every documented command
 *     man -l           same as above
 *     man NAME [NAME]  print the page(s) for NAME
 *     man -h           this help
 *
 * POLICY — READ THIS IF YOU ADD A COMMAND:
 * Every user-visible command in the system (vash builtins, coreutils and
 * standalone binaries like vedit) MUST have a page before the feature is
 * considered done. A page is one file in vnu/userspace/man/pages/; the
 * build installs it at /etc/vnu/manpages/<name>, which is where this
 * binary reads it. The rule lives in AGENTS.md at the repository root;
 * new commands get rejected in review without a `man <cmd>` page.
 */
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/keys.h>
#include <vlibc/dirent.h>
#include <vlibc/term.h>

static void w(const char* s)
{
    if (s)
        write(1, s, strlen(s));
}

static void we(const char* s)
{
    if (s)
        write(2, s, strlen(s));
}

/* --- pages that are files ------------------------------------------ */

/* A page is a file. The system's own pages are mounted by the kernel
 * under /etc/vnu/manpages/<name> (the sources are in
 * vnu/userspace/man/pages/, installed there at build time); a
 * third-party module (see thams/README.md) keeps its page next to its
 * binary at /apps/<name>/man/<name>, which is the whole point of
 * keeping the module out of the source: the OS is not edited to carry
 * somebody else's documentation. Either way reading it is all this has
 * to do.
 *
 * The page is the printed text as it stands, with no C string quoting. */
#define MAX_PAGE_BYTES 16384
#define MAN_DIR "/etc/vnu/manpages"

static char* read_whole_file(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char* buf = (char*)malloc(MAX_PAGE_BYTES);
    if (!buf) {
        close(fd);
        return 0;
    }
    /* A read may come back short, so the page is read until it ends or
     * the buffer is full — the last byte is left for the terminator. */
    int got = 0;
    while (got < MAX_PAGE_BYTES - 1) {
        int chunk = (int)read(fd, buf + got, MAX_PAGE_BYTES - 1 - got);
        if (chunk <= 0)
            break;
        got += chunk;
    }
    close(fd);
    if (got <= 0) {
        free(buf);
        return 0;
    }
    buf[got] = 0;
    return buf;
}

/* A module name is a directory name, and the one character that would
 * turn it into a path is a slash: without it the page can only come from
 * the directory it belongs in. Control characters are refused so the
 * name cannot put anything on the terminal that was not there to begin
 * with. */
static int page_name_ok(const char* name)
{
    if (!name[0] || strlen(name) > 48)
        return 0;
    for (const char* s = name; *s; ++s)
        if (*s == '/' || *s == '\\' || *s < ' ')
            return 0;
    return 1;
}

/* Append `s` to `out` (capacity `cap`), always terminator-safe, and
 * return the new length. Paths are built piece by piece so the page
 * name is the only part that comes from the user. */
static unsigned append(char* out, unsigned at, unsigned cap, const char* s)
{
    for (; *s && at + 1 < cap; ++s)
        out[at++] = *s;
    out[at] = 0;
    return at;
}

/* The page for `name`: what the system ships first, then what a module
 * ships. Returns a heap buffer (free it) or 0 when there is no page. */
static char* load_page_file(const char* name)
{
    if (!page_name_ok(name))
        return 0;
    char path[128];
    unsigned at = append(path, 0, sizeof(path), MAN_DIR);
    at = append(path, at, sizeof(path), "/");
    append(path, at, sizeof(path), name);
    char* text = read_whole_file(path);
    if (text)
        return text;

    at = append(path, 0, sizeof(path), "/apps/");
    at = append(path, at, sizeof(path), name);
    at = append(path, at, sizeof(path), "/man/");
    append(path, at, sizeof(path), name);
    return read_whole_file(path);
}

/* --- the listing ------------------------------------------------- */

static const char* const aliases[] = { "a", "all", "list", "-l", "--list", NULL };

static int is_alias(const char* a)
{
    for (int i = 0; aliases[i]; ++i)
        if (strcmp(a, aliases[i]) == 0)
            return 1;
    return 0;
}

static void usage(void)
{
    w("usage: man [NAME...]\n");
    w("       man -l | --list      list all documented commands\n");
    w("       man -h | --help      this help\n");
    w("\nOn a terminal a page is paged: Up/Down scroll, h lists the\n");
    w("keys, q quits. Redirected output is printed as is.\n");
}

/* The one-line listing summary is the page's own NAME line: the text
 * after the " - " that separates the name from its description. */
static void w_summary(const char* text)
{
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n')
            ++e;
        if (e - p == 4 && p[0] == 'N' && p[1] == 'A' && p[2] == 'M' &&
            p[3] == 'E') {
            const char* q = (*e == '\n') ? e + 1 : e;
            while (*q == '\n')
                ++q;
            const char* qe = q;
            while (*qe && *qe != '\n')
                ++qe;
            while (q < qe && (*q == ' ' || *q == '\t'))
                ++q;
            for (const char* s = q; s + 2 < qe; ++s)
                if (s[0] == ' ' && s[1] == '-' && s[2] == ' ') {
                    q = s + 3;
                    break;
                }
            write(1, q, (unsigned long)(qe - q));
            return;
        }
        p = (*e == '\n') ? e + 1 : e;
    }
}

static void list_all(void)
{
    w("VNU manual pages:\n\n");
    DIR* d = opendir(MAN_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != 0) {
            if (e->d_name[0] == '.')
                continue;
            char* text = load_page_file(e->d_name);
            w("    ");
            w(e->d_name);
            w("  -  ");
            if (text)
                w_summary(text);
            w("\n");
            free(text);
        }
        closedir(d);
    }
    w("\nType 'man NAME' for the page of NAME.\n");
}

/* --- the pager --------------------------------------------------------
 *
 * Every page is a heap buffer read off the VFS (the system's under
 * /etc/vnu/manpages, a module's under /apps/<name>/man), and the
 * document indexes it in place rather than copying: a display line is a
 * (pointer, length) pair into that text, cut at every '\n' and wrapped
 * at the screen width, so nothing is ever truncated.
 */

struct Line {
    const char* p;
    unsigned len;
    unsigned page; /* which requested page the line came from */
};

static const char* g_names[32]; /* page names, in the order given */
static unsigned g_npages;
static struct Line* g_lines;
static unsigned g_nlines, g_capacity;
static unsigned g_top; /* first display line on screen */
static unsigned g_rows = TNU_ROWS, g_cols = TNU_COLS;
static char g_pad[TNU_COLS + 8]; /* spaces, to fill the status bar */

/* The three fixed pieces of the status line, kept as strings so the
 * padding arithmetic below cannot drift from what is printed. */
static const char* const S_PRE = "Manual page ";
static const char* const S_MID = "(1) line ";
static const char* const S_POST = " (press h for help or q to quit)";

static void seq_goto(unsigned row, unsigned col)
{
    char q[24];
    int n = tnu_goto(q, sizeof q, row, col);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static void seq_sgr(const char* params)
{
    char q[16];
    int n = tnu_sgr(q, sizeof q, params);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static void seq_erase_eol(void)
{
    char q[8];
    int n = tnu_erase_eol(q, sizeof q);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static unsigned ndigits(unsigned v)
{
    unsigned n = 1;
    while (v >= 10) {
        v /= 10;
        ++n;
    }
    return n;
}

static int doc_reserve(unsigned extra)
{
    unsigned need = g_nlines + extra;
    if (need <= g_capacity)
        return 0;
    unsigned cap = g_capacity ? g_capacity : 256;
    while (cap < need)
        cap *= 2;
    struct Line* nl = (struct Line*)malloc(sizeof(struct Line) * cap);
    if (!nl)
        return -1;
    if (g_lines) {
        memcpy(nl, g_lines, sizeof(struct Line) * g_nlines);
        free(g_lines);
    }
    g_lines = nl;
    g_capacity = cap;
    return 0;
}

/* Cut `text` into display lines and append them to the document. */
static int doc_add(const char* text, unsigned page)
{
    const char* cur = text;
    for (;;) {
        const char* nl = cur;
        while (*nl != 0 && *nl != '\n')
            ++nl;
        unsigned len = (unsigned)(nl - cur);
        unsigned off = 0;
        do {
            unsigned take = len - off;
            if (take > g_cols)
                take = g_cols;
            if (doc_reserve(1) != 0)
                return -1;
            g_lines[g_nlines].p = cur + off;
            g_lines[g_nlines].len = take;
            g_lines[g_nlines].page = page;
            ++g_nlines;
            off += take;
        } while (off < len);
        if (*nl == 0)
            break;
        cur = nl + 1;
    }
    return 0;
}

static void draw_page(unsigned view)
{
    for (unsigned r = 0; r < view; ++r) {
        seq_goto(r + 1, 1);
        unsigned i = g_top + r;
        if (i < g_nlines) {
            write(1, g_lines[i].p, g_lines[i].len);
            seq_erase_eol();
        } else {
            /* Past the end of the document: leave the row blank. */
            seq_sgr("0");
            seq_erase_eol();
        }
    }
}

static const char* const HELP[] = {
    "man - how to move around a page",
    "",
    "  Up, k            scroll up one line",
    "  Down, j          scroll down one line",
    "  space, f, Enter  scroll down one screen",
    "  b, u             scroll up one screen",
    "  Home, g          first line of the page",
    "  End, G           last line of the page",
    "  h                this help",
    "  q                quit, back to the shell",
    "",
    "The arrow keys move one line; the rest are shortcuts. q and h also",
    "work on this screen - any other key returns to the page.",
    0
};

static void draw_help(unsigned view)
{
    unsigned r = 0;
    for (; r < view && HELP[r] != 0; ++r) {
        seq_goto(r + 1, 1);
        write(1, HELP[r], strlen(HELP[r]));
        seq_erase_eol();
    }
    for (; r < view; ++r) {
        seq_goto(r + 1, 1);
        seq_sgr("0");
        seq_erase_eol();
    }
}

/* The white bar on the last row: which page, which line, what to press.
 * The cursor is parked on the last cell, since the console has no
 * hide-cursor sequence. */
static void draw_status(void)
{
    unsigned first = g_top + 1;
    unsigned name = 0;
    if (g_nlines > 0 && g_top < g_nlines && g_names[0] != 0)
        name = g_lines[g_top].page;
    const char* nm = g_names[name] ? g_names[name] : "?";

    seq_goto(g_rows, 1);
    seq_sgr("0;30;47");
    printf("%s%s%s%u%s", S_PRE, nm, S_MID, first, S_POST);
    unsigned used = (unsigned)(strlen(S_PRE) + strlen(nm) + strlen(S_MID) +
                               ndigits(first) + strlen(S_POST));
    unsigned fill = used + 1 < g_cols ? g_cols - used - 1 : 0;
    if (fill > sizeof g_pad)
        fill = sizeof g_pad;
    if (fill > 0)
        write(1, g_pad, (unsigned long)fill);
    seq_goto(g_rows, g_cols);
    seq_sgr("0");
}

static void draw(int help)
{
    unsigned view = g_rows - 1; /* the last row belongs to the status bar */
    seq_sgr("0");
    if (help)
        draw_help(view);
    else
        draw_page(view);
    draw_status();
}

static void run_pager(void)
{
    int help = 0;

    tnu_size(&g_rows, &g_cols);
    if (g_rows < 8) /* room for a status bar plus a little text */
        g_rows = 8;
    memset(g_pad, ' ', sizeof g_pad);
    /* Scrolling never goes past the end: the last screen of the document
     * sits at the bottom of the window, like less does. */
    unsigned view = g_rows - 1;
    unsigned max_top = g_nlines > view ? g_nlines - view : 0;

    draw(help);
    for (;;) {
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            break;
        unsigned ch = (unsigned char)raw;

        if (ch == VNU_KEY_INTR || ch == VNU_KEY_ESC || ch == 'Q')
            break;

        if (help) {
            if (ch == 'q' || ch == 'Q' || ch == 'h' || ch == 'H')
                break;
            help = 0; /* any other key: back to the page */
            draw(help);
            continue;
        }
        if (ch == 'q')
            break;
        if (ch == 'h' || ch == 'H') {
            help = 1;
            draw(help);
            continue;
        }

        if (ch == VNU_KEY_UP || ch == 'k') {
            if (g_top > 0)
                --g_top;
        } else if (ch == VNU_KEY_DOWN || ch == 'j') {
            if (g_top < max_top)
                ++g_top;
        } else if (ch == ' ' || ch == '\n' || ch == 'f' || ch == 6 /* ^F */ ||
                   ch == 4 /* ^D */) {
            g_top += view;
            if (g_top > max_top)
                g_top = max_top;
        } else if (ch == 'b' || ch == 'u' || ch == 2 /* ^B */) {
            g_top = g_top > view ? g_top - view : 0;
        } else if (ch == VNU_KEY_HOME || ch == 'g') {
            g_top = 0;
        } else if (ch == VNU_KEY_END || ch == 'G') {
            g_top = max_top;
        } else {
            continue; /* an unbound key changes nothing */
        }
        draw(help);
    }
    /* Leave the shell a clean prompt: the newline scrolls the bar off the
     * bottom and the cursor lands where vash continues. */
    seq_sgr("0");
    write(1, "\n", 1);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        list_all();
        return 0;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (is_alias(argv[1])) {
        list_all();
        return 0;
    }
    int rc = 0;
    /* On a terminal the pages are shown one screen at a time; otherwise
     * they are printed straight out, so a redirect keeps working. */
    int interactive = isatty(1) == 1;
    /* Every page is read off the VFS: the system's from
     * /etc/vnu/manpages/<name>, a module's from /apps/<name>/man/<name>.
     * The pager keeps pointers into the buffers rather than copies, so
     * they are held until it has finished — the same cap as g_names,
     * since both grow once per page asked for. */
    static char* file_pages[32];
    unsigned n_file_pages = 0;

    for (int i = 1; i < argc; ++i) {
        char* own = load_page_file(argv[i]);
        if (!own) {
            we("man: no manual page for ");
            we(argv[i]);
            we("\n");
            rc = 1;
            continue;
        }
        if (interactive) {
            /* One document holds every page asked for, so the status bar
             * can name the one on screen; the array of names is the cap. */
            if (g_npages >= sizeof(g_names) / sizeof(g_names[0]) ||
                n_file_pages >= sizeof(file_pages) / sizeof(file_pages[0])) {
                we("man: too many pages requested\n");
                return 1;
            }
            g_names[g_npages++] = argv[i];
            if (doc_add(own, g_npages - 1) != 0) {
                we("man: out of memory building the page\n");
                return 1;
            }
            file_pages[n_file_pages++] = own;
        } else {
            w(own);
            w("\n");
            free(own);
        }
    }
    if (interactive && g_npages > 0)
        run_pager();
    for (unsigned i = 0; i < n_file_pages; ++i)
        free(file_pages[i]);
    return rc;
}
