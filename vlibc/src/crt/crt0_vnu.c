#include <stddef.h>
#include <vlibc/unistd.h>

extern int main(int argc, char** argv, char** envp);

/* The environment the kernel put on the stack is handed to main twice
 * over: once as main's own third parameter, and once here, where it
 * becomes the `environ' a program reaches through <unistd.h>. Without
 * this the block was computed, passed on, and then unreachable — there
 * was nothing in the tree that could ask what the process had been
 * given. Set before main, because a program may read `environ' (or call
 * getenv) from a constructor running before main. */
char** environ;

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
    environ = envp;

    int result = main(argc, argv, envp);
    exit(result);
    for (;;) {
        __asm__ volatile("hlt");
    }
}
