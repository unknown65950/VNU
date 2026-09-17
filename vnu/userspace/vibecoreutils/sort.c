/* sort — sort lines of a file (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    int is_stdin;
    int fd = open_or_stdin(argc, argv, 1, &is_stdin);
    if (fd < 0)
        return 1;
    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    for (int i = 1; i < count; ++i) {
        char tmp[LINE_CAP];
        strcpy(tmp, lines[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(lines[j], tmp) > 0) {
            strcpy(lines[j + 1], lines[j]);
            --j;
        }
        strcpy(lines[j + 1], tmp);
    }
    for (int i = 0; i < count; ++i) {
        w(lines[i]);
        w("\n");
    }
    return 0;
}