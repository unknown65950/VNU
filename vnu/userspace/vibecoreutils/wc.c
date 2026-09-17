/* wc — count lines, words and bytes (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    char buf[256];
    for (int a = 1; a < argc || (a == 1 && argc <= 1); ++a) {
        int fd;
        const char* label = 0;
        if (argc <= 1) {
            fd = 0;
            a = argc; /* stop after this single stdin pass */
        } else {
            fd = open(argv[a], O_RDONLY);
            label = argv[a];
            if (fd < 0) {
                we("wc: cannot open ");
                we(argv[a]);
                we("\n");
                continue;
            }
        }
        unsigned long lines = 0, words = 0, bytes = 0;
        int in_word = 0;
        for (;;) {
            long n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            for (long i = 0; i < n; ++i) {
                char c = buf[i];
                ++bytes;
                if (c == '\n')
                    ++lines;
                if (c == ' ' || c == '\t' || c == '\n') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    ++words;
                }
            }
        }
        if (fd != 0)
            close(fd);
        put_uint(lines);
        w(" ");
        put_uint(words);
        w(" ");
        put_uint(bytes);
        if (label) {
            w(" ");
            w(label);
        }
        w("\n");
        if (argc <= 1)
            break;
    }
    return 0;
}