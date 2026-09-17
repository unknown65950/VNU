/* grep — print lines matching a pattern (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        we("grep: missing pattern\n");
        return 1;
    }
    const char* pattern = argv[1];
    int is_stdin;
    int fd = open_or_stdin(argc, argv, 2, &is_stdin);
    if (fd < 0)
        return 1;
    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    int found = 1;
    for (int i = 0; i < count; ++i) {
        if (contains(lines[i], pattern)) {
            w(lines[i]);
            w("\n");
            found = 0;
        }
    }
    return found;
}