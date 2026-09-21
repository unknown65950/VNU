/*
 * calc — a graphical keypad calculator for the VNU desktop.
 *
 * Renders its own pixel framebuffer (via the vgfx library) into the
 * window's client area and reads mouse/keyboard events off stdin.
 * Arithmetic is classic immediate-execution with no operator
 * precedence (2 + 3 * 4 = 20, not 14).
 *
 * Layout: BS CE C / 7 8 9 / 4 5 6 * 1 2 3 - 0 ± . + and a wide = at
 * the bottom.
 *
 * Keys: digits, '.' '+', '-', '*', '/', '=', '+', '%', Backspace, Esc
 * (close the window).
 */
#include <vlibc/vgfx.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>

/* --- Geometry (client area is VGFX_W x VGFX_H) --- */
#define M 6                 /* window margin around chrome */
#define DISP_X M
#define DISP_Y 6
#define DISP_W (VGFX_W - 2 * M)
#define DISP_H 24
#define BTN_Y 38            /* first button row top edge */
#define BTN_H 18            /* button height */
#define BTN_GAP 4
#define BTN_COLS 4
#define BTN_W ((VGFX_W - 2 * M - (BTN_COLS - 1) * BTN_GAP) / BTN_COLS)
#define NROWS_DIGIT 5
#define NROWS 6             /* 5 key rows + one wide "=" row */

/* One extra pseudo-code per button so the keypad maps straight onto
 * the engine's key handler. 'B' = BS, 'E' = CE, 'R' = C (reset),
 * 'N' = +/- sign toggle. Row 5 is the wide "=". */
static const char keys[NROWS_DIGIT][BTN_COLS] = {
    {'B', 'E', 'R', '%'},
    {'7', '8', '9', '/'},
    {'4', '5', '6', '*'},
    {'1', '2', '3', '-'},
    {'0', 'N', '.', '+'},
};
static const char* labels[NROWS_DIGIT][BTN_COLS] = {
    {"BS", "CE", "C", "%"},
    {"7", "8", "9", "/"},
    {"4", "5", "6", "*"},
    {"1", "2", "3", "-"},
    {"0", "\xF1", ".", "+"},
};

/* --- Calculator engine state --- */
static double acc = 0;
static char pending = 0;      /* '+','-','*','/','%' or 0 */
static char entry[24];        /* raw number currently being typed */
static char disp[24];         /* what the display shows */
static int new_entry = 1;     /* next digit starts a fresh number */
static int error = 0;
static int hover = -1;        /* button pressed by the mouse, -1 if none */

static double parse_num(const char* s)
{
    int neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    else if (*s == '+') ++s;
    double val = 0;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (double)(*s++ - '0');
    if (*s == '.') {
        ++s;
        double m = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += m * (double)(*s++ - '0');
            m *= 0.1;
        }
    }
    return neg ? -val : val;
}

/* Powers of 10 for digit extraction (avoids __divdi3/__moddi3). */
static const double pow10[] = {
    1.0, 10.0, 100.0, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10
};

/* Format v rounded to p decimal places (p = 0..8).
 * Uses only 'long' (32-bit) integer math and doubles, so no
 * __divdi3 / __moddi3 libgcc calls are emitted. */
static void fmt_p(double v, int p, char* out)
{
    if (v < 0) {
        *out++ = '-';
        v = -v;
    }

    /* Round to p decimal places. */
    double scale = pow10[p];
    v += 0.5 / scale;

    /* Integer part: extract digits left-to-right using pow10. */
    double intv = v / scale;                       /* integer portion as double */
    int nd = 1;
    while (intv >= pow10[nd]) nd++;
    for (int i = nd - 1; i >= 0; i--) {
        long d = (long)(intv / pow10[i]);
        *out++ = (char)('0' + d);
        intv -= (double)d * pow10[i];
    }

    /* Fractional part: p digits, zero-padded. */
    if (p > 0) {
        *out++ = '.';
        long I = (long)(v / scale);               /* safe: I < 9e8 for p ≥ 1 */
        long F = (long)(v - (double)I * scale + 0.5);
        for (int i = p - 1; i >= 0; i--) {
            long d = F / (long)pow10[i];           /* 32-bit div, no libgcc */
            *out++ = (char)('0' + d);
            F -= d * (long)pow10[i];
        }
    }
    *out = 0;
}

/* Format v with the fewest decimal places that reproduce it (so 2.5
 * prints "2.5" and 2.0 prints "2"), up to 8 decimals. */
static void fmt(double v, char* out)
{
    if (v != v || v > 9e9 || v < -9e9) {
        strcpy(out, "Error");
        return;
    }
    if (v == 0) {
        strcpy(out, "0");
        return;
    }
    char best[40] = {0};
    for (int p = 0; p <= 8; ++p) {
        char t[40];
        fmt_p(v, p, t);
        double back = parse_num(t);
        double d = back - v;
        if (d < 0) d = -d;
        double tol = (v < 0 ? -v : v) * 1e-9 + 1e-12;
        if (d <= tol) {
            strcpy(out, t);
            return;
        }
        strcpy(best, t);
    }
    strcpy(out, best);
}

static void set_disp(const char* s)
{
    int i = 0;
    while (s[i] && i < 23) {
        disp[i] = s[i];
        ++i;
    }
    disp[i] = 0;
}

static void entry_init(void)
{
    entry[0] = '0';
    entry[1] = 0;
}

static void reset_all(void)
{
    acc = 0;
    pending = 0;
    entry_init();
    set_disp(entry);
    new_entry = 1;
    error = 0;
}

static void set_result(double v)
{
    fmt(v, entry);
    /* Full-width result becomes the display; 21 chars max. */
    new_entry = 1;
    if (strlen(entry) > 21)
        set_disp("Error");
    else
        set_disp(entry);
    error = (entry[0] == 'E');
}

static double apply(double a, double b, char op)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0)
            return 1e300; /* sentinel → Error on the next display */
        return a / b;
    case '%':
        /* percentage of the accumulator's side, like the classic
         * windows calc: a [op] b % → a [op] (a*b/100). */
        return a * b / 100.0;
    default:
        return a + b;
    }
}

static void handle_digit(char d)
{
    if (error)
        reset_all();
    if (new_entry) {
        entry_init();
        if (d == '0') {
            set_disp(entry);
            return;
        }
        new_entry = 0;
        entry[0] = d;
        entry[1] = 0;
        set_disp(entry);
        return;
    }
    if (strlen(entry) >= 20)
        return;
    int n = (int)strlen(entry);
    entry[n] = d;
    entry[n + 1] = 0;
    set_disp(entry);
}

static void handle_dot(void)
{
    if (error)
        reset_all();
    if (new_entry) {
        entry[0] = '0';
        entry[1] = '.';
        entry[2] = 0;
        new_entry = 0;
        set_disp(entry);
        return;
    }
    for (int i = 0; entry[i]; ++i)
        if (entry[i] == '.')
            return;
    int n = (int)strlen(entry);
    if (n >= 20)
        return;
    entry[n] = '.';
    entry[n + 1] = 0;
    set_disp(entry);
}

static void handle_op(char op)
{
    if (error)
        reset_all();
    double v = parse_num(entry);
    if (pending && !new_entry) {
        acc = apply(acc, v, pending);
        if (acc > 9e9 || acc < -9e9) {
            error = 1;
            acc = 0;
            pending = 0;
            set_disp("Error");
            new_entry = 1;
            return;
        }
    } else {
        acc = v;
    }
    pending = op;
    new_entry = 1;
    fmt(acc, entry);
    set_disp(entry);
}

static void handle_equal(void)
{
    if (error || !pending)
        return;
    double v = parse_num(entry);
    acc = apply(acc, v, pending);
    pending = 0;
    set_result(acc);
}

static void handle_percent(void)
{
    if (error)
        reset_all();
    double v = parse_num(entry);
    set_result(v / 100.0);
}

static void handle_negate(void)
{
    if (error)
        reset_all();
    if (new_entry) {
        double v = parse_num(entry);
        set_result(v == 0 ? 0 : -v);
        return;
    }
    if (entry[0] == '-') {
        for (int i = 0; entry[i]; ++i)
            entry[i] = entry[i + 1];
    } else if (strlen(entry) < 20) {
        for (int i = (int)strlen(entry) + 1; i > 0; --i)
            entry[i] = entry[i - 1];
        entry[0] = '-';
    }
    set_disp(entry);
}

static void handle_backspace(void)
{
    if (error || new_entry)
        return;
    int n = (int)strlen(entry);
    if (n <= 1) {
        entry_init();
    } else {
        entry[--n] = 0;
    }
    set_disp(entry);
}

static void handle_key(char k)
{
    if (k >= '0' && k <= '9')
        handle_digit(k);
    else if (k == '.')
        handle_dot();
    else if (k == '+' || k == '-' || k == '*' || k == '/')
        handle_op(k);
    else if (k == '=')
        handle_equal();
    else if (k == '%')
        handle_percent();
    else if (k == 'N')
        handle_negate();
    else if (k == 'B')
        handle_backspace();
    else if (k == 'E')
        reset_all();      /* CE: clear entry — same reset for simplicity */
    else if (k == 'R')
        reset_all();
}

/* --- Drawing --- */
static void draw_button(int x, int y, int w, int h, const char* label, int pressed)
{
    vgfx_fill_rect(x, y, w, h, pressed ? VGFX_DGRAY : VGFX_LGRAY);
    vgfx_rect(x, y, w, h, VGFX_BLACK);
    int tw = vgfx_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    vgfx_str(tx, ty, label, VGFX_WHITE);
}

static void draw(void)
{
    vgfx_clear(VGFX_LGRAY);

    /* Display: black LCD field, right-aligned white value, pending-op
     * indicator in the corner. */
    vgfx_fill_rect(DISP_X, DISP_Y, DISP_W, DISP_H, VGFX_BLACK);
    vgfx_rect(DISP_X, DISP_Y, DISP_W, DISP_H, VGFX_LGRAY);
    if (pending) {
        char pstr[2] = {pending, 0};
        vgfx_str(DISP_X + 4, DISP_Y + (DISP_H - 16) / 2, pstr, VGFX_WHITE);
    }
    int tw = vgfx_text_width(disp);
    vgfx_str(DISP_X + DISP_W - 4 - tw, DISP_Y + (DISP_H - 16) / 2, disp, VGFX_WHITE);

    /* Buttons. Row 5 is the wide "=". */
    int id = 0;
    for (int r = 0; r < NROWS_DIGIT; ++r) {
        for (int c = 0; c < BTN_COLS; ++c, ++id) {
            int x = M + c * (BTN_W + BTN_GAP);
            int y = BTN_Y + r * (BTN_H + BTN_GAP);
            draw_button(x, y, BTN_W, BTN_H, labels[r][c], hover == id);
        }
    }
    int eq_y = BTN_Y + NROWS_DIGIT * (BTN_H + BTN_GAP);
    int eq_w = BTN_COLS * BTN_W + (BTN_COLS - 1) * BTN_GAP;
    draw_button(M, eq_y, eq_w, BTN_H, "=", hover == id);
}

/* Map client-area pixel coords to a button id, -1 if none. */
static int hit_button(int px, int py)
{
    if (px < M || px >= VGFX_W - M)
        return -1;
    int eq_y = BTN_Y + NROWS_DIGIT * (BTN_H + BTN_GAP);
    if (py >= eq_y && py < eq_y + BTN_H)
        return NROWS_DIGIT * BTN_COLS; /* the wide = */
    if (py < BTN_Y)
        return -1;
    int r = (py - BTN_Y) / (BTN_H + BTN_GAP);
    if (r >= NROWS_DIGIT)
        return -1;
    int c = (px - M) / (BTN_W + BTN_GAP);
    if (c >= BTN_COLS)
        return -1;
    return r * BTN_COLS + c;
}

static void press_at(int px, int py)
{
    int id = hit_button(px, py);
    hover = id;
    draw();
    vgfx_flush();
}

static void release_at(int px, int py)
{
    int id = hit_button(px, py);
    int fired = (id >= 0 && id == hover) ? id : -1;
    hover = -1;
    draw();
    vgfx_flush();
    if (fired < 0)
        return;
    if (fired == NROWS_DIGIT * BTN_COLS) {
        handle_key('=');
    } else {
        char k = keys[fired / BTN_COLS][fired % BTN_COLS];
        if (k)
            handle_key(k);
    }
    draw();
    vgfx_flush();
}

int main(void)
{
    reset_all();
    draw();
    vgfx_flush();

    for (;;) {
        vgfx_event_t ev;
        vgfx_poll(&ev);
        if (ev.type == VGFX_EV_KEY) {
            if (ev.key == 0x1B) {
                exit(0); /* Esc closes the calculator window */
            }
            if (ev.key == '\n' || ev.key == '\r')
                handle_key('=');
            else if (ev.key == 0x7F || ev.key == '\b')
                handle_key('B');
            else
                handle_key(ev.key);
            draw();
            vgfx_flush();
        } else if (ev.type == VGFX_EV_PRESS) {
            press_at(ev.x, ev.y);
        } else if (ev.type == VGFX_EV_RELEASE) {
            release_at(ev.x, ev.y);
        }
    }
}