/* echo — write its arguments to standard output (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        w(argv[i]);
        if (i + 1 < argc)
            w(" ");
    }
    w("\n");
    return 0;
}