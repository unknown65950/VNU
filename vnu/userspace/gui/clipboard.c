/*
 * clipboard - the system clipboard, in a window.
 *
 * Every program on the machine shares one piece of text, and it lives
 * in the /dev/clipboard character device (the kernel's clipboard.cpp).
 * Ctrl+C in the other apps puts a line there and Ctrl+V reads it back;
 * this window is the same clipboard made visible. It reads the text
 * when it opens, lets you read and change it with the small editor a
 * note needs, and writes it back when you ask: the copy button stores
 * what is on screen, reload throws the screen away and reads the
 * clipboard again, and clear empties both.
 *
 * The point of the window is that the thing every app shares can be
 * seen rather than guessed at: what Ctrl+V will paste is on screen,
 * and a script's `cat /dev/clipboard` is the same text this window is
 * editing.
 *
 * Keys: printable keys type, Enter a new line, Backspace deletes
 * backwards, Delete forwards, the arrows and Home/End move the caret,
 * Esc closes the window (storing an edit that has not been copied
 * yet). Ctrl+C copies the line the caret is on, Ctrl+X cuts it,
 * Ctrl+V pastes the clipboard at the caret, and Ctrl+A copies the
 * whole buffer back to the clipboard - the same keys the other apps
 * use, so what is learned here carries.
 */
#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/fcntl.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/clipboard.h>

/* --- geometry ------------------------------------------------------- */

#define PAD_X 8                 /* left and right margin */
#define PAD_TOP 26              /* under the title strip */
#define STATUS_H 17             /* dark status strip at the bottom */
#define CW 8                    /* character cell width */
#define CH 16                   /* character cell height */
#define BOTTOM_BUTTONS 26       /* button row above the status strip */
#define BTN_Y (H - STATUS_H - BOTTOM_BUTTONS)
#define BTN_H 18

/* --- the buffer ----------------------------------------------------- */

/* The clipboard holds up to VCLIP_CAP (64 KiB). The window shows all
 * of it that fits the grid: 1024 lines of 64 columns, which is the
 * whole device, newlines included. */
#define MAX_LINES 1024
#define COLS 64

static char lines[MAX_LINES][COLS + 1];
static int nlines = 1;
static int cur_r, cur_c;       /* the caret */
static int top;                /* first line drawn */
static int left;               /* first column drawn */
static int dirty;              /* changed since the last store */
static int caret_on = 1;
static char msg[24];           /* last action, or empty */

/* What the kernel granted, measured once. */
static int W = VGFX_W, H = VGFX_H;

/* --- palette roles -------------------------------------------------- */

#define C_BG     VGFX_DGRAY     /* the editing surface */
#define C_RULE   VGFX_BLACK     /* its ruled lines */
#define C_INK    VGFX_WHITE     /* what is written on it */

/* --- the device ----------------------------------------------------- */

static void set_msg(const char* s)
{
    int i = 0;
    for (; s[i] && i < (int)sizeof(msg) - 1; ++i)
        msg[i] = s[i];
    msg[i] = 0;
}

/* Read the clipboard into the buffer. A short read or a line longer
 * than a row is taken as far as it goes; the buffer is the largest
 * thing the device can hold, so the only thing that can be lost is a
 * byte beyond the device's own limit. */
static void load_clip(void)
{
    int fd = open(VCLIP_DEV, O_RDONLY);
    if (fd < 0) {
        set_msg("clipboard error");
        return;
    }

    static char buf[VCLIP_CAP];
    int n = 0;
    for (;;) {
        int chunk = (int)sizeof(buf) - n;
        if (chunk <= 0)
            break;
        int got = (int)read(fd, buf + n, (unsigned long)chunk);
        if (got <= 0)
            break;
        n += got;
    }
    close(fd);
    if (n <= 0) {
        lines[0][0] = 0;
        nlines = 1;
        cur_r = cur_c = top = left = 0;
        dirty = 0;
        return;
    }

    nlines = 0;
    int col = 0;
    lines[0][0] = 0;
    for (int i = 0; i < n && nlines < MAX_LINES; ++i) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            lines[nlines][col] = 0;
            ++nlines;
            col = 0;
            if (nlines < MAX_LINES)
                lines[nlines][0] = 0;
            continue;
        }
        if (col < COLS)
            lines[nlines][col++] = c;
        lines[nlines][col] = 0;
    }
    if (nlines == 0)
        nlines = 1;
    cur_r = cur_c = top = left = 0;
    dirty = 0;
}

/* Write the buffer back. Opening the device is what clears it, and
 * each write appends, so this is the whole replace. Returns 1 on
 * success. */
static int store_clip(void)
{
    int fd = open(VCLIP_DEV, O_WRONLY);
    if (fd < 0)
        return 0;
    for (int i = 0; i < nlines; ++i) {
        int len = (int)strlen(lines[i]);
        if (len > 0 && write(fd, lines[i], (unsigned long)len) < 0) {
            close(fd);
            return 0;
        }
        if (i + 1 < nlines && write(fd, "\n", 1) < 0) {
            close(fd);
            return 0;
        }
    }
    close(fd);
    dirty = 0;
    return 1;
}

/* How many bytes the buffer holds as text, newlines included: what the
 * clipboard will be once copy is pressed. */
static int buf_bytes(void)
{
    int n = 0;
    for (int i = 0; i < nlines; ++i) {
        n += (int)strlen(lines[i]);
        if (i + 1 < nlines)
            ++n;
    }
    return n;
}

/* --- editing -------------------------------------------------------- */

static void clamp_caret(void)
{
    if (cur_r < 0)
        cur_r = 0;
    if (cur_r >= nlines)
        cur_r = nlines - 1;
    int len = (int)strlen(lines[cur_r]);
    if (cur_c > len)
        cur_c = len;
    if (cur_c < 0)
        cur_c = 0;
}

static void scroll_to_caret(void)
{
    int rows = (BTN_Y - PAD_TOP) / CH;
    if (rows < 1)
        rows = 1;
    if (cur_r < top)
        top = cur_r;
    if (cur_r >= top + rows)
        top = cur_r - rows + 1;

    int cols = (W - 2 * PAD_X) / CW;
    if (cols < 1)
        cols = 1;
    if (cur_c < left)
        left = cur_c;
    if (cur_c >= left + cols)
        left = cur_c - cols + 1;

    if (top > nlines - 1)
        top = nlines - 1;
    if (top < 0)
        top = 0;
    if (left < 0)
        left = 0;
}

static void insert_char(char ch)
{
    int len = (int)strlen(lines[cur_r]);
    if (len >= COLS)
        return;
    for (int i = len; i >= cur_c; --i)
        lines[cur_r][i + 1] = lines[cur_r][i];
    lines[cur_r][cur_c] = ch;
    ++cur_c;
    dirty = 1;
}

static void split_line(void)
{
    if (nlines >= MAX_LINES)
        return;
    int len = (int)strlen(lines[cur_r]);
    int tail = len - cur_c;
    for (int r = nlines; r > cur_r; --r)
        for (int i = 0; i <= COLS; ++i)
            lines[r][i] = lines[r - 1][i];
    ++nlines;
    for (int i = 0; i < tail; ++i)
        lines[cur_r + 1][i] = lines[cur_r][cur_c + i];
    lines[cur_r + 1][tail] = 0;
    lines[cur_r][cur_c] = 0;
    cur_r++;
    cur_c = 0;
    dirty = 1;
}

static void join_line(void)
{
    if (cur_r == 0 || cur_r + 1 >= nlines)
        return;
    int len = (int)strlen(lines[cur_r]);
    int tail = (int)strlen(lines[cur_r + 1]);
    if (len + tail > COLS)
        return;
    for (int i = 0; i < tail; ++i)
        lines[cur_r][len + i] = lines[cur_r + 1][i];
    lines[cur_r][len + tail] = 0;
    for (int r = cur_r + 1; r < nlines - 1; ++r)
        for (int i = 0; i <= COLS; ++i)
            lines[r][i] = lines[r + 1][i];
    --nlines;
    dirty = 1;
}

static void delete_back(void)
{
    if (cur_c > 0) {
        int len = (int)strlen(lines[cur_r]);
        for (int i = cur_c - 1; i < len; ++i)
            lines[cur_r][i] = lines[cur_r][i + 1];
        --cur_c;
        dirty = 1;
        return;
    }
    join_line();
}

static void delete_fwd(void)
{
    int len = (int)strlen(lines[cur_r]);
    if (cur_c < len) {
        for (int i = cur_c; i < len; ++i)
            lines[cur_r][i] = lines[cur_r][i + 1];
        dirty = 1;
        return;
    }
    if (cur_r + 1 >= nlines)
        return;
    join_line();
}

static void delete_line(void)
{
    if (nlines <= 1) {
        lines[0][0] = 0;
        cur_c = 0;
    } else {
        for (int r = cur_r; r < nlines - 1; ++r)
            for (int i = 0; i <= COLS; ++i)
                lines[r][i] = lines[r + 1][i];
        --nlines;
        if (cur_r >= nlines)
            cur_r = nlines - 1;
        cur_c = 0;
    }
    dirty = 1;
    clamp_caret();
}

/* --- clipboard from the editor ------------------------------------- */

/* The line the caret is on, newline included: the unit Ctrl+C works
 * on, the same one the other apps use. */
static void copy_line(void)
{
    char buf[COLS + 2];
    int len = (int)strlen(lines[cur_r]);
    for (int i = 0; i < len; ++i)
        buf[i] = lines[cur_r][i];
    buf[len] = '\n';
    vclip_set(buf, (unsigned long)(len + 1));
    set_msg("line copied");
}

static void cut_line(void)
{
    copy_line();
    delete_line();
    set_msg("line cut");
}

static void paste(void)
{
    int fd = open(VCLIP_DEV, O_RDONLY);
    if (fd < 0) {
        set_msg("clipboard error");
        return;
    }
    char buf[256];
    long total = 0;
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n')
                split_line();
            else if (c >= 0x20)
                insert_char(c);
        }
        total += n;
    }
    close(fd);
    set_msg(total > 0 ? "pasted" : "clipboard empty");
}

/* --- drawing -------------------------------------------------------- */

/* A small decimal formatter; there is no printf in the app's libc. */
static void fmt_int(char* out, int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0) {
        *out++ = '-';
        v = -v;
    }
    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0)
        *out++ = tmp[--n];
    *out = 0;
}

static void draw(void)
{
    int w = W, h = H;

    vgfx_clear(C_BG);

    /* Ruled lines, so a row is somewhere to write on. */
    int rows = (BTN_Y - PAD_TOP) / CH;
    for (int i = 0; i <= rows && PAD_TOP + i * CH < BTN_Y; ++i)
        vgfx_hline(PAD_X, PAD_TOP + i * CH + CH - 1, w - 2 * PAD_X, C_RULE);

    /* Title strip: what this is and which device it is. */
    vgfx_fill_rect(0, 0, w, PAD_TOP - 4, VGFX_LBLUE);
    vgfx_str(PAD_X, 5, "clipboard", VGFX_BLACK);
    vgfx_str(w - PAD_X - vgfx_text_width(VCLIP_DEV), 5, VCLIP_DEV, VGFX_BLACK);

    /* The text. */
    for (int i = 0; i < rows; ++i) {
        int r = top + i;
        if (r >= nlines)
            break;
        int len = (int)strlen(lines[r]);
        for (int c = 0; left + c < len; ++c) {
            int x = PAD_X + c * CW;
            if (x + CW > w - PAD_X)
                break;
            vgfx_char(x, PAD_TOP + i * CH, lines[r][left + c], C_INK);
        }
    }

    /* The caret, on its blink. */
    if (caret_on && cur_r >= top && cur_r < top + rows) {
        int x = PAD_X + (cur_c - left) * CW;
        int y = PAD_TOP + (cur_r - top) * CH;
        if (x + CW <= w - PAD_X)
            vgfx_fill_rect(x, y + 2, 2, CH - 4, C_INK);
    }

    /* Buttons. */
    vgfx_fill_rect(PAD_X, BTN_Y, 58, BTN_H, VGFX_LGRAY);
    vgfx_str(PAD_X + 6, BTN_Y + 1, "copy", VGFX_BLACK);
    vgfx_fill_rect(PAD_X + 66, BTN_Y, 74, BTN_H, VGFX_LGRAY);
    vgfx_str(PAD_X + 72, BTN_Y + 1, "reload", VGFX_BLACK);
    vgfx_fill_rect(PAD_X + 148, BTN_Y, 66, BTN_H, VGFX_LGRAY);
    vgfx_str(PAD_X + 154, BTN_Y + 1, "clear", VGFX_BLACK);

    /* Status strip: the state of the buffer on the left, its size on
     * the right. */
    vgfx_fill_rect(0, h - STATUS_H, w, STATUS_H, VGFX_BLACK);
    vgfx_str(PAD_X, h - STATUS_H + 1,
             msg[0] ? msg : (dirty ? "not copied - press copy"
                                   : "in the clipboard"),
             dirty && !msg[0] ? VGFX_YELLOW : VGFX_WHITE);
    char num[24];
    char tail[24];
    fmt_int(num, buf_bytes());
    int ti = 0;
    for (const char* p = num; *p; ++p)
        tail[ti++] = *p;
    const char* unit = " bytes";
    for (const char* p = unit; *p && ti < (int)sizeof(tail) - 1; ++p)
        tail[ti++] = *p;
    tail[ti] = 0;
    vgfx_str(w - PAD_X - vgfx_text_width(tail), h - STATUS_H + 1, tail,
             VGFX_WHITE);
}

static int hit_button(int x, int y, int* which)
{
    if (y < BTN_Y || y >= BTN_Y + BTN_H)
        return 0;
    if (x >= PAD_X && x < PAD_X + 58) {
        *which = 1;
        return 1;
    }
    if (x >= PAD_X + 66 && x < PAD_X + 66 + 74) {
        *which = 2;
        return 1;
    }
    if (x >= PAD_X + 148 && x < PAD_X + 148 + 66) {
        *which = 3;
        return 1;
    }
    return 0;
}

static void clear_all(void)
{
    lines[0][0] = 0;
    nlines = 1;
    cur_r = cur_c = top = left = 0;
    dirty = 0;
    vclip_clear();
    set_msg("cleared");
}

static void copy_all(void)
{
    if (store_clip())
        set_msg("copied to clipboard");
    else
        set_msg("clipboard error");
}

int main(void)
{
    (void)vgfx_canvas(VGFX_W, VGFX_H);
    vgfx_size(&W, &H);
    load_clip();
    clamp_caret();
    scroll_to_caret();
    draw();
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == VNU_KEY_ESC) {
                if (dirty)
                    (void)store_clip();
                exit(0);
            }
            if (ev.key != 0x06)
                msg[0] = 0;
            if (ev.key == 0x06) {
                caret_on = !caret_on;
            } else if (ev.key == 0x03) {      /* Ctrl+C */
                copy_line();
            } else if (ev.key == 0x18) {      /* Ctrl+X */
                cut_line();
            } else if (ev.key == 0x16) {      /* Ctrl+V */
                paste();
            } else if (ev.key == 0x01) {      /* Ctrl+A */
                copy_all();
            } else if (ev.key == '\r' || ev.key == '\n') {
                split_line();
            } else if (ev.key == 0x08) {
                delete_back();
            } else if (ev.key == VNU_KEY_DEL) {
                delete_fwd();
            } else if (ev.key == VNU_KEY_LEFT) {
                if (cur_c > 0)
                    --cur_c;
                else if (cur_r > 0) {
                    --cur_r;
                    cur_c = (int)strlen(lines[cur_r]);
                }
            } else if (ev.key == VNU_KEY_RIGHT) {
                if (cur_c < (int)strlen(lines[cur_r]))
                    ++cur_c;
                else if (cur_r + 1 < nlines) {
                    ++cur_r;
                    cur_c = 0;
                }
            } else if (ev.key == VNU_KEY_UP) {
                if (cur_r > 0)
                    --cur_r;
            } else if (ev.key == VNU_KEY_DOWN) {
                if (cur_r + 1 < nlines)
                    ++cur_r;
            } else if (ev.key == VNU_KEY_HOME) {
                cur_c = 0;
            } else if (ev.key == VNU_KEY_END) {
                cur_c = (int)strlen(lines[cur_r]);
            } else if (ev.key >= 0x20 && ev.key <= 0x7E) {
                insert_char((char)ev.key);
            }
        } else if (ev.type == VGFX_EV_PRESS) {
            int which = 0;
            if (!hit_button(ev.x, ev.y, &which)) {
                if (ev.x >= PAD_X && ev.y >= PAD_TOP && ev.y < BTN_Y) {
                    cur_r = top + (ev.y - PAD_TOP) / CH;
                    cur_c = left + (ev.x - PAD_X) / CW;
                    clamp_caret();
                }
            } else if (which == 1) {
                copy_all();
            } else if (which == 2) {
                load_clip();
                clamp_caret();
                scroll_to_caret();
                set_msg("reloaded");
            } else {
                clear_all();
            }
        }
        clamp_caret();
        scroll_to_caret();
        draw();
        vgfx_flush();
    }
}
