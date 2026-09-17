/* rm — remove files or directories (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        we("rm: missing operand\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (unlink(argv[i]) < 0 && rmdir(argv[i]) < 0) {
            we("rm: fail ");
            we(argv[i]);
            we("\n");
        }
    }
    return 0;
}