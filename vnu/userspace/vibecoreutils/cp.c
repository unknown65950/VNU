/* cp — copy a file (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 3) {
        we("cp: usage: cp SRC DST\n");
        return 1;
    }
    return copy_file(argv[1], argv[2]) == 0 ? 0 : 1;
}