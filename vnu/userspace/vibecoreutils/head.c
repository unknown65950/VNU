/* head — print the first lines of a file (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    int n = 10;
    int file_arg = 1;
    if (argc > 2 && argv[1][0] == '-') {
        n = atoi(argv[1] + 1);
        if (n <= 0)
            n = 10;
        file_arg = 2;
    }
    int is_stdin;
    int fd = open_or_stdin(argc, argv, file_arg, &is_stdin);
    if (fd < 0)
        return 1;
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