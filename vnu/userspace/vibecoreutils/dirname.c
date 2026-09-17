/* dirname — strip the final component of a path (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        we("dirname: missing operand\n");
        return 1;
    }
    char buf[256];
    strncpy(buf, argv[1], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    int n = (int)strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    while (n > 0 && buf[n - 1] != '/')
        --n;
    if (n == 0) {
        w(".\n");
    } else {
        while (n > 1 && buf[n - 1] == '/')
            --n;
        buf[n] = 0;
        w(buf);
        w("\n");
    }
    return 0;
}