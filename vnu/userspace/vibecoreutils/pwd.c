/* pwd — print the working directory (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    char buf[128];
    if (!getcwd(buf, sizeof(buf))) {
        we("pwd: fail\n");
        return 1;
    }
    w(buf);
    w("\n");
    return 0;
}