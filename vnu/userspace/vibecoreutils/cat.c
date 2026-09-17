/* cat — concatenate files and print (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    char buf[128];
    if (argc < 2) {
        for (;;) {
            long n = read(0, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(1, buf, (unsigned long)n);
        }
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            we("cat: cannot open ");
            we(argv[i]);
            we("\n");
            continue;
        }
        for (;;) {
            long n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(1, buf, (unsigned long)n);
        }
        close(fd);
    }
    return 0;
}