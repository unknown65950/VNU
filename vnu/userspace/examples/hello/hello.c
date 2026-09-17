#include <vlibc/unistd.h>
#include <vlibc/stdio.h>

int main(int argc, char** argv, char** envp)
{
    (void)envp;
    puts("Hello from VNU userspace!");
    if (argc < 0 || argc > 16) {
        printf("argc = %d (clamped)\n", argc);
        argc = 0;
    } else {
        printf("argc = %d\n", argc);
    }
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i] ? argv[i] : "(null)");
    }
    write(1, "bye\n", 4);
    return 0;
}
