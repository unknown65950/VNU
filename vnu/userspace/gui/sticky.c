/*
 * sticky - a sticky note for the VNU desktop.
 *
 * A window holding one note: type into it, and it is kept. The note
 * lives in a file under the user's home, so closing the window and
 * opening it again - or restarting the whole desktop - gives back the
 * text that was last written, not an empty page. That is the whole
 * point of the program, so the write is not something to remember to
 * do: the note is marked as needing a save whenever it changes and
 * written on the next heartbeat, which is within a second, and again
 * on the way out. The status line says which of the two you are
 * looking at, so a note that has not been written yet cannot be
 * mistaken for one that has.
 *
 * The editor is the small one a note needs rather than the one
 * vedit is: printable keys, Enter, Backspace, Delete, the arrows,
 * Home and End. Lines longer than the window are scrolled sideways
 * so the caret stays on screen, and a note longer than the window is
 * scrolled vertically to keep the line being edited in view.
 *
 * Keys: printable keys type, Enter a new line, Backspace deletes
 * backwards, VNU_KEY_DEL forwards, arrows and Home/End move, Esc
 * closes the window.
 */
#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/fcntl.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>

/* --- geometry ------------------------------------------------------- */

#define PAD_X 8                 /* left and right margin */
#define PAD_TOP 26              /* under the title strip */
#define STATUS_H 17             /* dark status strip at the bottom */
#define CW 8                    /* character cell width */
#define CH 16                   /* character cell height */
#define BOTTOM_BUTTONS 26       /* button row above the status strip */
#define BTN_Y (H - STATUS_H - BOTTOM_BUTTONS)
#define BTN_H 18

/* --- the note ------------------------------------------------------- */

/* Big enough for a note worth keeping: 400 lines of 64 columns is a
 * page of prose rather than a reminder, and the file it is saved to
 * is written from here. */
#define MAX_LINES 400
#define COLS 64

static char lines[MAX_LINES][COLS + 1];
static int nlines = 1;
static int cur_r, cur_c;       /* the caret */
static int top;                /* first line drawn */
static int left;               /* first column drawn */
static int dirty;              /* changed since the last write */
static int caret_on = 1;

/* What the kernel granted, measured once: every primitive below is
 * laid out from this rather than from the header's default, because
 * the window is not the size this file assumes. */
static int W = VGFX_W, H = VGFX_H;

/* --- palette roles (Catppuccin Mocha lives in the 16 VGA slots) ----- */

#define C_PAPER  VGFX_YELLOW   /* the note itself */
#define C_PAPER2 VGFX_BROWN    /* its ruled lines */
#define C_INK    VGFX_BLACK    /* what is written on it */
#define C_TITLE  VGFX_LGRAY
#define C_TXT    VGFX_WHITE
#define C_ACCENT VGFX_LBLUE
#define C_DIM    VGFX_DGRAY

/* --- the file the note is kept in ----------------------------------- */

/* One note per user, in that user's home, so the desktop that starts
 * a session is the session that gets its text back. /root is where
 * the demo user's shell lands and where the desktop keeps its own
 * per-user files; the name is dotted so it does not clutter an ls of
 * the home directory. */
static const char* const SAVE_PATH = "/root/.sticky-note";

/* Read the note back, if there is one. A missing file is the normal
 * first run, not an error: the note is simply empty. A short read or
 * a line longer than a row is taken as far as it goes rather than
 * refused, so a note written by a later version still opens. */
static void load_note(void)
{
    int fd = open(SAVE_PATH, O_RDONLY);
    if (fd < 0)
        return;

    static char buf[sizeof(lines)];
    int n = 0;
    for (;;) {
        int chunk = (int)sizeof(buf) - 1 - n;
        if (chunk <= 0)
            break;
        int got = (int)read(fd, buf + n, (unsigned long)chunk);
        if (got <= 0)
            break;
        n += got;
    }
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;

    nlines = 0;
    int col = 0;
    for (int i = 0; i <= n && nlines < MAX_LINES; ++i) {
        char c = buf[i];
        if (i == n || c == '\n' || c == '\r') {
            lines[nlines][col] = 0;
            ++nlines;
            col = 0;
            continue;
        }
        if (col < COLS)
            lines[nlines][col++] = c;
    }
    if (nlines == 0)
        nlines = 1;
    cur_r = cur_c = top = left = 0;
}

/* Write the note out. Returns 1 on success. A failure is not worth a
 * message of its own - the status line reads "not saved" and the
 * note stays on screen to be copied out by hand. */
static int save_note(void)
{
    int fd = open(SAVE_PATH, O_WRONLY | O_CREAT | O_TRUNC);
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

/* --- editing -------------------------------------------------------- */

/* Keep the caret in the text: on a row that exists, inside it, and on
 * a row that does not, which is the empty row at the end. */
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

/* Scroll so the caret is on screen: upwards first, because a note
 * that has grown wants its end in view, and then sideways, so the
 * caret's column is a visible one. */
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

static void insert_char(char c)
{
    int len = (int)strlen(lines[cur_r]);
    if (len >= COLS)
        return;                 /* a row that cannot grow: no wrap */
    for (int i = len; i > cur_c; --i)
        lines[cur_r][i + 1] = lines[cur_r][i];
    lines[cur_r][cur_c] = c;
    lines[cur_r][len + 1] = 0;
    ++cur_c;
    dirty = 1;
}

static void split_line(void)
{
    if (nlines >= MAX_LINES)
        return;
    int len = (int)strlen(lines[cur_r]);
    int tail = len - cur_c;
    for (int r = nlines; r > cur_r + 1; --r)
        for (int i = 0; i <= COLS; ++i)
            lines[r - 1][i] = lines[r - 2][i];
    ++nlines;
    for (int i = 0; i < tail; ++i)
        lines[cur_r + 1][i] = lines[cur_r][cur_c + i];
    lines[cur_r + 1][tail] = 0;
    lines[cur_r][cur_c] = 0;
    cur_r++;
    cur_c = 0;
    dirty = 1;
}

/* Join the row below onto this one, which is what Backspace at the
 * start of a row means. */
static void join_line(void)
{
    if (cur_r == 0 || cur_r + 1 >= nlines)
        return;
    int len = (int)strlen(lines[cur_r]);
    int tail = (int)strlen(lines[cur_r + 1]);
    if (len + tail > COLS)
        return;                 /* it would not fit on one row */
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
    /* At the end of a row, Delete takes the row below's first
     * character rather than the line feed, so the note reads as the
     * text it is rather than as a paragraph mark. */
    join_line();
}

/* --- drawing -------------------------------------------------------- */

static void draw(void)
{
    int w = W, h = H;

    vgfx_clear(C_PAPER);

    /* Ruled lines under the text, so a row is somewhere to write on. */
    int rows = (BTN_Y - PAD_TOP) / CH;
    for (int i = 0; i <= rows && PAD_TOP + i * CH < BTN_Y; ++i)
        vgfx_hline(PAD_X, PAD_TOP + i * CH + CH - 1, w - 2 * PAD_X, C_PAPER2);
    /* the margin the lines start from */
    vgfx_vline(PAD_X - 3, PAD_TOP, BTN_Y - PAD_TOP, C_PAPER2);

    /* Title strip: what this is, and where it is kept. */
    vgfx_fill_rect(0, 0, w, PAD_TOP - 4, C_ACCENT);
    vgfx_str(PAD_X, 5, "sticky", C_TXT);

    /* The text. */
    for (int i = 0; i < rows; ++i) {
        int r = top + i;
        if (r >= nlines)
            break;
        int len = (int)strlen(lines[r]);
        for (int c = 0; left + c < len; ++c) {
            int x = PAD_X + c * CW;
            if (x + CW > w - PAD_X)
                break;         /* past the right margin */
            vgfx_char(x, PAD_TOP + i * CH, lines[r][left + c], C_INK);
        }
    }

    /* The caret, at the cursor, when it is on a visible row and the
     * blink is in its "on" half. A caret that blinks at a second a
     * tick is the one timer a windowed task is given (see TICK_BYTE
     * below), so it is a slow one and it is deliberate. */
    if (caret_on && cur_r >= top && cur_r < top + rows) {
        int x = PAD_X + (cur_c - left) * CW;
        int y = PAD_TOP + (cur_r - top) * CH;
        if (x + CW <= w - PAD_X)
            vgfx_fill_rect(x, y + 2, 2, CH - 4, C_INK);
    }

    /* Buttons: save now, and clear the note. */
    vgfx_fill_rect(PAD_X, BTN_Y, 74, BTN_H, C_TITLE);
    vgfx_str(PAD_X + 6, BTN_Y + 1, "save", C_INK);
    vgfx_fill_rect(PAD_X + 82, BTN_Y, 96, BTN_H, C_TITLE);
    vgfx_str(PAD_X + 88, BTN_Y + 1, "clear note", C_INK);

    /* Status line: where the note lives and whether it is written. */
    vgfx_fill_rect(0, h - STATUS_H, w, STATUS_H, C_DIM);
    vgfx_str(PAD_X, h - STATUS_H + 1,
             dirty ? "not saved - saved on the next tick   " : "saved   ", C_TXT);
    vgfx_str(PAD_X + 28 * CW + 6, h - STATUS_H + 1, SAVE_PATH, C_ACCENT);
}

static int hit_button(int x, int y, int* which)
{
    if (y < BTN_Y || y >= BTN_Y + BTN_H)
        return 0;
    if (x >= PAD_X && x < PAD_X + 74) {
        *which = 1;
        return 1;
    }
    if (x >= PAD_X + 82 && x < PAD_X + 82 + 96) {
        *which = 2;
        return 1;
    }
    return 0;
}

static void clear_note(void)
{
    lines[0][0] = 0;
    nlines = 1;
    cur_r = cur_c = top = left = 0;
    dirty = 1;
}

int main(void)
{
    (void)vgfx_canvas(VGFX_W, VGFX_H);
    vgfx_size(&W, &H);
    load_note();
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
                    (void)save_note();
                exit(0);      /* Esc closes the window */
            }
            if (ev.key == 0x06) {
                /* the kernel's one-second heartbeat: the moment to put
                 * a note that has changed on disk, and to blink */
                if (dirty)
                    (void)save_note();
                caret_on = !caret_on;
            } else if (ev.key == '\r' || ev.key == '\n') {
                split_line();
            } else if (ev.key == 0x08 || ev.key == 0x7F) {
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
            } else if (ev.key >= 0x20 && ev.key < 0x7F) {
                insert_char((char)ev.key);
            }
            clamp_caret();
            scroll_to_caret();
            caret_on = 1;
        } else if (ev.type == VGFX_EV_PRESS) {
            int which = 0;
            if (hit_button(ev.x, ev.y, &which) == 0) {
                /* A press in the paper puts the caret where it
                 * landed, in the row it landed in. */
                int r = top + (ev.y - PAD_TOP) / CH;
                int c = left + (ev.x - PAD_X) / CW;
                if (r >= 0 && r < nlines) {
                    cur_r = r;
                    int len = (int)strlen(lines[r]);
                    cur_c = c < len ? c : len;
                    caret_on = 1;
                    clamp_caret();
                    scroll_to_caret();
                }
            } else if (which == 1) {
                (void)save_note();
            } else {
                clear_note();
            }
        }
        draw();
        vgfx_flush();
    }
}