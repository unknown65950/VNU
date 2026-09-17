#include <vlibc/unistd.h>

long read(int fd, void* buffer, unsigned long count) {
    return syscall(SYS_read, fd, buffer, count);
}
