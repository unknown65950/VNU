/* mv — move (rename) a file (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 3) {
        we("mv: usage: mv SRC DST\n");
        return 1;
    }
    /* No rename() in this VFS yet — copy then remove the original. */
    if (copy_file(argv[1], argv[2]) != 0)
        return 1;
    unlink(argv[1]);
    return 0;
}