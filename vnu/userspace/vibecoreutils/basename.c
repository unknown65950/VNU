/* basename — strip the directory part of a path (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        we("basename: missing operand\n");
        return 1;
    }
    w(base(argv[1]));
    w("\n");
    return 0;
}