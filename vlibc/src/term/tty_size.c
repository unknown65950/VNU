#include <vlibc/term.h>
#include <vlibc/sys/syscall.h>

int tnu_size(unsigned* rows, unsigned* cols)
{
    unsigned r = TNU_ROWS, c = TNU_COLS;
    if (syscall(SYS_tty_size, &r, &c) != 0) {
        /* No console behind the call (or a kernel without the syscall):
         * the caller still gets a usable screen to lay out. */
        r = TNU_ROWS;
        c = TNU_COLS;
    }
    if (r < 4)
        r = 4; /* room for a status line plus a little text */
    if (c < 20)
        c = 20;
    if (rows)
        *rows = r;
    if (cols)
        *cols = c;
    return 0;
}
