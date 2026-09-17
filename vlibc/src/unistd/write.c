#include <vlibc/unistd.h>

long write(int fd, const void* buffer, unsigned long count) {
    return syscall(SYS_write, fd, buffer, count);
}
