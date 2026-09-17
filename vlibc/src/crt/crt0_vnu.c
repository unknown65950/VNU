#include <stddef.h>
#include <vlibc/unistd.h>

extern int main(int argc, char** argv, char** envp);

void _start_c(long* stack)
{
    int argc = 0;
    char** argv = (char**)0;
    char** envp = (char**)0;

    if (stack) {
        long raw = stack[0];
        /* Guard against garbage stack (e.g. old enter_user push bug). */
        if (raw >= 0 && raw <= 64) {
            argc = (int)raw;
            argv = (char**)&stack[1];
            envp = argv + argc + 1;
        }
    }

    int result = main(argc, argv, envp);
    exit(result);
    for (;;) {
        __asm__ volatile("hlt");
    }
}
