/* touch — create an empty file (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_CREAT);
        if (fd >= 0)
            close(fd);
    }
    return 0;
}