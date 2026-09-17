/* clear — clear the terminal screen (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < 40; ++i)
        w("\n");
    return 0;
}