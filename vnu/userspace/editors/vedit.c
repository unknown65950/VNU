/*
 * vedit — simple full-screen text editor for VNU (MS-DOS EDIT style).
 *
 * Keys:
 *   arrows / Home / End / Del / Backspace / Enter
 *   Esc     — command line (bottom)
 *   In command mode:
 *     s [file]  — save
 *     o [file]  — open
 *     q         — quit (asks if modified: q! force)
 *     h         — help
 *     Enter / Esc — back to edit
 */
#include <vlibc/unistd.h>
#include <vlibc/stdio.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/keys.h>

#define ROWS 22
#define COLS 78
#define MAX_PATH 64

static char lines[ROWS][COLS + 1];
static int nlines = 1;
static int cur_r = 0, cur_c = 0;
static int dirty = 0;
static char path[MAX_PATH] = "untitled.txt";
static int cmd_mode = 0;
static char cmdbuf[COLS + 1];
static int cmd_len = 0;
static char status[COLS + 1];

static void wstr(const char* s)
{
    if (s)
        write(1, s, strlen(s));
}

static void wch(char c) { write(1, &c, 1); }

static void clear_screen(void)
{
    /* Many newlines + home-ish: tty scrolls; good enough without ANSI. */
    for (int i = 0; i < 30; ++i)
        wch('\n');
}

static void set_status(const char* s)
{
    int i = 0;
    for (; s[i] && i < COLS; ++i)
        status[i] = s[i];
    status[i] = 0;
}

static void clear_buf(void)
{
    for (int r = 0; r < ROWS; ++r) {
        lines[r][0] = 0;
        for (int c = 0; c <= COLS; ++c)
            lines[r][c] = 0;
    }
    nlines = 1;
    cur_r = cur_c = 0;
    dirty = 0;
}

static int load_file(const char* p)
{
    int fd = open(p, 0);
    if (fd < 0)
        return -1;
    clear_buf();
    char buf[2048];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    int r = 0, c = 0;
    for (long i = 0; i < n && r < ROWS; ++i) {
        char ch = buf[i];
        if (ch == '\n' || ch == '\r') {
            lines[r][c] = 0;
            ++r;
            c = 0;
            if (ch == '\r' && i + 1 < n && buf[i + 1] == '\n')
                ++i;
            continue;
        }
        if (c < COLS)
            lines[r][c++] = ch;
    }
    if (r < ROWS)
        lines[r][c] = 0;
    nlines = r + 1;
    if (nlines > ROWS)
        nlines = ROWS;
    if (nlines < 1)
        nlines = 1;
    cur_r = cur_c = 0;
    dirty = 0;
    strncpy(path, p, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    return 0;
}

static int save_file(const char* p)
{
    int fd = open(p, 0x40 | 0x200); /* O_CREAT | O_TRUNC */
    if (fd < 0)
        return -1;
    for (int r = 0; r < nlines; ++r) {
        write(fd, lines[r], strlen(lines[r]));
        if (r + 1 < nlines)
            write(fd, "\n", 1);
    }
    close(fd);
    dirty = 0;
    strncpy(path, p, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    return 0;
}

static void redraw(void)
{
    clear_screen();
    wstr("=== VEDIT — MS-DOS style ===  Esc=cmd  arrows move\n");
    for (int r = 0; r < ROWS; ++r) {
        char num[8];
        /* line number */
        int ln = r + 1;
        num[0] = (char)('0' + (ln / 10) % 10);
        num[1] = (char)('0' + ln % 10);
        num[2] = ' ';
        num[3] = 0;
        wstr(num);
        if (r < nlines)
            wstr(lines[r]);
        wch('\n');
    }
    wstr("--- ");
    wstr(path);
    if (dirty)
        wstr(" *");
    wstr("  ");
    /* row,col */
    {
        char b[32];
        int i = 0;
        b[i++] = 'L';
        int v = cur_r + 1;
        if (v >= 10)
            b[i++] = (char)('0' + v / 10);
        b[i++] = (char)('0' + v % 10);
        b[i++] = ':';
        v = cur_c + 1;
        if (v >= 10)
            b[i++] = (char)('0' + v / 10);
        b[i++] = (char)('0' + v % 10);
        b[i] = 0;
        wstr(b);
    }
    wch('\n');
    if (cmd_mode) {
        wstr("Cmd> ");
        wstr(cmdbuf);
        wch('_');
    } else {
        wstr(status[0] ? status : "Edit mode. Esc for save/quit.");
    }
    wch('\n');
    /* soft cursor marker: reprint current line with ^ under cursor is heavy;
       status shows position instead. */
}

static void insert_char(char ch)
{
    int len = (int)strlen(lines[cur_r]);
    if (len >= COLS)
        return;
    if (cur_c > len)
        cur_c = len;
    for (int i = len; i >= cur_c; --i)
        lines[cur_r][i + 1] = lines[cur_r][i];
    lines[cur_r][cur_c] = ch;
    ++cur_c;
    dirty = 1;
}

static void delete_back(void)
{
    int len = (int)strlen(lines[cur_r]);
    if (cur_c > 0) {
        for (int i = cur_c - 1; i < len; ++i)
            lines[cur_r][i] = lines[cur_r][i + 1];
        --cur_c;
        dirty = 1;
        return;
    }
    if (cur_r > 0) {
        int plen = (int)strlen(lines[cur_r - 1]);
        int clen = (int)strlen(lines[cur_r]);
        if (plen + clen > COLS)
            return;
        for (int i = 0; i < clen; ++i)
            lines[cur_r - 1][plen + i] = lines[cur_r][i];
        lines[cur_r - 1][plen + clen] = 0;
        for (int r = cur_r; r + 1 < nlines; ++r)
            strcpy(lines[r], lines[r + 1]);
        lines[nlines - 1][0] = 0;
        --nlines;
        --cur_r;
        cur_c = plen;
        dirty = 1;
    }
}

static void delete_fwd(void)
{
    int len = (int)strlen(lines[cur_r]);
    if (cur_c < len) {
        for (int i = cur_c; i < len; ++i)
            lines[cur_r][i] = lines[cur_r][i + 1];
        dirty = 1;
    }
}

static void split_line(void)
{
    if (nlines >= ROWS)
        return;
    int len = (int)strlen(lines[cur_r]);
    if (cur_c > len)
        cur_c = len;
    for (int r = nlines; r > cur_r + 1; --r)
        strcpy(lines[r], lines[r - 1]);
    strcpy(lines[cur_r + 1], lines[cur_r] + cur_c);
    lines[cur_r][cur_c] = 0;
    ++nlines;
    ++cur_r;
    cur_c = 0;
    dirty = 1;
}

static void run_cmd(void)
{
    cmdbuf[cmd_len] = 0;
    if (cmd_len == 0) {
        cmd_mode = 0;
        set_status("Edit mode.");
        return;
    }
    if (cmdbuf[0] == 'h') {
        set_status("s [file]=save  o [file]=open  q=quit  q!=force quit");
        cmd_mode = 0;
        cmd_len = 0;
        cmdbuf[0] = 0;
        return;
    }
    if (cmdbuf[0] == 'q') {
        if (dirty && cmdbuf[1] != '!') {
            set_status("Modified! Use q! to quit without save.");
            cmd_mode = 0;
            cmd_len = 0;
            cmdbuf[0] = 0;
            return;
        }
        clear_screen();
        wstr("vedit: bye\n");
        exit(0);
    }
    if (cmdbuf[0] == 's') {
        const char* p = path;
        if (cmdbuf[1] == ' ' && cmdbuf[2])
            p = cmdbuf + 2;
        if (save_file(p) == 0)
            set_status("Saved.");
        else
            set_status("Save failed.");
        cmd_mode = 0;
        cmd_len = 0;
        cmdbuf[0] = 0;
        return;
    }
    if (cmdbuf[0] == 'o') {
        const char* p = cmdbuf[1] == ' ' ? cmdbuf + 2 : path;
        if (!p[0]) {
            set_status("o <file>");
        } else if (load_file(p) == 0)
            set_status("Opened.");
        else {
            clear_buf();
            strncpy(path, p, MAX_PATH - 1);
            set_status("New file.");
        }
        cmd_mode = 0;
        cmd_len = 0;
        cmdbuf[0] = 0;
        return;
    }
    set_status("Unknown cmd. h=help");
    cmd_mode = 0;
    cmd_len = 0;
    cmdbuf[0] = 0;
}

int main(int argc, char** argv)
{
    clear_buf();
    if (argc > 1) {
        if (load_file(argv[1]) != 0) {
            strncpy(path, argv[1], MAX_PATH - 1);
            set_status("New file.");
        } else
            set_status("Loaded.");
    } else {
        set_status("New buffer. Esc then s to save, q to quit.");
    }

    for (;;) {
        redraw();
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            break;
        unsigned char ch = (unsigned char)raw;

        if (cmd_mode) {
            if (ch == VNU_KEY_ESC) {
                cmd_mode = 0;
                cmd_len = 0;
                cmdbuf[0] = 0;
                set_status("Edit mode.");
            } else if (ch == '\n') {
                run_cmd();
            } else if (ch == '\b') {
                if (cmd_len > 0)
                    cmdbuf[--cmd_len] = 0;
            } else if (ch >= 32 && ch < 127 && cmd_len < COLS - 1) {
                cmdbuf[cmd_len++] = ch;
                cmdbuf[cmd_len] = 0;
            }
            continue;
        }

        if (ch == VNU_KEY_ESC) {
            cmd_mode = 1;
            cmd_len = 0;
            cmdbuf[0] = 0;
            set_status("");
            continue;
        }
        if (ch == VNU_KEY_LEFT) {
            if (cur_c > 0)
                --cur_c;
            continue;
        }
        if (ch == VNU_KEY_RIGHT) {
            int len = (int)strlen(lines[cur_r]);
            if (cur_c < len)
                ++cur_c;
            continue;
        }
        if (ch == VNU_KEY_UP) {
            if (cur_r > 0) {
                --cur_r;
                int len = (int)strlen(lines[cur_r]);
                if (cur_c > len)
                    cur_c = len;
            }
            continue;
        }
        if (ch == VNU_KEY_DOWN) {
            if (cur_r + 1 < nlines) {
                ++cur_r;
                int len = (int)strlen(lines[cur_r]);
                if (cur_c > len)
                    cur_c = len;
            }
            continue;
        }
        if (ch == VNU_KEY_HOME) {
            cur_c = 0;
            continue;
        }
        if (ch == VNU_KEY_END) {
            cur_c = (int)strlen(lines[cur_r]);
            continue;
        }
        if (ch == VNU_KEY_DEL) {
            delete_fwd();
            continue;
        }
        if (ch == '\b') {
            delete_back();
            continue;
        }
        if (ch == '\n') {
            split_line();
            continue;
        }
        if (ch >= 32 && ch < 127)
            insert_char(ch);
    }
    return 0;
}
