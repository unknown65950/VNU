#include <vlibc/unistd.h>
#include <vlibc/string.h>

int main(int argc, char** argv, char** envp)
{
    (void)envp;
    for (int i = 1; i < argc; ++i) {
        write(1, argv[i], strlen(argv[i]));
        if (i + 1 < argc)
            write(1, " ", 1);
    }
    write(1, "\n", 1);
    return 0;
}
