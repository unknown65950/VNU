#include <vlibc/unistd.h>

char* getcwd(char* buffer, unsigned long size) {
    long result = syscall(SYS_getcwd, buffer, size);

    if (result < 0) {
        return (void*)0;
    }

    return buffer;
}
