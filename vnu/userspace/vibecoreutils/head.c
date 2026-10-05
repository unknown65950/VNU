/* head — print the first lines (or bytes) of a file (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    int n = 10;
    int bytes = 0;
    int file_arg = 1;
    /* -N, -n N and -c N, each of them with the number either attached
     * (-n10, as POSIX writes it) or as the next argument. */
    while (file_arg < argc && argv[file_arg][0] == '-' &&
           argv[file_arg][1] != 0) {
        const char* a = argv[file_arg];
        const char* num;
        char which;
        /* `--' ends the options, which is the only way to reach a file
         * whose name starts with a dash - and that is not a corner case
         * here, because `head -n5' is how you ask for five lines and
         * `-n5' is a perfectly good file name. Without this the loop
         * reads `--' as a plain -N, hands atoi("-") its one dash, gets 0
         * back and complains "invalid number: --" about the separator
         * itself. */
        if (a[1] == '-' && a[2] == 0) {
            ++file_arg;
            break;
        }
        if (a[1] == 'n' || a[1] == 'c') {
            which = a[1];
            num = a + 2;
        } else {
            which = 'n'; /* a plain -N is -n N */
            num = a + 1;
        }
        int v;
        if (num[0] != 0) {
            v = atoi(num);
            ++file_arg;
        } else if (file_arg + 1 < argc) {
            v = atoi(argv[file_arg + 1]);
            file_arg += 2;
        } else {
            we("head: option needs a number: ");
            we(a);
            we("\n");
            return 1;
        }
        if (v <= 0) {
            we("head: invalid number: ");
            we(a);
            we("\n");
            return 1;
        }
        if (which == 'c')
            bytes = v;
        n = v;
    }

    int is_stdin;
    int fd = open_or_stdin(argc, argv, file_arg, &is_stdin);
    if (fd < 0)
        return 1;

    if (bytes) {
        /* Bytes, not lines: the input is a picture as often as it is
         * text, so this half writes what it read without caring what
         * is in it - no line buffer, no stopping at a newline. */
        static char buf[4096];
        int left = bytes;
        while (left > 0) {
            int want = left < (int)sizeof(buf) ? left : (int)sizeof(buf);
            int r = (int)read(fd, buf, (unsigned long)want);
            if (r <= 0)
                break;
            int off = 0;
            while (off < r) {
                int wn = (int)write(1, buf + off, (unsigned long)(r - off));
                if (wn <= 0)
                    break;
                off += wn;
            }
            left -= r;
        }
        if (!is_stdin)
            close(fd);
        return 0;
    }

    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    if (n > count)
        n = count;
    for (int i = 0; i < n; ++i) {
        w(lines[i]);
        w("\n");
    }
    return 0;
}