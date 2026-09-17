#include <vlibc/unistd.h>

int close(int fd) {
    return (int)syscall(SYS_close, fd);
}
