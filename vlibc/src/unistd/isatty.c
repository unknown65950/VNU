#include <vlibc/unistd.h>

int isatty(int fd) {
    return (int)syscall(SYS_isatty, fd);
}
