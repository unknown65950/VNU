/* vlibc/term.h — terminal geometry and the escape sequences a full-screen
 * program needs.
 *
 * The sequence builders are pure: they format an escape sequence into a
 * caller-supplied buffer and return its length, so this header compiles
 * and behaves identically on a host system, where the same strings can be
 * printed to a real terminal. Only tnu_size() touches VNU, and only
 * because the console geometry lives in the kernel.
 *
 * Usage:
 *     char seq[24];
 *     int n = tnu_goto(seq, sizeof seq, 3, 1);
 *     write(1, seq, n);
 *
 * The console understands the small ANSI subset these helpers emit: CUP
 * (H), ED (J), EL (K) and SGR (m) colors/attributes. Anything else in the
 * stream is ignored, so a program can mix these with plain text.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The VGA text plane as it is configured today. Used as a fallback when
 * tnu_size() cannot reach the kernel. */
#define TNU_ROWS 25
#define TNU_COLS 80

/* Cursor to `row`, `col` (both 1-based, the way a terminal counts).
 * Returns the length written, or 0 if `cap` is too small. */
static inline int tnu_goto(char* buf, unsigned long cap, unsigned row,
                           unsigned col)
{
    unsigned long n = 0;
    unsigned val[2];
    val[0] = row;
    val[1] = col;
    if (cap < 4)
        return 0;
    buf[n++] = 27;
    buf[n++] = '[';
    for (int i = 0; i < 2; ++i) {
        if (i == 1) {
            if (n + 1 >= cap)
                return 0;
            buf[n++] = ';';
        }
        unsigned v = val[i];
        unsigned div = 100;
        int started = 0;
        while (div > 0) {
            unsigned d = (v / div) % 10;
            if (d != 0 || started != 0 || div == 1) {
                if (n + 1 >= cap)
                    return 0;
                buf[n++] = (char)('0' + d);
                started = 1;
            }
            div /= 10;
        }
    }
    if (n + 1 >= cap)
        return 0;
    buf[n++] = 'H';
    return (int)n;
}

/* SGR with raw parameters, e.g. tnu_sgr(seq, cap, "0") to reset, "7" for
 * reverse video, "0;30;47" for black text on a white background. */
static inline int tnu_sgr(char* buf, unsigned long cap, const char* params)
{
    unsigned long n = 0, i = 0;
    if (cap < 4)
        return 0;
    buf[n++] = 27;
    buf[n++] = '[';
    while (params[i] != 0) {
        if (n + 2 >= cap)
            return 0;
        buf[n] = params[i];
        ++n;
        ++i;
    }
    if (n + 2 >= cap)
        return 0;
    buf[n++] = 'm';
    return (int)n;
}

/* Erase from the cursor to the end of the line. */
static inline int tnu_erase_eol(char* buf, unsigned long cap)
{
    if (cap < 3)
        return 0;
    buf[0] = 27;
    buf[1] = '[';
    buf[2] = 'K';
    return 3;
}

/* Clear the whole screen and home the cursor. */
static inline int tnu_clear(char* buf, unsigned long cap)
{
    if (cap < 4)
        return 0;
    buf[0] = 27;
    buf[1] = '[';
    buf[2] = '2';
    buf[3] = 'J';
    return 4;
}

/* Console geometry in character cells, through the `tty_size` syscall.
 * Either out-pointer may be null. Returns 0 on success; when the call
 * fails the TNU_ROWS/TNU_COLS defaults are stored instead, so a caller can
 * always lay out a screen. */
int tnu_size(unsigned* rows, unsigned* cols);

#ifdef __cplusplus
}
#endif
